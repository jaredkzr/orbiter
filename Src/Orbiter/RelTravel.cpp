// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Relativistic travel: the trip autopilot (see RelTravel.h)
// =======================================================================

#include "RelTravel.h"
#include "BlackHole.h"
#include "Orbiter.h"
#include "Vessel.h"
#include "Celbody.h"
#include "Psys.h"
#include "Astro.h"
#include "Maneuver.h"
#include "Log.h"
#include "StarTargets.h"
#include "Star.h"
#include <algorithm>
#include <cmath>

extern Orbiter *g_pOrbiter;
extern TimeData td;
extern PlanetarySystem *g_psys;
extern Vessel *g_focusobj;

using namespace relativity;

namespace reltravel {

	namespace {
		const double ALIGN_LIMIT = 2.0 * RAD;  // attitude error of full drive
		const double CUT_LIMIT = 10.0 * RAD;   // attitude error beyond which the drive is off
		const double EPS_TAU = 1e-4;           // [s] a burn shorter than this is done
		const int MAX_FAILS = 100;             // steps without a trip before giving up
		const double EVENT_FRAMES = 30.0;      // time warp lowered when a burn's end is fewer steps ahead
		const double RESTORE_FRAMES = 100.0;   // a ship-time warp lowered for an event raised back when the next is more steps ahead (x10)
		const double IDEAL_TURN_RATE = 5.0 * RAD; // [rad/s] turns without attitude thrusters
		const double HOLD_TURN_RATE = 30.0 * RAD; // [rad/s] holding (falling while it turns)
		const double MAX_RCS_STEP = 10.0;      // [s] longer steps on the vessel's clock: turns without attitude thrusters
		const double FLIP_COAST = 10.0;        // [s] coast planned between the burns (the drive reverses: no turn; a margin)
		const double REVERSE_HYST = 0.5;       // the drive's end changes when the other is this much nearer (cosine)
		const double FREEZE_FRAMES = 3.0;      // the guidance is held over the last steps
		const double DRIVE_MARGIN = 1.25;      // the guidance may ask this much more than the set acceleration
		const double LAT_MAX = 0.2;            // braking: sideways corrections at most this part of the deceleration
		const double LAT_FADE = 10.0;          // [s] braking: sideways corrections fade out to this time before the stop
		const double BRAKE_START = 0.995;      // braking starts when the stop asks this much of the drive
		const double HOLD_MIN = 1e-7;          // coasting: gravity cancelled above this part of the drive,
		const double HOLD_LEAD = 300.0;        // [s] until this long before the braking
		const double STOP_DIST = 10.0;         // [m] arrived when this close to the point,
		const double STOP_SPEED = 1.0;         // [m/s] and this slow relative to it
		const double REPLAN_MISS = 1000.0;     // [m] stopped farther from the arrival: a new approach
		const int MAX_REPLANS = 5;
		const double HOLD_CATCH = 1e-3;        // (at rest by a star: stopped this part of the distance short, held and drawn there)
		const double REPLAN_BRAKE = 0.01;      // (braking at less than this part of the drive)
		const double BH_ARRIVE_R = 20.0;       // [M] arrival by a black hole: at least this far out,
		const double BH_HOVER_SHARE = 0.25;    // and where staying at rest takes at most this part of the drive
		const double HOLD_TIME = 10.0;         // [s] holding at rest: the position's correction time
		const double HOLD_TIME_STAR = 30.0;    // [s] (by a star)
		const double TRIM_TIME = 2.0;          // [s] an orbit by a black hole trimmed to circular: the velocity's correction time,
		const double TRIM_TOL = 1e-5;          // until within this part of the orbital speed,
		const double TRIM_MAX = 120.0;         // [s] or for at most this long

		/// Arrival later than the soonest (T0 from now) by this, for a coast
		/// of FLIP_COAST between the burns: the coast grows as sqrt (2 T0 dT)
		double FlipMargin (double T0)
		{
			return std::clamp (FLIP_COAST * FLIP_COAST / (2.0 * std::max (T0, 1.0)), 0.5, FLIP_COAST);
		}

		/// Whether a straight burn from x along dir keeps clear of the bodies:
		/// it passes none closer than it is now, or than its radius and a
		/// margin (from a low orbit: the path must lead outwards). The trip's
		/// target aside (the arrival keeps clear of it).
		bool ClearPath (const Vector &x, const Vector &dir, const Body *target, const Body *&blocker)
		{
			for (size_t i = 0; i < g_psys->nGrav(); i++) {
				const CelestialBody *b = g_psys->GetGravObj ((int)i);
				if (b == target) continue;
				Vector rel (x - b->GPos());
				double s = -dotp (rel, dir);
				if (s <= 0.0) continue; // (heading away)
				double d = (rel + dir * s).length();
				double R = b->Size(), rc = R + std::max (200e3, 0.05 * R);
				if (d < std::min (rel.length(), rc)) { blocker = b; return false; }
			}
			return true;
		}
	}

	const char *PhaseName (int phase)
	{
		static const char *name[] = { "Idle", "Aligning", "Accelerating", "Coasting", "Decelerating", "Arrived", "No trip", "Holding" };
		return phase >= 0 && phase <= PH_HOLD ? name[phase] : "-";
	}

	Controller::Controller (Vessel *v): vessel (v) {}

	void Controller::BodyState (const Body *b, double t, Vector &x, Vector &v)
	{
		x = b->GPos(), v = b->GVel();
		double dt = t - td.SimT0;
		if (dt == 0.0) return;
		const Vessel *vs = dynamic_cast<const Vessel*> (b);
		const CelestialBody *p = b->ElRef();
		if ((vs && vs->Relativistic()) || !p || p == b) { x += v * dt; return; }
		Vector r0 (x - p->GPos()), v0 (v - p->GVel()), r, vv;
		double mu = Ggrav * (p->Mass() + (vs ? 0.0 : b->Mass()));
		if (!maneuver::Kepler (r0, v0, mu, dt, r, vv)) r = r0 + v0 * dt, vv = v0;
		Vector px, pv;
		BodyState (p, t, px, pv);
		x = px + r, v = pv + vv;
	}

	bool Controller::Resolve ()
	{
		if (!settings.target && settings.star < 0 && !settings.targetName.empty()) {
			const std::string &n = settings.targetName;
			if (!n.compare (0, 5, "star:")) {
				settings.star = startargets::Get ().Find (n.c_str() + 5);
				if (const Star *b = StarBody (settings.star)) settings.target = b, settings.star = -1; // (saved before it was a body)
			}
			else settings.target = g_psys->GetObj (n.c_str(), true);
			if (HasTarget ()) settings.targetName.clear();
		}
		if (settings.star >= 0) return settings.star < (int)startargets::Get ().Stars ().size ();
		return settings.target && settings.target != vessel;
	}

	std::string Controller::TargetName () const
	{
		if (settings.star >= 0 && settings.star < (int)startargets::Get ().Stars ().size ())
			return startargets::Get ().Stars ()[settings.star].name;
		return settings.target ? settings.target->Name () : settings.targetName;
	}

	void Controller::TargetState (double t, Vector &x, Vector &v) const
	{
		if (settings.star >= 0) startargets::Get ().State (settings.star, t, x, v);
		else if (settings.target) BodyState (settings.target, t, x, v);
		else x = v = Vector ();
	}

	void Controller::SetTarget (const Body *b)
	{
		settings.target = b, settings.star = -1, settings.targetName.clear ();
		if (settings.altitude > 1e12) settings.altitude = 1e6; // (from a star's distance)
		if (b && b->Type () == OBJTP_BLACKHOLE) {
			// (where the drive can hold the vessel: Newton's GM/r^2, a share of
			// the drive's acceleration; out of the strongest field)
			const kerr::Hole &K = ((const BlackHole*)b)->Kerr ();
			double r = std::max (BH_ARRIVE_R * K.M, std::sqrt (Ggrav * b->Mass () / (BH_HOVER_SHARE * settings.accel)));
			settings.altitude = r - b->Size ();
		}
	}

	Vector Controller::Gravity (const Vector &x, const Vector &v) const
	{
		if (const BlackHole *bh = vessel->StrongFieldHole ())
			return bh->FreeAccel (x - bh->GPos (), v - bh->GVel ());
		return g_psys->Gacc (x, vessel);
	}

	void Controller::Trim ()
	{
		// the velocity of the circular orbit through where the vessel is (in
		// the equatorial plane: Kerr's), the difference taken out over
		// TRIM_TIME; done when close enough
		const BlackHole *bh = vessel->StrongFieldHole ();
		Vector rp, rv, vc;
		char buf[128];
		snprintf (buf, sizeof (buf), "Arrived at %s", TargetName ().c_str());
		if (bh) rp = vessel->GPos () - bh->GPos (), rv = vessel->GVel () - bh->GVel ();
		if (!bh || !bh->CircularVelocity (rp, vc)) { trimming = false; Stop (PH_ARRIVED, buf); return; }
		Vector dv (vc - rv);
		if (dv.length () < TRIM_TOL * vc.length () || td.SimT0 - trimStart > TRIM_MAX) {
			trimming = false;
			Stop (PH_ARRIVED, buf);
			return;
		}
		Vector alpha (dv / std::max (TRIM_TIME, td.SimDT));
		double amax = std::min (DRIVE_MARGIN * settings.accel, Vessel::RELDRIVE_MAX_ACCEL);
		if (alpha.length () > amax) alpha *= amax / alpha.length ();
		attErr = Point (alpha.unit ());
		Drive (alpha, td.SimT1 + 1.0);
		phase = PH_BURN2;
		message = "Trimming the orbit";
	}

	void Controller::Hold ()
	{
		// at rest relative to the hole, at the arrival point: the drive's
		// acceleration of an observer at rest there, and corrections back to
		// the point (critically damped over HOLD_TIME); by a star: against
		// its gravity (Newton's)
		const BlackHole *bh = vessel->StrongFieldHole ();
		const Body *tb = bh ? (const Body*)bh : settings.target;
		if (!tb || vessel->InsideHorizon ()) { Stop (PH_FAILED, "Can't hold position"); return; }
		Vector rp (vessel->GPos () - tb->GPos ()), rv (vessel->GVel () - tb->GVel ());
		if (!holdSet) holdPos = rp, holdSet = true;
		Vector hover;
		if (!bh) hover = -Gravity (vessel->GPos (), vessel->GVel ());
		else if (!bh->HoverAccel (rp, hover)) { Stop (PH_FAILED, "Can't hold position: within the ergosphere"); return; }
		// (by a star the hover is weak, the corrections would swing the
		// thrust about faster than the vessel turns: gentler, and at most
		// half the hover, the thrust within 30 deg of it)
		double w = 2.0 / (bh ? HOLD_TIME : HOLD_TIME_STAR);
		Vector corr ((holdPos - rp) * (w * w) - rv * (2.0 * w));
		if (!bh) {
			// (towards the point at a speed it can stop from with the
			// correction available: sqrt (2 a d), else w d, then matched)
			double cmax = 0.5 * hover.length ();
			Vector dp (holdPos - rp);
			double d = dp.length ();
			Vector vd (d > 0.0 ? dp * (std::min (w * d, std::sqrt (cmax * d)) / d) : Vector ());
			corr = (vd - rv) * (2.0 * w);
			if (corr.length () > cmax) corr *= cmax / corr.length ();
		}
		Vector alpha (hover + corr);
		attErr = Point (alpha.unit ());
		Drive (alpha, td.SimT1 + 1.0);
		char buf[128];
		snprintf (buf, sizeof (buf), "Holding at rest by %s: %.3g g", tb->Name (), alpha.length () / G0);
		message = buf;
	}

	void Controller::SetStar (int i)
	{
		// a star of the world (Config/Systems): the body (its own place, a
		// binary's member's; an orbit about it), at the star's default distance
		if (const Star *b = StarBody (i)) {
			SetTarget (b);
			settings.altitude = std::max (startargets::Get ().ArrivalDistance (i) - b->Size (), b->Size ());
			return;
		}
		settings.target = nullptr, settings.star = i, settings.targetName.clear ();
		if (i >= 0) settings.altitude = startargets::Get ().ArrivalDistance (i);
		if (settings.arrival == ARRIVE_ORBIT) settings.arrival = ARRIVE_REST; // (a point: no orbit)
	}

	const Star *Controller::StarBody (int i)
	{
		for (size_t k = 0; i >= 0 && g_psys && k < g_psys->nStar (); k++)
			if (g_psys->GetStar ((int)k)->CatalogueIndex () == i) return g_psys->GetStar ((int)k);
		return nullptr;
	}

	bool Controller::Solve (Plan &p, double tArr, const Plan *warm)
	{
		p = Plan ();
		p.planned = td.SimT0;
		if (!Resolve ()) return false;
		const Body *tg = settings.target;
		const int star = settings.star;
		relativity::State &s = p.spec.start;
		s.t = td.SimT0, s.tau = 0.0;
		s.x = vessel->GPos(), s.u = UOfVelocity (vessel->GVel());
		p.spec.accel = settings.accel;
		p.spec.mode = settings.arrival == ARRIVE_FLYBY ? TRIP_FLYBY : TRIP_RENDEZVOUS;
		p.spec.tArrive = tArr;
		// (warm start: the last step's plan, the proper time since)
		if (warm && warm->ok) {
			p.spec.warm = &warm->trip;
			p.spec.warmElapsed = (td.SimT0 - warm->planned) / GammaOfU (warm->spec.start.u);
		}

		// arrival: on the side the vessel comes from, at the altitude (a
		// flyby: beside the target, across the approach)
		Vector tx, tv;
		TargetState (td.SimT0, tx, tv);
		Vector a (vessel->GPos() - tx);
		a = a.length() > 1.0 ? a.unit() : Vector (1, 0, 0);
		if (settings.arrival == ARRIVE_FLYBY) {
			Vector n (crossp (a, Vector (0, 1, 0)));
			if (n.length() < 1e-3) n = crossp (a, Vector (1, 0, 0));
			a = n.unit();
		}
		double r = (tg ? tg->Size() : 0.0) + settings.altitude; // (a star: from its centre)
		const CelestialBody *cb = dynamic_cast<const CelestialBody*> (tg);
		bool orbit = settings.arrival == ARRIVE_ORBIT && cb && cb->Mass() > 0.0;
		Vector vorb;
		if (orbit && cb->Type() == OBJTP_BLACKHOLE) {
			// a black hole: in its equatorial plane (circular orbits are
			// there), prograde, at Kerr's orbital speed
			const BlackHole *bh = (const BlackHole*)cb;
			Vector ax (bh->SpinAxis()), q (a - ax * dotp (a, ax));
			a = q.length() > 1e-3 ? q.unit() : crossp (ax, Vector (1, 0, 0)).length() > 1e-3 ? crossp (ax, Vector (1, 0, 0)).unit() : Vector (1, 0, 0);
			if (!bh->CircularVelocity (a * r, vorb)) orbit = false;
		} else if (orbit) {
			// circular, about the rotation axis (equatorial when arriving over the equator)
			Vector t (crossp (cb->RotAxis(), a));
			if (t.length() < 1e-3) t = crossp (Vector (0, 1, 0), a);
			if (t.length() < 1e-3) t = crossp (Vector (1, 0, 0), a);
			vorb = t.unit() * std::sqrt (Ggrav * cb->Mass() / r);
		}
		p.spec.target = [tg, star, a, r, orbit, vorb](double t, Vector &x, Vector &v) {
			if (star >= 0) startargets::Get ().State (star, t, x, v);
			else BodyState (tg, t, x, v);
			x += a * r;
			if (orbit) v += vorb;
		};
		p.trip = SolveTrip (p.spec);
		p.ok = p.trip.ok;
		return p.ok;
	}

	bool Controller::Remaining (double &tSolar, double &tShip) const
	{
		double now = td.SimT0;
		if (Engaged () && guided) {
			if (settings.arrival == ARRIVE_FLYBY) {
				tSolar = tArrive - now;
				tShip = tSolar / Gamma (vessel->GVel().length());
				return true;
			}
			// (coasting, then braking at a from proper speed u: u/a, on board (c/a) asinh (u/c))
			double coast = phase == PH_BURN2 ? 0.0 : std::max (0.0, tArrive - now - tStop);
			double a = std::max (aBrake, 1e-9);
			tSolar = coast + tStop;
			tShip = coast / std::sqrt (1.0 + uRel * uRel / (C * C)) + C / a * std::asinh (uRel / C);
			return true;
		}
		const Plan &p = Engaged () ? cur : preview;
		if (!p.ok) return false;
		tSolar = p.trip.tArrive - now;
		tShip = p.trip.tauArrive - (now - p.planned) / GammaOfU (p.spec.start.u);
		return true;
	}

	const Plan &Controller::Preview ()
	{
		Solve (preview, settings.fastest ? 0.0 : td.SimT0 + settings.tripTime);
		return preview;
	}

	bool Controller::Engage (bool on)
	{
		if (!on) {
			if (Engaged ()) Stop (PH_IDLE, "Disengaged");
			return true;
		}
		if (!Resolve ()) { message = "No target"; return false; }
		if (!vessel->SetRelDrive (settings.accel, false)) { message = "Refused: docked or attached"; return false; }
		for (int i = NAVMODE_KILLROT; i <= NAVMODE_ANTINORMAL; i++)
			if (i != NAVMODE_HOLDALT) vessel->ClrNavMode (i);
		tArrive = settings.fastest ? 0.0 : td.SimT0 + settings.tripTime;
		phase = PH_ALIGN;
		aligned = guided = stopped = trimming = holdSet = false;
		fails = replans = 0;
		message = "Engaged";
		LOGOUT ("Relativistic travel: %s to %s, %.4g g, %s", vessel->Name(), TargetName ().c_str(),
			settings.accel / G0, settings.fastest ? "as soon as possible" : "at a set time");
		return true;
	}

	void Controller::Abort (const char *msg)
	{
		if (Engaged ()) Stop (PH_FAILED, msg);
	}

	void Controller::Stop (int ph, const char *msg)
	{
		vessel->SetRelDrive (settings.accel, false);
		phase = ph;
		message = msg;
		LOGOUT ("Relativistic travel: %s %s", vessel->Name(), msg);
		if (vessel == g_focusobj) g_pOrbiter->ShowInfoMessage (msg);
	}

	double Controller::Point (const Vector &dir)
	{
		if (!std::isfinite (dir.x + dir.y + dir.z) || dir.length() < 0.5) {
			LOGOUT_WARN ("Relativistic travel: no direction to point at (phase %d)", phase);
			return attErr;
		}
		// (the drive pushes either way along z: the end nearer the direction,
		// kept unless the other is much nearer, so a reversal needs no turn)
		Vector zg (mul (vessel->GRot(), Vector (0, 0, 1)));
		double cz = dotp (zg, dir);
		if (cz < -REVERSE_HYST) driveSign = -1.0;
		else if (cz > REVERSE_HYST) driveSign = 1.0;
		// (-z towards dir: +z away from it; attitude thrusters if they can,
		// else the ideal drive turns the vessel)
		Vector tgt (dir * driveSign);
		if (!IdealTurns ()) return vessel->SteerTowards (tgt, Vector (0, 0, 1));
		return vessel->TurnTowards (tgt, Vector (0, 0, 1), (phase == PH_HOLD ? HOLD_TURN_RATE : IDEAL_TURN_RATE) * td.SimDT);
	}

	bool Controller::IdealTurns () const
	{
		// (attitude thrusters can't hold an attitude over steps this long on
		// the vessel's clock; under a ship-time warp the trip is the drive's
		// throughout, the final corrections too)
		// (holding by a black hole, the drive's thousands of g want the
		// attitude exact: a hundredth of a degree off pushes sideways at tens
		// of m/s^2)
		double shipStep = td.SimDT / Gamma (vessel->GVel().length());
		return !vessel->AttitudeThrustersUsable () || shipStep > MAX_RCS_STEP || g_pOrbiter->ShipWarp () > 0.0 || phase == PH_HOLD;
	}

	void Controller::Drive (const Vector &alpha, double tEnd)
	{
		if (!std::isfinite (alpha.x + alpha.y + alpha.z + tEnd)) {
			LOGOUT_WARN ("Relativistic travel: no drive command (phase %d, guided %d)", phase, (int)guided);
			vessel->SetRelDrive (settings.accel, false);
			return;
		}
		double level = attErr <= ALIGN_LIMIT ? 1.0 : attErr >= CUT_LIMIT ? 0.0 : (CUT_LIMIT - attErr) / (CUT_LIMIT - ALIGN_LIMIT);
		if (level > 0.0 && alpha.length() > 0.0) {
			vessel->SetRelDrive (driveSign * std::min (alpha.length() * level, Vessel::RELDRIVE_MAX_ACCEL), true);
			if (tEnd < td.SimT1) vessel->SetRelDriveCutoff (std::max (tEnd, td.SimT0));
		} else {
			vessel->SetRelDrive (settings.accel, false);
		}
	}

	void Controller::LimitWarp (double tEvent)
	{
		// time warp: lowered ahead of an event (a burn's end, the arrival), and
		// while turning far. A ship-time warp lowered for an event is raised
		// back once the next is far (a trip of years spans many events' worth
		// of steps); one the user changed is left as set.
		double w = td.Warp();
		double frames = (tEvent - td.SimT0) / std::max (td.SimDT, 1e-9);
		double sw = g_pOrbiter->ShipWarp ();
		if (sw != warpLastSet) warpRestore = 0.0; // (the user's)
		if (w > 1.0 && frames < EVENT_FRAMES) {
			// (to the event in EVENT_FRAMES steps: approached geometrically)
			if (sw > 0.0 && warpRestore <= 0.0) warpRestore = sw;
			g_pOrbiter->CapWarp (std::max (1.0, w * std::max (0.1, frames / EVENT_FRAMES)));
		} else if (w > 10.0 && attErr > 5.0 * RAD && !IdealTurns ()) {
			if (sw > 0.0 && warpRestore <= 0.0) warpRestore = sw;
			g_pOrbiter->CapWarp (10.0);
		} else if (sw > 0.0 && warpRestore > sw && frames > 10.0 * RESTORE_FRAMES) {
			g_pOrbiter->SetShipWarp (std::min (warpRestore, sw * 10.0));
		}
		if (warpRestore > 0.0 && g_pOrbiter->ShipWarp () >= warpRestore) warpRestore = 0.0;
		warpLastSet = g_pOrbiter->ShipWarp ();
	}

	void Controller::Update ()
	{
		if (!Engaged ()) return;
		if (!Resolve ()) { Stop (PH_FAILED, "No target"); return; }
		double now = td.SimT0;

		// arrived (a flyby: at the time; a rendezvous: braked to rest, Guide)
		if (phase == PH_HOLD) { Hold (); return; }
		if (trimming) { Trim (); return; }
		if (guided && now >= tArrive - 1e-6 && (settings.arrival == ARRIVE_FLYBY || stopped)) {
			char buf[128];
			snprintf (buf, sizeof (buf), "Arrived at %s", TargetName ().c_str());
			// (at rest by a black hole, or a star (its gravity: falling in
			// else): held there)
			const BlackHole *bh = settings.target && settings.target->Type () == OBJTP_BLACKHOLE ? (const BlackHole*)settings.target : nullptr;
			bool star = settings.target && settings.target->Type () == OBJTP_STAR;
			if (settings.arrival == ARRIVE_REST && ((bh && vessel->StrongFieldHole () == bh) || star)) {
				LOGOUT ("Relativistic travel: %s %s: holding there", vessel->Name(), buf);
				if (vessel == g_focusobj) g_pOrbiter->ShowInfoMessage (buf);
				phase = PH_HOLD;
				holdPos = vessel->GPos () - settings.target->GPos (), holdSet = true;
				Hold ();
				return;
			}
			// (in orbit about a black hole: trimmed to its circular orbit there)
			if (bh && settings.arrival == ARRIVE_ORBIT && vessel->StrongFieldHole () == bh) {
				trimming = true, trimStart = now;
				Trim ();
				return;
			}
			Stop (PH_ARRIVED, buf);
			return;
		}
		if (guided) { Guide (); return; }

		// the departure and the first burn: the planned trip (arriving at a
		// fixed time: the user's, or once under way the soonest with a coast
		// between the burns for the turn; if that can't be made any more, as
		// soon as possible)
		Plan p;
		double tArr = tArrive > now ? tArrive : 0.0;
		if (!Solve (p, tArr, &cur) && !(tArr > 0.0 && Solve (p, 0.0, &cur))) {
			// (close to the arrival: guided the rest of the way)
			double ta = tArrive > now ? tArrive : cur.ok ? cur.trip.tArrive : 0.0;
			if (aligned && cur.ok && ta > now && ta - now < std::max (EVENT_FRAMES * td.SimDT, 60.0)) {
				guided = true, arrival = cur.spec.target, tArrive = ta, tStop = ta - now;
				Guide ();
				return;
			}
			vessel->SetRelDrive (settings.accel, false);
			if (++fails > MAX_FAILS) Stop (PH_FAILED, "No trip found");
			return;
		}
		fails = 0;
		cur = p;
		const Trip &tr = cur.trip;
		Vector dir (tr.burns[0].dir);
		double tEnd = TripState (cur.spec, tr, tr.burns[0].dtau).t;

		// the drive less gravity (the plan is free of it), the attitude towards it
		Vector g (Gravity (vessel->GPos(), vessel->GVel()));
		Vector alpha (dir * settings.accel - g);
		attErr = Point (alpha.unit());
		if (!aligned) {
			phase = PH_ALIGN;
			// (a clear departure: not through the body it starts from)
			const Body *blocker = nullptr;
			if (!ClearPath (vessel->GPos(), dir, settings.target, blocker)) {
				message = std::string ("Waiting for a clear departure (") + blocker->Name() + ")";
				vessel->SetRelDrive (settings.accel, false);
				return;
			}
			if (attErr > ALIGN_LIMIT) { message = "Aligning"; vessel->SetRelDrive (settings.accel, false); return; }
			aligned = true;
			message = "Under way";
			// (a rendezvous: the soonest arrival, and a margin, fixed; a flyby
			// at full drive has no slack: solved as soon as possible throughout)
			if (settings.fastest && tr.burns.size() > 1) tArrive = tr.tArrive + FlipMargin (tr.tArrive - now);
		}
		phase = PH_BURN1;
		Drive (alpha, tEnd);
		// (the time warp against the arrival: the burn's end falls within a
		// step exactly, the drive cut off there)
		LimitWarp (std::max (tEnd, tArrive > now ? tArrive : tr.tArrive));
		// the first burn ends in this step: guided from here to the arrival
		if (tEnd <= td.SimT1 || tr.burns[0].dtau <= EPS_TAU) {
			guided = true;
			arrival = cur.spec.target;
			if (tArrive <= now) tArrive = tr.tArrive;
			tStop = tArrive - now;
		}
	}

	void Controller::Guide ()
	{
		// After the first burn, to the arrival state.
		// Rendezvous: braking to the arrival point in the frame moving with
		// the arrival velocity. Constant proper deceleration a stops a vessel
		// of Lorentz factor gamma within c^2 (gamma - 1) / a: the vessel
		// coasts (turned for the braking) until the deceleration this asks of
		// the remaining distance reaches the drive's, then brakes at exactly
		// that, its sideways miss taken out over the stopping time |u|/a
		// (proper speed falls linearly in coordinate time). Exact in
		// relativity, whatever the speed; gravity taken out at each step.
		// Flyby: zero-effort-miss corrections of the position.
		double now = td.SimT0, tgo = tArrive - now;
		bool fly = settings.arrival == ARRIVE_FLYBY;
		// (a rendezvous: the arrival state when the vessel will have stopped)
		if (!fly) tgo = phase == PH_BURN2 ? tStop : std::max (tArrive - now, tStop);
		Vector xt, vt;
		arrival (now + tgo, xt, vt);
		Vector x (vessel->GPos()), v (vessel->GVel());
		Vector g (Gravity (x, v));
		Vector alpha;
		double tEnd = tArrive;
		bool burn;
		if (fly) {
			if (tgo > FREEZE_FRAMES * td.SimDT) {
				Vector zem (xt - (x + v * tgo));
				alpha = ProperOfCoordAccel (zem * (3.0 / (tgo * tgo)), v);
				double amax = std::min (DRIVE_MARGIN * settings.accel, Vessel::RELDRIVE_MAX_ACCEL);
				if (alpha.length() > amax) alpha *= amax / alpha.length();
				lastAlpha = alpha;
			} else alpha = lastAlpha;
			if (alpha.length() < 0.01 * settings.accel) alpha = Vector (); // (no small corrections)
			alpha -= g; // (gravity cancelled: the planned coast is free of it)
			burn = alpha.length() > HOLD_MIN * settings.accel;
		} else {
			// (the arrival point, moving with the arrival velocity: where it is now)
			Vector r (xt - vt * tgo - x);
			Vector u (UOfVelocity (AddVelocity (-vt, v))); // (relative: the arrival frame)
			double us = u.length(), gm1 = GammaMinus1OfU (u), gam = 1.0 + gm1;
			// braking against the velocity (its direction then stays: a steady
			// attitude), the miss across it taken out before the last seconds
			Vector n (us > 0.0 ? u / us : r.unit());
			double D = dotp (r, n);
			Vector side (r - n * D);
			double areq = D > 0.0 ? C * C * gm1 / D : DRIVE_MARGIN * settings.accel;
			double amax = std::min (DRIVE_MARGIN * settings.accel, Vessel::RELDRIVE_MAX_ACCEL);
			// (stopped short: the braking asks next to nothing of the drive, a
			// miss left that it doesn't take out: a fresh trip from here,
			// rather than creeping the rest)
			double miss = r.length();
			// (at rest by a star, stopped a little short: held, drawn to the
			// arrival point; its gravity would spoil a short new approach)
			bool starRest = settings.arrival == ARRIVE_REST && settings.target && settings.target->Type () == OBJTP_STAR;
			if (starRest && phase == PH_BURN2 && miss > STOP_DIST && miss < HOLD_CATCH * settings.altitude && areq < REPLAN_BRAKE * settings.accel) {
				char buf[128];
				snprintf (buf, sizeof (buf), "Arrived at %s", TargetName ().c_str());
				LOGOUT ("Relativistic travel: %s %s (%.3g km short): holding there", vessel->Name(), buf, miss * 1e-3);
				if (vessel == g_focusobj) g_pOrbiter->ShowInfoMessage (buf);
				phase = PH_HOLD, guided = false;
				holdPos = xt - vt * tgo - settings.target->GPos (), holdSet = true;
				Hold ();
				return;
			}
			if (phase == PH_BURN2 && miss > REPLAN_MISS && areq < REPLAN_BRAKE * settings.accel && replans < MAX_REPLANS) {
				replans++;
				LOGOUT ("Relativistic travel: %s stopped %.3g km short of the arrival: a new approach", vessel->Name(), miss * 1e-3);
				guided = stopped = false;
				tArrive = 0.0;
				phase = PH_BURN1;
				cur = Plan (), fails = 0; // (no warm start from the long trip's plan)
				vessel->SetRelDrive (settings.accel, false);
				// (the time warp down to the hop: ~2 sqrt (miss / a) long)
				double hop = 2.0 * std::sqrt (miss / settings.accel);
				g_pOrbiter->CapWarp (std::max (1.0, hop / (3.0 * EVENT_FRAMES * std::max (td.SimDT / std::max (td.Warp (), 1e-9), 1e-3))));
				return;
			}
			burn = phase == PH_BURN2 || areq >= BRAKE_START * settings.accel;
			double a = burn ? std::min (areq, amax) : settings.accel;
			double tstop = us / std::max (a, 1e-9);
			tStop = tstop, uRel = us, aBrake = a;
			double fade = std::clamp ((tstop - LAT_FADE) / LAT_FADE, 0.0, 1.0);
			Vector lat (fade > 0.0 ? side * (2.0 / (tstop * tstop) * gam * gam * fade) : Vector ());
			if (lat.length() > LAT_MAX * a) lat *= LAT_MAX * a / lat.length();
			alpha = -n * a + lat;
			if (alpha.length() > amax) alpha *= amax / alpha.length();
			alpha -= g;
			tEnd = now + tstop;
			// (there, at rest: done, rather than creeping the last metres)
			if (burn && r.length() < STOP_DIST && us < STOP_SPEED) tEnd = now;
		}
		// coasting (a rendezvous before its braking): gravity cancelled, the
		// planned coast is free of it; turned for the braking in its last minutes
		bool hold = false;
		if (!burn && !fly && tArrive - now - tStop > HOLD_LEAD) {
			alpha = -g;
			hold = alpha.length() > HOLD_MIN * settings.accel;
		}
		if (alpha.length() > 0.0) attErr = Point (alpha.unit());
		if (burn) {
			phase = fly ? PH_COAST : PH_BURN2;
			// (at rest by a black hole: no cutoff within the step, the vessel
			// would fall for the rest of it; the braking up to the stop and the
			// hover after it averaged over the step (its velocity change
			// right), held from the next)
			bool holdNext = !fly && settings.arrival == ARRIVE_REST && settings.target && settings.target->Type () == OBJTP_BLACKHOLE;
			if (holdNext && tEnd < td.SimT1) {
				double f = std::clamp ((tEnd - now) / std::max (td.SimDT, 1e-9), 0.0, 1.0);
				alpha = alpha * f - g * (1.0 - f);
				attErr = Point (alpha.unit());
			}
			Drive (alpha, fly ? tArrive : holdNext ? std::max (tEnd, td.SimT1 + 1.0) : tEnd);
			if (!fly && tEnd <= td.SimT1) tArrive = tEnd, stopped = true; // (stops within this step: arrived)
		} else if (hold) {
			phase = PH_COAST;
			Drive (alpha, tArrive);
		} else {
			phase = PH_COAST;
			vessel->SetRelDrive (settings.accel, false);
		}
		LimitWarp (fly ? tArrive : burn ? tEnd : tArrive);
	}

} // namespace reltravel

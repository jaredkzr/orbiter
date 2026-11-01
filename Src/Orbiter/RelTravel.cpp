// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Relativistic travel: the trip autopilot (see RelTravel.h)
// =======================================================================

#include "RelTravel.h"
#include "Orbiter.h"
#include "Vessel.h"
#include "Celbody.h"
#include "Psys.h"
#include "Astro.h"
#include "Maneuver.h"
#include "Log.h"
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
		const double IDEAL_TURN_RATE = 5.0 * RAD; // [rad/s] turns without attitude thrusters
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

		/// Arrival later than the soonest (T0 from now) by this, for a coast
		/// of FLIP_COAST between the burns: the coast grows as sqrt (2 T0 dT)
		double FlipMargin (double T0)
		{
			return std::clamp (FLIP_COAST * FLIP_COAST / (2.0 * std::max (T0, 1.0)), 0.5, FLIP_COAST);
		}

		/// Whether a straight burn from x along dir keeps clear of the bodies:
		/// it passes none closer than it is now, or than its radius and a
		/// margin (from a low orbit: the path must lead outwards)
		bool ClearPath (const Vector &x, const Vector &dir, const Body *&blocker)
		{
			for (size_t i = 0; i < g_psys->nGrav(); i++) {
				const CelestialBody *b = g_psys->GetGravObj ((int)i);
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
		static const char *name[] = { "Idle", "Aligning", "Accelerating", "Coasting", "Decelerating", "Arrived", "No trip" };
		return phase >= 0 && phase <= PH_FAILED ? name[phase] : "-";
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
		if (!settings.target && !settings.targetName.empty()) {
			settings.target = g_psys->GetObj (settings.targetName.c_str(), true);
			if (settings.target) settings.targetName.clear();
		}
		return settings.target && settings.target != vessel;
	}

	bool Controller::Solve (Plan &p, double tArr, const Plan *warm)
	{
		p = Plan ();
		p.planned = td.SimT0;
		if (!Resolve ()) return false;
		const Body *tg = settings.target;
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
		Vector a (vessel->GPos() - tg->GPos());
		a = a.length() > 1.0 ? a.unit() : Vector (1, 0, 0);
		if (settings.arrival == ARRIVE_FLYBY) {
			Vector n (crossp (a, Vector (0, 1, 0)));
			if (n.length() < 1e-3) n = crossp (a, Vector (1, 0, 0));
			a = n.unit();
		}
		double r = tg->Size() + settings.altitude;
		const CelestialBody *cb = dynamic_cast<const CelestialBody*> (tg);
		bool orbit = settings.arrival == ARRIVE_ORBIT && cb && cb->Mass() > 0.0;
		Vector vorb;
		if (orbit) {
			// circular, about the rotation axis (equatorial when arriving over the equator)
			Vector t (crossp (cb->RotAxis(), a));
			if (t.length() < 1e-3) t = crossp (Vector (0, 1, 0), a);
			if (t.length() < 1e-3) t = crossp (Vector (1, 0, 0), a);
			vorb = t.unit() * std::sqrt (Ggrav * cb->Mass() / r);
		}
		p.spec.target = [tg, a, r, orbit, vorb](double t, Vector &x, Vector &v) {
			BodyState (tg, t, x, v);
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
		aligned = guided = stopped = false;
		fails = 0;
		message = "Engaged";
		LOGOUT ("Relativistic travel: %s to %s, %.4g g, %s", vessel->Name(), settings.target->Name(),
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
		if (vessel->AttitudeThrustersUsable ()) return vessel->SteerTowards (tgt, Vector (0, 0, 1));
		return vessel->TurnTowards (tgt, Vector (0, 0, 1), IDEAL_TURN_RATE * td.SimDT);
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
		// time warp: lowered ahead of an event (a burn's end, the arrival), and while turning far
		double w = td.Warp();
		if (w <= 1.0) return;
		double frames = (tEvent - td.SimT0) / std::max (td.SimDT, 1e-9);
		if (frames < EVENT_FRAMES) g_pOrbiter->SetWarpFactor (std::max (1.0, w * 0.1));
		else if (attErr > 5.0 * RAD && w > 10.0) g_pOrbiter->SetWarpFactor (10.0);
	}

	void Controller::Update ()
	{
		if (!Engaged ()) return;
		if (!Resolve ()) { Stop (PH_FAILED, "No target"); return; }
		double now = td.SimT0;

		// arrived (a flyby: at the time; a rendezvous: braked to rest, Guide)
		if (guided && now >= tArrive - 1e-6 && (settings.arrival == ARRIVE_FLYBY || stopped)) {
			char buf[128];
			snprintf (buf, sizeof (buf), "Arrived at %s", settings.target->Name());
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
		Vector g (g_psys->Gacc (vessel->GPos(), vessel));
		Vector alpha (dir * settings.accel - g);
		attErr = Point (alpha.unit());
		if (!aligned) {
			phase = PH_ALIGN;
			// (a clear departure: not through the body it starts from)
			const Body *blocker = nullptr;
			if (!ClearPath (vessel->GPos(), dir, blocker)) {
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
		LimitWarp (tEnd);
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
		Vector g (g_psys->Gacc (x, vessel));
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
			Drive (alpha, fly ? tArrive : tEnd);
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

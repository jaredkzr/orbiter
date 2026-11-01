// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Relativistic flight of rigid bodies (see Relativity.h): the step of a
// body driven by the ideal drive or faster than any natural orbit.
// - Motion: position and proper velocity in coordinate time, the forces
//   felt on board as a proper acceleration, gravity of all the celestial
//   bodies (their own fields, nonspherical terms included) at first
//   post-Newtonian order, relative to each body's motion.
// - Substeps: the path of one at most a twentieth of the distance to the
//   nearest body, the rapidity change at most 0.002. The bodies move on
//   cubic Hermite curves between their states at the step's ends.
// - Impacts: each substep's straight path is tested against the bodies'
//   spheres (a fast body crosses a planet within a frame), above an
//   impact speed the ground contacts couldn't take.
// - Clock: proper time, including gravitational time dilation.
// - Attitude: turns on the body's own time.
// =======================================================================

#include "Orbiter.h"
#include "Rigidbody.h"
#include "Celbody.h"
#include "Psys.h"
#include "Astro.h"
#include "Relativity.h"
#include "Vessel.h"
#include "RelTravel.h"
#include "Planet.h"
#include "Structure.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <vector>

extern Orbiter *g_pOrbiter;
extern TimeData td;
extern PlanetarySystem *g_psys;
extern Vessel *g_focusobj;

namespace {
	const double REL_SUBSTEP_DIST = 0.05;  // path of a substep, as a fraction of the distance to the nearest body
	const double REL_SUBSTEP_ETA = 0.002;  // rapidity change of a substep
	const int    REL_SUBSTEP_MAX = 5000;
	const double REL_IMPACT_SPEED = 500.0; // [m/s] slower: the ground contacts take the touchdown

	/// A celestial body over the step: Hermite curve between its end states
	struct Source {
		const CelestialBody *body;
		double gm, radius;
		Vector p0, v0, p1, v1;
		void At (double f, double dt, Vector &p, Vector &v) const
		{
			double f2 = f*f, f3 = f2*f;
			double h00 = 2*f3 - 3*f2 + 1, h10 = f3 - 2*f2 + f, h01 = -2*f3 + 3*f2, h11 = f3 - f2;
			double d00 = 6*f2 - 6*f, d10 = 3*f2 - 4*f + 1, d01 = -6*f2 + 6*f, d11 = 3*f2 - 2*f;
			p = p0 * h00 + v0 * (h10 * dt) + p1 * h01 + v1 * (h11 * dt);
			v = (p0 * d00 + p1 * d01) / dt + v0 * d10 + v1 * d11;
		}
	};

	/// Gravity at x (velocity v) at fraction f of the step: post-Newtonian
	/// coordinate acceleration and potential
	Vector Gravity (const std::vector<Source> &src, double f, double dt, const Vector &x, const Vector &v, double &phi)
	{
		thread_local std::vector<Vector> g, vs;
		g.resize (src.size()), vs.resize (src.size());
		phi = 0.0;
		for (size_t i = 0; i < src.size(); i++) {
			Vector p;
			src[i].At (f, dt, p, vs[i]);
			Vector rel (p - x);
			double d = std::max (rel.length(), 1.0);
			g[i] = SingleGacc (rel, src[i].body);
			phi -= src[i].gm / d;
		}
		Vector a;
		for (size_t i = 0; i < src.size(); i++)
			a += relativity::PNAccel (g[i], phi, v - vs[i]);
		return a;
	}
}

void RigidBody::BeginStateUpdate ()
{
	Body::BeginStateUpdate ();
	clockOfs1 = clockOfs;
}

void RigidBody::EndStateUpdate ()
{
	Body::EndStateUpdate ();
	clockOfs = clockOfs1;
}

double RigidBody::Potential (const Vector &gpos)
{
	double phi = 0.0;
	for (size_t i = 0; i < g_psys->nGrav(); i++) {
		const CelestialBody *b = g_psys->GetGravObj ((int)i);
		double d = std::max (gpos.dist (b->s1 ? b->s1->pos : b->GPos()), 1.0);
		phi -= Ggrav * b->Mass() / d;
	}
	return phi;
}

void RigidBody::RelativisticUpdate ()
{
	using namespace relativity;
	const double dt = td.SimDT;
	s1->Set (*s0);
	if (dt <= 0.0) return;

	// the bodies over the step
	std::vector<Source> src;
	src.reserve (g_psys->nGrav());
	for (size_t i = 0; i < g_psys->nGrav(); i++) {
		const CelestialBody *b = g_psys->GetGravObj ((int)i);
		if (b == this) continue;
		src.push_back (Source{ b, Ggrav * b->Mass(), b->Size(), b->s0->pos, b->s0->vel, b->s1->pos, b->s1->vel });
	}

	// substeps: by the distance to the nearest body, the rapidity change
	const Vector x0 (s0->pos), v0 (s0->vel);
	double hmax = dt;
	for (const Source &s : src) {
		double d = x0.dist (s.p0), vr = (v0 - s.v0).length();
		if (vr > 0.0) hmax = std::min (hmax, REL_SUBSTEP_DIST * d / vr);
	}
	double a0 = ProperAccel (*s0, td.SimT0).length();
	if (a0 > 0.0) hmax = std::min (hmax, REL_SUBSTEP_ETA * C / a0);
	// (a substep boundary where the drive is cut off within the step)
	double fcut = (DriveCutoff() - td.SimT0) / dt;
	std::vector<double> fb (1, 0.0);
	for (double fe : { fcut > 0.0 && fcut < 1.0 ? fcut : 1.0, 1.0 }) {
		double f = fb.back();
		if (fe <= f) continue;
		int n = std::clamp ((int)std::ceil ((fe - f) * dt / hmax), 1, REL_SUBSTEP_MAX);
		for (int k = 1; k <= n; k++) fb.push_back (f + (fe - f) * k / n);
	}

	// position as the change from x0 (precision), proper velocity
	Vector d, u (UOfVelocity (v0));
	for (size_t k = 0; k + 1 < fb.size(); k++) {
		double f0 = fb[k], f1 = fb[k+1], fh = 0.5 * (f0 + f1), h = (f1 - f0) * dt;
		Vector alpha (ProperAccel (*s1, td.SimT0 + f0 * dt)); // (with the attitude at the substep's start)
		auto rate = [&](double f, const Vector &dd, const Vector &uu, Vector &ddot, double *pphi = nullptr) {
			Vector v (VelocityOfU (uu));
			double ph;
			Vector a (Gravity (src, f, dt, x0 + dd, v, ph));
			if (pphi) *pphi = ph;
			ddot = v;
			return URate (uu, a, alpha);
		};
		Vector k1d, k2d, k3d, k4d;
		double p1, p2, p3, p4; // (the potential at each stage: the clock's rate)
		Vector k1u = rate (f0, d, u, k1d, &p1);
		Vector k2u = rate (fh, d + k1d * (0.5 * h), u + k1u * (0.5 * h), k2d, &p2);
		Vector k3u = rate (fh, d + k2d * (0.5 * h), u + k2u * (0.5 * h), k3d, &p3);
		Vector k4u = rate (f1, d + k3d * h, u + k3u * h, k4d, &p4);
		Vector dPrev (d);
		double g0 = GammaOfU (u);
		clockOfs1 += (ClockRateMinus1 (k1d.length2(), p1) + 2.0 * ClockRateMinus1 (k2d.length2(), p2) +
			2.0 * ClockRateMinus1 (k3d.length2(), p3) + ClockRateMinus1 (k4d.length2(), p4)) * (h / 6.0);
		d += (k1d + k2d * 2.0 + k3d * 2.0 + k4d) * (h / 6.0);
		u += (k1u + k2u * 2.0 + k3u * 2.0 + k4u) * (h / 6.0);
		if (!std::isfinite (d.x + d.y + d.z + u.x + u.y + u.z)) {
			// (a state the integration can't continue: kept as it was)
			LOGOUT_WARN ("Relativistic flight: %s: the state diverged; the step is dropped", Name());
			s1->Set (*s0);
			RelDiverged ();
			return;
		}

		// impacts along the substep's path (relative to each body)
		for (const Source &s : src) {
			if (s.radius <= 0.0) continue;
			Vector pa, va, pb, vb;
			s.At (f0, dt, pa, va);
			s.At (f1, dt, pb, vb);
			Vector r0 (x0 + dPrev - pa), r1 (x0 + d - pb), dr (r1 - r0);
			double R2 = s.radius * s.radius;
			if (r0.length2() <= R2) continue; // (inside already: not an impact of this step)
			double A = dr.length2(), B = 2.0 * dotp (r0, dr), Cc = r0.length2() - R2;
			double disc = B*B - 4.0*A*Cc;
			if (A <= 0.0 || disc < 0.0) continue;
			double t = (-B - std::sqrt (disc)) / (2.0 * A);
			if (t < 0.0 || t > 1.0) continue;
			Vector vrel (VelocityOfU (u) - (va + (vb - va) * t));
			if (vrel.length() < REL_IMPACT_SPEED) continue;
			if (RelImpact (s.body, r0 + dr * t, vrel)) return;
		}

		// attitude, on the body's own time
		double ht = h * 0.5 * (1.0 / g0 + 1.0 / GammaOfU (u));
		s1->Q.Rotate (s1->omega * ht);
		s1->omega += arot * ht;
		s1->pos = rpos_base + rpos_add + d;
		s1->vel = VelocityOfU (u);
		s1->R.Set (s1->Q);
	}
	// torque at the step's end: the next step's turn rate change (once a
	// frame: the moments evaluate the gravity sources' ephemerides)
	Vector accDummy, tau;
	GetIntermediateMoments (accDummy, tau, *s1, 1.0, dt);
	arot.Set (EulerInv_full (tau, s1->omega));
	rpos_add += d;
	Vector v1 (VelocityOfU (u));
	rvel_add += v1 - v0;
	s1->pos = rpos_base + rpos_add;
	s1->vel = rvel_base + rvel_add;
	acc = (v1 - v0) / dt;
	// ground contacts: at a landing's speeds only (their search reaches
	// speed x step: a fast pass would find the ground everywhere; its
	// impacts are found above)
	const Source *near = nullptr;
	double dn = 1e300;
	for (const Source &q : src) {
		double d = (s1->pos - q.p1).length() / std::max (q.radius, 1.0);
		if (d < dn) dn = d, near = &q;
	}
	if (near && (v1 - near->v1).length() < REL_IMPACT_SPEED) ResolveContacts (dt);
}

// =======================================================================
// Vessels: the ideal drive, impacts

bool Vessel::SetRelDrive (double accel, bool on)
{
	if (on && (supervessel || attach)) return false;
	reldrive.accel = std::clamp (accel, -RELDRIVE_MAX_ACCEL, RELDRIVE_MAX_ACCEL); // (negative: reversed, along -z)
	reldrive.on = on;
	reldrive.until = 0.0;
	return true;
}

bool Vessel::Relativistic () const
{
	if (supervessel || attach || destroyed || bFRplayback || fstatus != FLIGHTSTATUS_FREEFLIGHT) return false;
	return (reldrive.on && reldrive.accel != 0.0) || s0->vel.length2() > REL_SPEED * REL_SPEED;
}

Vector Vessel::ProperAccel (const StateVectors &s, double t) const
{
	Vector a (Flin_add / mass);
	if (reldrive.on && !(reldrive.until > 0.0 && t >= reldrive.until)) a.z += reldrive.accel;
	return mul (s.Q, a);
}

double Vessel::DriveCutoff () const
{
	return reldrive.on ? reldrive.until : 0.0;
}

bool Vessel::RelImpact (const CelestialBody *body, const Vector &rpos, const Vector &rvel)
{
	char cbuf[256];
	double v = rvel.length();
	if (v >= 0.01 * relativity::C) sprintf (cbuf, "hit %s at %0.3f c", body->Name(), v / relativity::C);
	else sprintf (cbuf, "hit %s at %0.1f km/s", body->Name(), v * 1e-3);
	auto setState = [&](const Vector &pos, const Vector &vel) {
		s1->pos = pos, s1->vel = vel;
		rpos_base = pos, rpos_add.Set (0, 0, 0);
		rvel_base = vel, rvel_add.Set (0, 0, 0);
		acc.Set (0, 0, 0);
	};
	reldrive.on = false;
	if (structure::DamageEnabled()) {
		// (where it hit, as it hit)
		setState (body->s1->pos + rpos, body->s1->vel + rvel);
		Destroy (cbuf);
	} else {
		// set down on the ground below the impact point, moving with it
		Vector up (rpos.unit());
		double rad = body->Size();
		if (body->Type() == OBJTP_PLANET && ((const Planet*)body)->HasSurface()) {
			double lng, lat, r;
			body->LocalToEquatorial (tmul (body->s1->R, up), lng, lat, r);
			rad += ((const Planet*)body)->Elevation (lng, lat);
		}
		Vector p1 (up * (rad + size));
		Vector p0 (mul (body->s0->R, tmul (body->s1->R, p1)) + body->s0->pos); // (the ground point at the step's start)
		setState (body->s1->pos + p1, td.SimDT > 0.0 ? (body->s1->pos + p1 - p0) / td.SimDT : body->s1->vel);
		s1->omega.Set (0, 0, 0);
		strcat (cbuf, ": the drive stopped");
	}
	if (reltrip) reltrip->Abort (cbuf);
	LOGOUT ("Relativistic flight: %s %s", Name(), cbuf);
	if (this == g_focusobj) g_pOrbiter->ShowInfoMessage (cbuf);
	return true;
}

void Vessel::Impacted (const Vector &gpos, const Vector &gvel, double speed)
{
	// A long step's impact (high time acceleration): at crash speed with the
	// damage simulation, destroyed where it hit; else set down there
	char cbuf[256];
	sprintf (cbuf, "hit %s at %.0f m/s", proxybody ? proxybody->Name() : "the ground", speed);
	VesselBase::Impacted (gpos, gvel, speed);
	reldrive.on = false;
	if (reltrip) reltrip->Abort (cbuf);
	if (structure::DamageEnabled() && speed > CRASH_SPEED) {
		Destroy (cbuf);
	} else if (speed > CRASH_SPEED) {
		strcat (cbuf, ": set down");
		if (this == g_focusobj) g_pOrbiter->ShowInfoMessage (cbuf);
	}
	LOGOUT ("%s %s (a step of %.1f s)", Name(), cbuf, td.SimDT);
}

void Vessel::RelDiverged ()
{
	reldrive.on = false;
	if (reltrip) reltrip->Abort ("The flight could not be integrated: the drive stopped");
}

reltravel::Controller *Vessel::RelTrip (bool create)
{
	if (!reltrip && create) reltrip = std::make_unique<reltravel::Controller> (this);
	return reltrip.get();
}

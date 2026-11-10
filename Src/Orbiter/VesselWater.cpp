// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Vessels (and debris) in the water: the step re-integrated with the
// water's forces (Hydro.h) where the body reaches the sea (Sea.h), in a
// local frame on the turning planet: x east, y up, z north, its origin on
// the water's mean surface under the body.
// =======================================================================

#include "Vesselbase.h"
#include "Vessel.h"
#include "Planet.h"
#include "Sea.h"
#include "Hydro.h"
#include "Structure.h"
#include "Orbiter.h"
#include "Log.h"
#include "Astro.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>

extern TimeData td;

namespace {

	// planet frame -> local horizon frame at (lng, lat): rows east, up, north (Vessel's L2H)
	Matrix L2H (double lng, double lat)
	{
		double slng = sin (lng), clng = cos (lng), slat = sin (lat), clat = cos (lat);
		return Matrix (-slng, 0, clng, clat * clng, slat, clat * slng, -slat * clng, clat, -slat * slng);
	}

	const int GRID_MAX = 17;      // swell grid nodes per side
	const double GRID_MIN_DS = 0.5; // [m]

}

void VesselBase::HydroStep (double h)
{
	water.near = false;
	if (h <= 0.0 || !proxyplanet || proxybody != proxyplanet || IsComponent() || fstatus != FLIGHTSTATUS_FREEFLIGHT) return;
	sea::Sea *sea = proxyplanet->GetSea();
	if (!sea) return;
	const Planet *pl = proxyplanet;
	const StateVectors &S0 = StartState(), &P0 = *pl->s0, &P1 = *pl->s1;
	const double R = pl->Size();
	// where the step ends, over the planet
	Vector p1 (tmul (P1.R, s1->pos - P1.pos)), p0 (tmul (P0.R, S0.pos - P0.pos));
	double r1 = p1.length(), r0 = p0.length();
	if (!(r1 < R + 1e5) && !(r0 < R + 1e5)) { water.wet = false; return; } // (far above anything)
	double lng = atan2 (p1.z, p1.x), lat = asin (std::clamp (p1.y / r1, -1.0, 1.0));
	const hydro::Model *m = nullptr;
	// (reach: the body's size, the swell's crests, its motion over the step)
	const sea::Swell &sw = sea->GetSwell();
	double travel = (s1->pos - S0.pos).length() + 1.0;
	double size0 = CollisionRadius() > 0.0 ? CollisionRadius() : Size();
	if (std::min (r0, r1) - R > size0 + sw.CrestMax() + travel + 500.0) { water.wet = false; return; } // (well above the sea's surface; no lookups)
	sea::Point sp = sea->At (lat, lng, false);
	if (!sp.water) { water.wet = false; return; }
	m = WaterModel();
	if (!m || m->Empty()) return;
	double reach = m->radius + sw.CrestMax() + travel;
	if (std::min (r0, r1) - R - sp.datum > reach) { water.wet = false; return; }
	water.near = true;
	water.volume = m->volume;

	// the local frame: on the datum under the step's end
	Matrix L (L2H (lng, lat));
	Vector up (p1 / r1);
	Vector O (up * (R + sp.datum));
	const double wrot = Pi2 / pl->RotT();
	auto ground = [&](const Vector &p) { return Vector (-p.z * wrot, 0, p.x * wrot); }; // (the turning planet's velocity at p, planet frame)
	auto toLocal = [&](const StateVectors &S, const StateVectors &P, Vector &x, Vector &v, Matrix &Rb) {
		Vector pp (tmul (P.R, S.pos - P.pos));
		x = mul (L, pp - O);
		v = mul (L, tmul (P.R, S.vel - P.vel) - ground (pp));
		Rb = L * (transp (P.R) * S.R);
	};
	Vector x0, v0, x1, v1;
	Matrix R0, R1;
	toLocal (S0, P0, x0, v0, R0);
	toLocal (*s1, P1, x1, v1, R1);
	const Vector cg (WaterCG());

	// the swell about the body at the step's end
	hydro::Surface srf;
	srf.rho = sea->Density(), srf.nu = sea->Viscosity();
	srf.g = Ggrav * pl->Mass() / (R * R);
	if (!sw.Calm()) {
		double ext = 2.0 * (m->radius + (x1 - x0).length()) + 2.0;
		double ds = std::max (ext / (GRID_MAX - 1), GRID_MIN_DS);
		int n = std::clamp ((int)ceil (ext / ds) + 1, 2, GRID_MAX);
		srf.n = n, srf.ds = ds;
		Vector c (x1 + mul (R1, cg));
		srf.x0 = c.x - 0.5 * ds * (n - 1), srf.z0 = c.z - 0.5 * ds * (n - 1);
		srf.h.resize ((size_t)n * n), srf.vel.resize ((size_t)n * n);
		double t = sea::Sea::SwellTime (td.MJD1);
		Vector e (L.m11, L.m12, L.m13), nn (L.m31, L.m32, L.m33); // (east, north: planet frame)
		for (int j = 0; j < n; j++)
			for (int i = 0; i < n; i++) {
				Vector u ((O + e * (srf.x0 + i * ds) + nn * (srf.z0 + j * ds)).unit());
				sea::SwellSample ss;
				sw.Sample (u, t, 0.0, ss);
				srf.h[(size_t)j * n + i] = ss.h;
				srf.vel[(size_t)j * n + i] = ss.vel;
			}
		double wp = Pi2 / std::max (sw.Tp(), 0.1);
		srf.kp = wp * wp / srf.g;
	}

	// (quick: not reaching the water in the step at all)
	{
		double lowest = std::min (x0.y, x1.y) - m->radius;
		if (lowest > sw.CrestMax() + 0.5) { water.wet = false; return; }
	}

	// the step again, in the water: from the start, with the other forces'
	// accelerations as they were over it
	hydro::Body b;
	b.mass = mass;
	b.pmi = pmi;
	b.cg = cg;
	b.x = x0;
	b.R = R0;
	b.w = S0.omega;
	// (velocities of the CG; the origin's plus w x cg)
	b.v = v0 + mul (R0, crossp (S0.omega, cg));
	Vector v1cg (v1 + mul (R1, crossp (s1->omega, cg)));
	Vector aExt ((v1cg - b.v) / h);
	// (the sea's surface is level to gravity and the planet's turning
	// together, the geoid: a sphere's centrifugal pull along the surface,
	// towards the equator, would make everything afloat drift)
	{
		Vector pc (O + tmul (L, x1));
		Vector acf (mul (L, Vector (pc.x, 0.0, pc.z) * (wrot * wrot))); // (outward from the axis; local frame)
		aExt -= Vector (acf.x, 0.0, acf.z);
	}
	Vector alphaExt ((s1->omega - S0.omega) / h);
	hydro::Result r;
	static const bool prof = getenv ("ORBITER_WATER_PROFILE") != nullptr;
	auto tp0 = std::chrono::steady_clock::now();
	hydro::Step (*m, srf, b, aExt, alphaExt, h, WaterParts(), r);
	if (prof) {
		static double tsum = 0.0; static int nsum = 0;
		tsum += std::chrono::duration<double> (std::chrono::steady_clock::now() - tp0).count(); nsum++;
		if (nsum == 500) { LOGOUT ("Water profile: %.3f ms per step (%s, last %d sub-steps)", tsum / nsum * 1e3, Name(), r.substeps); tsum = 0.0; nsum = 0; }
	}

	bool wet = r.wetted > 0.0 || r.submerged > 0.0 || r.F.length2() > 0.0;
	if (!wet) water.accFilt = 0.0;
	if (wet && !water.wet) water.tWet = td.SimT1, water.entrySpeed = 0.0, water.slamMax = 0.0, water.accMax = 0.0;
	water.wet = wet;
	water.submerged = r.submerged;
	water.wetted = r.wetted;
	water.waterline = r.waterline;
	water.substeps = r.substeps;
	water.entrySpeed = std::max (water.entrySpeed, r.entrySpeed);
	water.slamMax = std::max (water.slamMax, r.slamMax);
	// (the acceleration felt: the water's mean over the step, filtered over
	// ACC_TAU; the sub-steps' peaks are the solver's, not what is felt)
	{
		const double ACC_TAU = 0.02;
		double a = r.F.length() / std::max (mass, 1e-3);
		water.accFilt += (a - water.accFilt) * std::min (1.0, h / ACC_TAU);
		water.accMax = std::max (water.accMax, water.accFilt);
	}
	if (!wet && r.F.length2() == 0.0) { water.F.Set (0, 0, 0); return; } // (the step as it was)

	// the new state, back to the global frame
	Vector vo (b.v - mul (b.R, crossp (b.w, cg))); // (the origin's velocity)
	Vector pp (O + tmul (L, b.x));
	Vector pos (P1.pos + mul (P1.R, pp));
	Vector vel (P1.vel + mul (P1.R, tmul (L, vo) + ground (pp)));
	Matrix Rg (P1.R * (transp (L) * b.R));
	Vector dpos (pos - s1->pos), dvel (vel - s1->vel);
	rpos_add += dpos;
	rvel_add += dvel;
	s1->pos = rpos_base + rpos_add;
	s1->vel = rvel_base + rvel_add;
	s1->Q.Set (Rg);
	s1->R.Set (s1->Q);
	s1->omega = b.w;
	water.F = mul (P1.R, tmul (L, r.F));
	WaterLoads (r, h);
}

double VesselBase::WaterDepth (const Vector &p) const
{
	if (!water.near || !proxyplanet) return -1e10;
	sea::Sea *sea = proxyplanet->GetSea();
	if (!sea) return -1e10;
	const StateVectors &P = *proxyplanet->s0;
	Vector pp (tmul (P.R, s0->pos + mul (s0->R, p) - P.pos));
	double r = pp.length();
	double lng = atan2 (pp.z, pp.x), lat = asin (std::clamp (pp.y / r, -1.0, 1.0));
	sea::Point sp = sea->At (lat, lng, false);
	if (!sp.water) return -1e10;
	return sp.datum + sea->GetSwell().Height (pp / r, sea::Sea::SwellTime (td.MJD0)) - (r - proxyplanet->Size());
}

// =======================================================================
// Vessel

const hydro::Model *Vessel::WaterModel () const
{
	const std::vector<collision::Hull> &hl = Hulls();
	DWORD dv = DamageVersion();
	if (water_model && water_model_gen == hull_gen && water_model_dmg == dv) return water_model.get();
	auto t0 = std::chrono::steady_clock::now();
	const structure::Model &sm = Structure();
	std::vector<convex::Polytope> hs;
	for (size_t i = 0; i < hl.size(); i++) {
		if (!part_lost.empty() && PartLost (sm.PartOfHull ((int)i))) continue;
		hs.push_back (hl[i]);
	}
	water_model = std::make_unique<hydro::Model> (hs.empty() ? hydro::Sphere (Vector (0, 0, 0), 0.5 * size) :
		hydro::Build (hs, [&sm](const Vector &p) { return sm.PartAt (p); }));
	water_model_gen = hull_gen;
	water_model_dmg = dv;
	double ms = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() * 1e3;
	LOGOUT ("Water model of %s: %d hulls, %.2f m3 in %d cells of %.3f m, %d panels %.1f m2, %.0f ms",
		Name(), (int)hs.size(), water_model->volume, (int)water_model->cells.size(), water_model->h,
		(int)water_model->panels.size(), water_model->area, ms);
	return water_model.get();
}

int Vessel::WaterParts () const
{
	return (int)part_Fc.size();
}

void Vessel::WaterLoads (const hydro::Result &r, double h)
{
	// (the water's forces load the parts as contacts do: W6)
}

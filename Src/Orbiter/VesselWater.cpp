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
#include "Util.h"
#include "Mesh.h"
#include "Astro.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <unordered_map>
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
	// (a long step, at high time acceleration or a fall at kilometres a
	// second: its impact on the water is found along its path, SurfaceImpact)
	if ((p1 - p0).length() > OBSTACLE_REACH) { water.wet = false; return; }
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
	water.flooded = Type() == OBJTP_DEBRIS;

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
	// (Orbiter's angular velocity turns a point at r with crossp (r, omega) and
	// a force F at r has the moment crossp (F, r); Hydro's the opposite: its w
	// is -omega, its torques crossp (r, F))
	b.w = -S0.omega;
	// (velocities of the CG; the origin's plus w x cg)
	b.v = v0 + mul (R0, crossp (b.w, cg));
	Vector v1cg (v1 + mul (R1, crossp (-s1->omega, cg)));
	Vector aExt ((v1cg - b.v) / h);
	// (the sea's surface is level to gravity and the planet's turning
	// together, the geoid: a sphere's centrifugal pull along the surface,
	// towards the equator, would make everything afloat drift)
	{
		Vector pc (O + tmul (L, x1));
		Vector acf (mul (L, Vector (pc.x, 0.0, pc.z) * (wrot * wrot))); // (outward from the axis; local frame)
		aExt -= Vector (acf.x, 0.0, acf.z);
	}
	Vector alphaExt ((S0.omega - s1->omega) / h);
	hydro::Result r;
	static const bool prof = getenv ("ORBITER_WATER_PROFILE") != nullptr;
	auto tp0 = std::chrono::steady_clock::now();
	static double dbgT = getenv ("ORBITER_WATER_DEBUG_T") ? atof (getenv ("ORBITER_WATER_DEBUG_T")) : -1.0;
	hydro::debugPanels = (dbgT >= 0.0 && water.wet && td.SimT1 - water.tWet < dbgT) ? 6 : 0;
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
	s1->omega = -b.w;
	water.F = mul (P1.R, tmul (L, r.F));
	water_vel = P1.vel + mul (P1.R, ground (pp));
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

// The hulls are made from the meshes as stored, in their animations'
// default states (a delta-glider's gear down): the water model is made from
// them posed as the animations are now. Each hull belongs to the mesh group
// most of its vertices came from; each group is moved as the graphics
// client moves it (a component maps its animation's state onto a progress
// over [state0, state1], and is applied for the difference from the
// default's progress; children inherit their parents' transforms).

namespace {

	struct Xf { Matrix R; Vector t; }; // p' = R p + t
	Xf Mul (const Xf &a, const Xf &b) { return { a.R * b.R, mul (a.R, b.t) + a.t }; }
	bool Identity (const Xf &x)
	{
		const double *r = x.R.data;
		return fabs (r[0] - 1) + fabs (r[4] - 1) + fabs (r[8] - 1) + fabs (r[1]) + fabs (r[2]) + fabs (r[3]) + fabs (r[5]) + fabs (r[6]) + fabs (r[7]) < 1e-9 && x.t.length2() < 1e-12;
	}
	double Progress (const ANIMATIONCOMP *c, double state)
	{
		if (c->state1 == c->state0) return state >= c->state0 ? 1.0 : 0.0;
		return std::clamp ((state - c->state0) / (c->state1 - c->state0), 0.0, 1.0);
	}
	Xf LocalXf (const ANIMATIONCOMP *c, double state, double defstate)
	{
		Xf I { Matrix (1,0,0, 0,1,0, 0,0,1), Vector (0,0,0) };
		double dp = Progress (c, state) - Progress (c, defstate);
		MGROUP_TRANSFORM *tr = c->trans;
		if (!tr || dp == 0.0) return I;
		switch (tr->Type()) {
		case MGROUP_TRANSFORM::ROTATE: {
			auto *rot = static_cast<MGROUP_ROTATE*>(tr);
			Vector ax (MakeVector (rot->axis));
			double len = ax.length();
			if (len == 0.0) return I;
			Vector k (ax / len);
			double th = rot->angle * dp, cs = cos (th), sn = sin (th), c1 = 1.0 - cs;
			Matrix R (cs + c1*k.x*k.x, c1*k.x*k.y - sn*k.z, c1*k.x*k.z + sn*k.y,
				c1*k.x*k.y + sn*k.z, cs + c1*k.y*k.y, c1*k.y*k.z - sn*k.x,
				c1*k.x*k.z - sn*k.y, c1*k.y*k.z + sn*k.x, cs + c1*k.z*k.z);
			Vector ref (MakeVector (rot->ref));
			return { R, ref - mul (R, ref) };
		}
		case MGROUP_TRANSFORM::TRANSLATE:
			return { I.R, MakeVector (static_cast<MGROUP_TRANSLATE*>(tr)->shift) * dp };
		case MGROUP_TRANSFORM::SCALE: {
			auto *scl = static_cast<MGROUP_SCALE*>(tr);
			double p = Progress (c, state), p0 = Progress (c, defstate);
			auto ratio = [&](double sv) { double d = 1.0 + (sv - 1.0) * p0; return d != 0.0 ? (1.0 + (sv - 1.0) * p) / d : 1.0; };
			Matrix S (ratio (scl->scale.x), 0, 0, 0, ratio (scl->scale.y), 0, 0, 0, ratio (scl->scale.z));
			Vector ref (MakeVector (scl->ref));
			return { S, ref - mul (S, ref) };
		}
		default:
			return I;
		}
	}

}

uint64_t Vessel::WaterPose (std::vector<std::pair<Matrix,Vector>> *hullXf) const
{
	const std::vector<collision::Hull> &hl = Hulls();
	// (each hull's mesh group: once per set of hulls)
	if (hull_group_gen != hull_gen) {
		hull_group_gen = hull_gen;
		hull_group.assign (hl.size(), { -1, -1 });
		std::vector<ExtGroup> eg;
		ExternalGroups (eg);
		std::unordered_map<uint64_t, int> vtx; // (quantised vertex -> index into eg)
		auto key = [](const Vector &p) {
			auto q = [](double v) { return (uint64_t)(int64_t)llround (v * 1000.0) & 0x1fffff; };
			return (q (p.x) << 42) | (q (p.y) << 21) | q (p.z);
		};
		for (size_t e = 0; e < eg.size(); e++) {
			const GroupSpec *g = (const GroupSpec*)eg[e].grp;
			for (DWORD k = 0; k < g->nVtx; k++) vtx[key (Vector (g->Vtx[k].x, g->Vtx[k].y, g->Vtx[k].z) + eg[e].ofs)] = (int)e;
		}
		for (size_t h = 0; h < hl.size(); h++) {
			std::unordered_map<int,int> votes;
			for (const Vector &v : hl[h].v) {
				auto it = vtx.find (key (v));
				if (it != vtx.end()) votes[it->second]++;
			}
			int best = -1, bn = 0;
			for (auto &vt : votes) if (vt.second > bn) best = vt.first, bn = vt.second;
			if (best >= 0) hull_group[h] = { (int)eg[best].mesh, (int)eg[best].group };
		}
	}
	// the groups' transforms now (vessel frame)
	std::map<std::pair<int,int>, Xf> gx;
	uint64_t pose = 1469598103934665603ull;
	if (nanim && anim) {
		std::unordered_map<const ANIMATIONCOMP*, const ANIMATION*> owner;
		for (UINT a = 0; a < nanim; a++)
			for (UINT c = 0; c < anim[a].ncomp; c++) owner[anim[a].comp[c]] = &anim[a];
		std::unordered_map<const ANIMATIONCOMP*, Xf> cm;
		std::function<Xf(const ANIMATIONCOMP*)> comp = [&](const ANIMATIONCOMP *c) -> Xf {
			auto it = cm.find (c);
			if (it != cm.end()) return it->second;
			auto ow = owner.find (c);
			Xf M = ow != owner.end() ? LocalXf (c, ow->second->state, ow->second->defstate) : Xf { Matrix (1,0,0, 0,1,0, 0,0,1), Vector (0,0,0) };
			if (c->parent) M = Mul (comp (c->parent), M);
			cm[c] = M;
			return M;
		};
		for (UINT a = 0; a < nanim; a++) {
			// (the pose: the states, coarsely; a change of a hundredth makes no new model)
			int64_t q = llround (anim[a].state * 100.0);
			pose = (pose ^ (uint64_t)q) * 1099511628211ull;
			for (UINT ci = 0; ci < anim[a].ncomp; ci++) {
				const ANIMATIONCOMP *c = anim[a].comp[ci];
				MGROUP_TRANSFORM *tr = c->trans;
				if (!tr || !tr->grp || !tr->ngrp || tr->mesh == LOCALVERTEXLIST || tr->mesh >= nmesh || !meshlist[tr->mesh]) continue;
				Xf M = comp (c);
				// (in the vessel frame: the mesh's offset)
				Vector ofs (MakeVector (meshlist[tr->mesh]->meshofs));
				Xf V { M.R, M.t + ofs - mul (M.R, ofs) };
				for (UINT k = 0; k < tr->ngrp; k++) {
					auto keyg = std::make_pair ((int)tr->mesh, (int)tr->grp[k]);
					auto it = gx.find (keyg);
					gx[keyg] = it == gx.end() ? V : Mul (V, it->second);
				}
			}
		}
	}
	if (hullXf) {
		hullXf->assign (hl.size(), { Matrix (1,0,0, 0,1,0, 0,0,1), Vector (0,0,0) });
		for (size_t h = 0; h < hl.size(); h++) {
			auto it = gx.find (hull_group[h]);
			if (it != gx.end() && !Identity (it->second)) (*hullXf)[h] = { it->second.R, it->second.t };
		}
	}
	return pose;
}

const hydro::Model *Vessel::WaterModel () const
{
	const std::vector<collision::Hull> &hl = Hulls();
	DWORD dv = DamageVersion();
	uint64_t pose = WaterPose (nullptr);
	// (made again for new hulls, damage, or another pose: at most every
	// WATER_POSE_DT [s] while the pose changes, e.g. the gear going up)
	if (water_model && water_model_gen == hull_gen && water_model_dmg == dv &&
		(water_model_pose == pose || td.SimT1 - water_model_t < WATER_POSE_DT)) return water_model.get();
	auto t0 = std::chrono::steady_clock::now();
	std::vector<std::pair<Matrix,Vector>> xf;
	WaterPose (&xf);
	const structure::Model &sm = Structure();
	std::vector<convex::Polytope> hs;
	int moved = 0;
	for (size_t i = 0; i < hl.size(); i++) {
		if (!part_lost.empty() && PartLost (sm.PartOfHull ((int)i))) continue;
		const Matrix &R = xf[i].first;
		const Vector &t = xf[i].second;
		if (t.length2() == 0.0 && R.m11 == 1.0 && R.m22 == 1.0 && R.m33 == 1.0) { hs.push_back (hl[i]); continue; }
		// (posed: its points moved, the hull made again)
		std::vector<Vector> pts;
		for (const Vector &v : hl[i].v) pts.push_back (mul (R, v) + t);
		convex::Polytope P;
		if (convex::Build (pts, P)) hs.push_back (std::move (P)), moved++;
	}
	water_model = std::make_unique<hydro::Model> (hs.empty() ? hydro::Sphere (Vector (0, 0, 0), 0.5 * size) :
		hydro::Build (hs, [&sm](const Vector &p) { return sm.PartAt (p); }));
	water_model_gen = hull_gen;
	water_model_dmg = dv;
	water_model_pose = pose;
	water_model_t = td.SimT1;
	double ms = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() * 1e3;
	if (const char *dump = getenv ("ORBITER_WATER_DUMP")) {
		FILE *f = fopen (dump, "w");
		if (f) {
			for (const hydro::Panel &q : water_model->panels) fprintf (f, "P %f %f %f %f %f %f %f %d\n", q.c.x, q.c.y, q.c.z, q.n.x, q.n.y, q.n.z, q.area, q.part);
			for (size_t i = 0; i < hs.size(); i++) for (const convex::Face &fc : hs[i].f) {
				fprintf (f, "F %zu %f %f %f", i, fc.n.x, fc.n.y, fc.n.z);
				for (int k : fc.idx) fprintf (f, " %f %f %f", hs[i].v[k].x, hs[i].v[k].y, hs[i].v[k].z);
				fprintf (f, "\n");
			}
			fclose (f);
		}
	}
	LOGOUT ("Water model of %s: %d hulls (%d posed), %.2f m3 in %d cells of %.3f m, %d panels %.1f m2%s, %.0f ms",
		Name(), (int)hs.size(), moved, water_model->volume, (int)water_model->cells.size(), water_model->h,
		(int)water_model->panels.size(), water_model->area, water_model->symmetric ? ", symmetric" : "", ms);
	return water_model.get();
}

int Vessel::WaterParts () const
{
	return (int)part_Fc.size();
}

void Vessel::WaterLoads (const hydro::Result &r, double h)
{
	// The water's forces load the parts they act on, as contacts do. The
	// solver stops a panel within a sub-step (the water as rigid as the
	// laws make it); a structure meeting water is decelerated over a stroke
	// (the hull's penetration and give): the loads are the forces low-passed
	// over the time that stroke takes at the closing speed, so a blow is
	// spread and steady forces (buoyancy, planing) are kept whole. Its
	// torque about the centre of gravity in Orbiter's sense (crossp (F, r)).
	if (h <= 0.0) return;
	size_t n = std::min (r.partF.size(), part_Fc.size());
	if (water_Ff.size() != n) water_Ff.assign (n, Vector (0, 0, 0));
	double vc = std::max (r.entrySpeed, 0.5);
	double k = std::min (1.0, h * vc / WATER_STROKE);
	for (size_t j = 0; j < n; j++) water_Ff[j] += (r.partF[j] - water_Ff[j]) * k;
	water_Tf += (r.T - water_Tf) * k;
	if (!loads_on || part_Fc.empty()) return;
	for (size_t j = 0; j < n; j++) {
		const Vector &F = water_Ff[j];
		double w = F.length();
		if (w <= 0.0) continue;
		part_Fc[j] += F;
		// (what the part touched: the water, at rest on the turning planet)
		if (j < part_vx.size()) part_vx[j] += water_vel * (w * h), part_jx[j] += w * h;
		if (j < part_hit.size() && r.entrySpeed > part_hit[j]) {
			part_hit[j] = r.entrySpeed;
			part_hitpos[j] = Structure().Parts()[j].cg;
		}
	}
	contact_tau -= water_Tf;
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Debris.h"
#include "Psys.h"
#include "Planet.h"
#include "Element.h"
#include "elevmgr.h"
#include "Mesh.h"
#include "Orbiter.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

extern PlanetarySystem *g_psys;
extern Orbiter *g_pOrbiter;
extern TimeData td;
extern bool g_bStateUpdate;

namespace {
	const double CD = 1.0;          // drag coefficient (tumbling)
	const double FRICTION = 0.6;    // with the ground
	const double REST_SPEED = 0.05; // [m/s] slower than this (and turning slower than REST_OMEGA) ...
	const double REST_OMEGA = 0.02; // [rad/s]
	const double REST_TIME = 2.0;   // [s] ... for this long: at rest
}

Debris::Debris (const std::string &_name, const std::string &_source, double m, const Vector &cg,
	const StateVectors &s, std::vector<Group> &&_groups, const std::vector<std::vector<Vector>> &parts)
: VesselBase (m, 1.0, Vector (1,1,1)), source (_source), groups (std::move (_groups))
{
	VesselBase::SetDefaultState ();
	name = _name;
	created_t = td.SimT1;
	static DWORD next = 0;
	serial = ++next;

	// the triangles about the centre of gravity; area and moments of
	// inertia (a thin shell of uniform density)
	double S[3] = {0,0,0};
	for (Group &g : groups) {
		g.ofs -= cg;
		for (NTVERTEX &v : g.vtx) v.x -= (float)cg.x, v.y -= (float)cg.y, v.z -= (float)cg.z;
		for (size_t k = 0; k + 2 < g.vtx.size(); k += 3) {
			Vector p[3];
			for (int i = 0; i < 3; i++) p[i] = Vector (g.vtx[k+i].x, g.vtx[k+i].y, g.vtx[k+i].z);
			double a = 0.5 * crossp (p[1]-p[0], p[2]-p[0]).length();
			area += a;
			Vector sum (p[0] + p[1] + p[2]);
			for (int j = 0; j < 3; j++) // (the integral of x_j^2 over the triangle)
				S[j] += a / 12.0 * (p[0].data[j]*p[0].data[j] + p[1].data[j]*p[1].data[j] + p[2].data[j]*p[2].data[j] + sum.data[j]*sum.data[j]);
		}
	}
	if (area > 0.0) pmi.Set ((S[1]+S[2])/area, (S[0]+S[2])/area, (S[0]+S[1])/area);
	else pmi.Set (0.1, 0.1, 0.1);
	for (int j = 0; j < 3; j++) pmi.data[j] = std::max (pmi.data[j], 1e-3);

	// a convex hull for each part
	for (const std::vector<Vector> &pp : parts) {
		if (pp.empty()) continue;
		std::vector<Vector> q (pp);
		for (Vector &x : q) x -= cg;
		hulls.push_back (collision::MakeHull (q));
	}
	for (const collision::Hull &h : hulls)
		hullBound = std::max (hullBound, h.centre.length() + h.radius + collision::SKIN);
	size = std::max (hullBound, 0.1);
	if (hulls.empty()) hullBound = -1.0; // (no collisions)

	// the state of the vessel's point at cg
	el = new Elements;
	cbody = 0;
	Vector r (mul (s.R, cg));
	s0->pos = s.pos + r;
	s0->vel = s.vel + mul (s.R, crossp (cg, s.omega));
	s0->R.Set (s.R);
	s0->Q.Set (s.Q);
	s0->omega.Set (s.omega);
	if (s1 != s0) s1->Set (*s0);
	rpos_base = s0->pos, rpos_add.Set (0,0,0);
	rvel_base = s0->vel, rvel_add.Set (0,0,0);
	bOrbitStabilised = false;
	gfielddata.ngrav = 0;
	g_psys->ScanGFieldSources (&s0->pos, this, &gfielddata);
	acc = g_psys->Gacc (s0->pos, this, &gfielddata);
	UpdateProxies ();
	cpos = s0->pos - cbody->GPos();
	cvel = s0->vel - cbody->GVel();
	UpdateSurfParams ();
}

Debris::~Debris ()
{
}

void Debris::Ignite (double f, double burnTime)
{
	fuel = f;
	burnEnd = td.SimT1 + burnTime;
	temp0 = std::max (temp0, 1200.0), tempT = td.SimT1;
}

double Debris::BurnLeft () const
{
	return std::max (0.0, burnEnd - td.SimT0);
}

double Debris::Temp () const
{
	// (cooling with a time constant of 30 s towards the air's temperature;
	// burning: at least 1000 K)
	double Ta = sp.is_in_atm && sp.atmT > 0.0 ? sp.atmT : 250.0;
	double T = temp0 > Ta ? Ta + (temp0 - Ta) * exp (-(td.SimT0 - tempT) / 30.0) : Ta;
	return BurnLeft() > 0.0 ? std::max (T, 1000.0) : T;
}

void Debris::ContactsResolved (const std::vector<contact::Point> &pts, int index, double h)
{
	// the fastest sliding contact (sparks) and the impact (dust)
	scrape = impact = 0.0;
	double Jn = 0.0;
	for (const contact::Point &p : pts) {
		if (p.a != index || p.b >= 0 || p.ln <= 0.0) continue;
		Vector v (s1->vel + mul (s1->R, crossp (tmul (s1->R, p.ra), s1->omega)) - p.vs);
		Vector vt (v - p.n * dotp (v, p.n));
		scrape = std::max (scrape, vt.length());
		Jn += p.ln;
	}
	impact = Jn / mass;
}

void Debris::AddVelocity (const Vector &dv)
{
	if (fstatus == FLIGHTSTATUS_LANDED) wake = true;
	if (g_bStateUpdate && s1 != s0) {
		rvel_add += dv;
		s1->vel = rvel_base + rvel_add;
	} else {
		rvel_add += dv;
		s0->vel = rvel_base + rvel_add;
	}
	cvel += dv;
}

// =======================================================================

void Debris::UpdateBodyForces ()
{
	Fdrag.Set (0,0,0);
	Mdrag.Set (0,0,0);
	if (fstatus != FLIGHTSTATUS_FREEFLIGHT || !proxybody || !sp.is_in_atm || sp.atmrho <= 0.0) return;
	double v = sp.airspd;
	if (v < 1e-3) return;
	double A = std::max (0.25 * area, 1e-4); // (mean cross section of a closed surface)
	double F = 0.5 * sp.atmrho * v * v * CD * A;
	double h = td.SimDT;
	if (h > 0.0) F = std::min (F, 0.5 * mass * v / h); // (no reversal within the step)
	Fdrag = sp.airvel_glob * (-F / v);

	// tumbling damped by the air
	double w = s0->omega.length();
	if (w > 0.0) {
		double M = 0.05 * sp.atmrho * v * A * size * size * w;
		double I = mass * std::min ({pmi.x, pmi.y, pmi.z});
		if (h > 0.0) M = std::min (M, 0.5 * I * w / h);
		Mdrag = s0->omega * (-M / w);
	}
}

void Debris::GetIntermediateMoments (Vector &acc, Vector &tau, const StateVectors &state, double tfrac, double dt)
{
	RigidBody::GetIntermediateMoments (acc, tau, state, tfrac, dt); // (gravity)
	acc += Fdrag / mass;
	tau += Mdrag / mass;
}

// =======================================================================

void Debris::SetRestState ()
{
	Planet *P = proxyplanet;
	P->LocalToGlobal_t1 (sp.ploc, s1->pos);
	double vground = Pi2 * sp.rad * sp.clat / P->RotT();
	s1->vel.Set (mul (P->s1->R, Vector (-vground*sp.slng, 0.0, vground*sp.clng)) + P->s1->vel);
	s1->R.Set (land_rot);
	s1->R.premul (P->s1->R);
	s1->Q.Set (s1->R);
	s1->omega.Set (0,0,0);
	acc = P->Acceleration() + (P->s1->pos - s1->pos) * (vground * Pi2 / (sp.rad * P->RotT()));
	el_valid = false;
}

void Debris::Update (bool force)
{
	if (fstatus == FLIGHTSTATUS_FREEFLIGHT) {
		RigidBody::Update (force);
	} else if (fstatus == FLIGHTSTATUS_LANDED && proxyplanet) {
		SetRestState ();
		if (wake) { // (hit: moving again)
			fstatus = FLIGHTSTATUS_FREEFLIGHT;
			rpos_base = s1->pos, rpos_add.Set (0,0,0);
			rvel_base = s1->vel, rvel_add.Set (0,0,0);
			restTime = 0.0;
		}
	}
	wake = false;
	if (proxybody && fstatus != FLIGHTSTATUS_LANDED) UpdateSurfParams ();
	cpos = s1->pos - cbody->s1->pos;
	cvel = s1->vel - cbody->s1->vel;
}

void Debris::PostUpdate ()
{
	// (proxy bodies checked periodically, as for vessels)
	if (fstatus == FLIGHTSTATUS_FREEFLIGHT && td.SimT1 > proxyCheckT) {
		UpdateProxies ();
		proxyCheckT = td.SimT1 + 100.0;
	}

	// at rest: still on the ground for a while
	if (fstatus == FLIGHTSTATUS_FREEFLIGHT) {
		bool still = false;
		if (touching && proxyplanet) {
			const Planet *P = proxyplanet;
			Vector pl (tmul (P->s0->R, s0->pos - P->s0->pos));
			double wrot = Pi2 / P->RotT();
			Vector vg (P->s0->vel + mul (P->s0->R, Vector (-pl.z*wrot, 0, pl.x*wrot)));
			still = s0->vel.dist (vg) < REST_SPEED && s0->omega.length() < REST_OMEGA;
		}
		restTime = still ? restTime + td.SimDT : 0.0;
		if (restTime > REST_TIME) {
			Matrix lrot (s0->R);
			lrot.tpremul (proxyplanet->s0->R);
			InitLanded (proxyplanet, 0.0, 0.0, 0.0, &lrot);
		}
	}
}

void Debris::InitLanded (Planet *planet, double, double, double, const Matrix *hrot, double, bool)
{
	// (at rest where it is)
	proxybody = proxyplanet = planet;
	sp.ploc = tmul (planet->s0->R, s0->pos - planet->s0->pos);
	planet->LocalToEquatorial (sp.ploc, sp.lng, sp.lat, sp.rad);
	sp.slng = sin (sp.lng), sp.clng = cos (sp.lng), sp.slat = sin (sp.lat), sp.clat = cos (sp.lat);
	if (hrot) land_rot.Set (*hrot);
	else {
		land_rot.Set (s0->R);
		land_rot.tpremul (planet->s0->R);
	}
	fstatus = FLIGHTSTATUS_LANDED;
	bSurfaceContact = true;
	double vground = Pi2 * sp.rad * sp.clat / planet->RotT();
	s0->vel.Set (mul (planet->s0->R, Vector (-vground*sp.slng, 0.0, vground*sp.clng)) + planet->s0->vel);
	s0->omega.Set (0,0,0);
	rvel_base = s0->vel, rvel_add.Set (0,0,0);
	rpos_base = s0->pos, rpos_add.Set (0,0,0);
	if (s1 != s0) s1->Set (*s0);
	cpos = s0->pos - cbody->GPos();
	cvel = s0->vel - cbody->GVel();
	el_valid = false;
}

void Debris::ContactStateChanged ()
{
	if (proxybody) UpdateSurfParams ();
	if (cbody) {
		cpos = s1->pos - cbody->s1->pos;
		cvel = s1->vel - cbody->s1->vel;
	}
}

// =======================================================================

void Debris::CollectHulls (const StateVectors &s, std::vector<collision::HullRef> &hl) const
{
	for (const collision::Hull &h : hulls)
		hl.push_back ({&h, s.R, s.pos});
}

void Debris::CollectGroundContacts (const StateVectors &s, const StateVectors &sb, int body, double h,
	std::vector<contact::Point> &pts) const
{
	touching = false;
	if (!proxybody || hulls.empty()) return;
	if (proxybody->Type() == OBJTP_PLANET && !((const Planet*)proxybody)->HasSurface()) return; // (gas giants)
	const StateVectors &P0 = *proxybody->s0, &P1 = *proxybody->s1;
	double alt = s.pos.dist (P0.pos) - proxybody->Size();
	double margin = (sp.groundspd + s.omega.length()*hullBound)*h + 0.05;
	if (alt - sp.elev > hullBound + margin + 1e3) return; // (no danger of surface contact)

	ElevationManager *emgr = (proxybody->Type() == OBJTP_PLANET ? ((Planet*)proxybody)->ElevMgr() : 0);
	int reslvl = 1;
	if (emgr) reslvl = (int)(32.0-log(std::max(alt,100.0))*LOG2);
	double wrot = Pi2 / proxybody->RotT();

	// the vertices of the hulls (with their skin) against the terrain
	for (const collision::Hull &hull : hulls)
		for (const Vector &v : hull.v) {
			Vector pg (s.pos + mul (s.R, v));     // global position
			Vector pl (tmul (P0.R, pg - P0.pos)); // planet-local position
			double lng, lat, rad, elev = 0.0;
			proxybody->LocalToEquatorial (pl, lng, lat, rad);
			Vector nh (0,1,0);
			if (emgr) elev = emgr->Elevation (lat, lng, reslvl, &etile, &nh);
			double slng = sin(lng), clng = cos(lng), slat = sin(lat), clat = cos(lat);
			Matrix L2H (-slng,0,clng, clat*clng,slat,clat*slng, -slat*clng,clat,-slat*slng);
			Vector nl (tmul (L2H, nh));
			double C = (rad - proxybody->Size() - elev) * dotp (nl, pl/rad) - collision::SKIN;
			if (C > margin) continue;
			if (C < 0.02) touching = true;
			contact::Point p;
			p.a = body, p.b = -1;
			p.ra = pg - sb.pos;
			p.n = mul (P0.R, nl);
			contact::Tangents (p.n, Vector (1,0,0), p.t1, p.t2);
			p.vs = P1.vel + mul (P1.R, Vector (-pl.z*wrot, 0, pl.x*wrot));
			p.C = C;
			p.k = p.c = 0.0;
			p.mu1 = p.mu2 = FRICTION;
			p.warm = nullptr;
			pts.push_back (p);
		}
}

// =======================================================================
// Scenarios

void Debris::Write (std::ostream &os) const
{
	for (const Group &g : groups)
		if (!g.mesh || !g.mesh->GetName() || !g.mesh->GetName()[0]) return; // (a mesh made by a module: not written)
	os << "PIECE " << name << '\n';
	os << "  SOURCE " << source << '\n';
	os << std::setprecision (10) << "  MASS " << mass << '\n';
	if (parts) os << "  PARTS " << parts << '\n';
	auto mat = [&](const Matrix &R) {
		for (int i = 0; i < 9; i++) os << ' ' << R.data[i];
		os << '\n';
	};
	if (AtRest() && proxyplanet) {
		os << "  REST " << proxyplanet->Name() << ' ' << sp.ploc.x << ' ' << sp.ploc.y << ' ' << sp.ploc.z << '\n';
		os << "  LROT";
		mat (land_rot);
	} else if (cbody) {
		Vector p (GPos() - cbody->GPos()), v (GVel() - cbody->GVel());
		os << "  STATE " << cbody->Name() << ' ' << p.x << ' ' << p.y << ' ' << p.z << ' ' << v.x << ' ' << v.y << ' ' << v.z << '\n';
		os << "  ROT";
		mat (s0->R);
		os << "  VROT " << s0->omega.x << ' ' << s0->omega.y << ' ' << s0->omega.z << '\n';
	}
	for (const Group &g : groups) {
		os << "  GROUP " << g.mesh->GetName() << ' ' << g.group << ' ' << g.ofs.x << ' ' << g.ofs.y << ' ' << g.ofs.z;
		// (triangle indices, runs as a-b)
		for (size_t i = 0; i < g.tris.size(); ) {
			size_t j = i;
			while (j + 1 < g.tris.size() && g.tris[j+1] == g.tris[j] + 1) j++;
			os << ' ' << g.tris[i];
			if (j > i) os << '-' << g.tris[j];
			i = j + 1;
		}
		os << '\n';
	}
	for (const collision::Hull &h : hulls) {
		os << "  HULL " << h.v.size();
		for (const Vector &v : h.v) os << ' ' << v.x << ' ' << v.y << ' ' << v.z;
		os << '\n';
	}
	os << "END\n";
}

Debris *Debris::Read (std::istream &is, const std::string &name)
{
	std::string line, source, ref;
	double m = 0.0;
	uint64_t parts = 0;
	bool rest = false, state = false;
	Vector p, v, w;
	Matrix R (1,0,0, 0,1,0, 0,0,1);
	std::vector<Group> groups;
	std::vector<std::vector<Vector>> hullPts;
	while (std::getline (is, line)) {
		std::istringstream ss (line);
		std::string tok;
		if (!(ss >> tok)) continue;
		if (tok == "END") break;
		if (tok == "SOURCE") ss >> source;
		else if (tok == "MASS") ss >> m;
		else if (tok == "PARTS") ss >> parts;
		else if (tok == "REST") { ss >> ref >> p.x >> p.y >> p.z; rest = true; }
		else if (tok == "STATE") { ss >> ref >> p.x >> p.y >> p.z >> v.x >> v.y >> v.z; state = true; }
		else if (tok == "ROT" || tok == "LROT") { for (int i = 0; i < 9; i++) ss >> R.data[i]; }
		else if (tok == "VROT") ss >> w.x >> w.y >> w.z;
		else if (tok == "GROUP") {
			Group g;
			std::string meshname;
			ss >> meshname >> g.group >> g.ofs.x >> g.ofs.y >> g.ofs.z;
			g.mesh = g_pOrbiter->LoadMeshGlobal (meshname.c_str());
			GroupSpec *grp = g.mesh ? const_cast<Mesh*>(g.mesh)->GetGroup (g.group) : nullptr;
			if (!grp) { LOGOUT_WARN ("Debris %s: no group %u in mesh %s", name.c_str(), (unsigned)g.group, meshname.c_str()); return nullptr; }
			std::string run;
			while (ss >> run) {
				unsigned a = 0, b = 0;
				int n = sscanf (run.c_str(), "%u-%u", &a, &b);
				if (n < 1) continue;
				if (n < 2) b = a;
				for (unsigned k = a; k <= b && 3*k+2 < grp->nIdx; k++) {
					g.tris.push_back (k);
					for (int j = 0; j < 3; j++) {
						NTVERTEX x = grp->Vtx[grp->Idx[3*k+j]];
						x.x += (float)g.ofs.x, x.y += (float)g.ofs.y, x.z += (float)g.ofs.z;
						g.vtx.push_back (x);
					}
				}
			}
			groups.push_back (std::move (g));
		}
		else if (tok == "HULL") {
			size_t n = 0;
			ss >> n;
			std::vector<Vector> pts (n);
			for (Vector &x : pts) ss >> x.x >> x.y >> x.z;
			hullPts.push_back (std::move (pts));
		}
	}
	CelestialBody *cb = g_psys->GetGravObj (ref.c_str());
	if (!cb || m <= 0.0 || (!rest && !state)) return nullptr;
	StateVectors s;
	if (rest) { // (on the turning ground)
		s.pos = cb->GPos() + mul (cb->GRot(), p);
		s.vel = cb->GVel();
		s.R.Set (cb->GRot());
		s.R.postmul (R);
	} else {
		s.pos = cb->GPos() + p, s.vel = cb->GVel() + v;
		s.R.Set (R);
		s.omega = w;
	}
	s.Q.Set (s.R);
	Debris *d = new Debris (name, source, m, Vector (0,0,0), s, std::move (groups), hullPts);
	d->SetParts (parts);
	if (rest && cb->Type() == OBJTP_PLANET) d->InitLanded ((Planet*)cb, 0.0, 0.0, 0.0, &R);
	return d;
}

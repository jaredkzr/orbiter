// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Trajectory.h"
#include "Orbiter.h"
#include "Psys.h"
#include "Celbody.h"
#include "Vessel.h"
#include "Element.h"
#include "Astro.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

extern TimeData td;
extern PlanetarySystem *g_psys;

using namespace trajectory;

namespace {
	const double RTOL = 1e-10;      // integration tolerance (relative to the distance from the reference body)
	const double ATOL_R = 0.1;      // [m]
	const double ATOL_V = 1e-4;     // [m/s]
	const double MAX_ANGLE = 1.5*RAD; // largest step as an angle about the reference body
	const size_t MAX_STEPS = 400000;
	const int MAX_SOI_CHANGES = 8;
	const double MOON_RANGE = 40.0;   // moons pull while the vessel is within this many orbit radii of their planet
}

// =======================================================================
// Inputs (a snapshot taken on the simulation thread)

struct Predictor::Input {
	struct BodySnap {
		const CelestialBody *body;
		int parent = -1;
		std::vector<int> children;
		double mass = 0.0, mu = 0.0, radius = 0.0;
		int nJ = 0;
		double J[3] = {0, 0, 0};
		Vector poleY, rotAxis;            // body frame y axis, rotation axis (global)
		bool analytic = false;
		std::shared_ptr<Elements> els;    // for bodies without ephemeris module
		Vector g0, gv0;                   // global state at t0
		double soi = 1e100;               // Laplace sphere radius
		double orbitRadius = 0.0;         // distance from the parent
		double hstep = 3600.0;            // ephemeris sample interval [s]
	};
	double t0 = 0.0;
	Vector r0, v0;                        // vessel state (global)
	std::vector<maneuver::Node> nodes;
	std::vector<double> nodeT;            // node times (simulation time)
	int burningNode = -1;                 // node 0 is being burnt: its remaining velocity change applies at t0
	Vector burnDvg;
	bool targetVessel = false;
	Vector tr0, tv0;                      // target vessel state (global)
	const Body *target = nullptr;
	int targetIdx = -1;                   // target celestial body index
	std::vector<BodySnap> bodies;
	double tmax = 0.0;
	unsigned int serial = 0;
	const Vessel *vessel = nullptr;
};

// =======================================================================
// Worker state: ephemeris caches (kept between predictions)

struct Predictor::Context {
	struct Sample { Vector p, v; };
	// relative true states (to the parent's true position) on each body's sample grid
	std::unordered_map<const CelestialBody*, std::unordered_map<long long, Sample>> cache;
	size_t nsample = 0;

	// per evaluation
	const Input *in = nullptr;
	std::vector<Vector> rel0, relv0;  // relative states at t0 (interpolated, for the offsets)
	std::vector<Vector> G, GV;        // global states at the evaluation time
	std::vector<char> active;         // bodies evaluated at the current time
	double tEval = -1e100;

	// raw ephemeris at time t (memoised per sample computation)
	std::unordered_map<const CelestialBody*, CelestialBody::EphemState> eph;
	double ephT = -1e100;

	const CelestialBody::EphemState &Eph (int i, double t)
	{
		if (t != ephT) eph.clear(), ephT = t;
		const auto &b = in->bodies[i];
		auto it = eph.find (b.body);
		if (it != eph.end()) return it->second;
		CelestialBody::EphemState s;
		if (!b.body->EphemerisAtTime (t, s, b.els.get())) s = CelestialBody::EphemState();
		return eph[b.body] = s;
	}

	// barycentre of a body's system relative to its true position
	void BaryOfs (int i, double t, Vector &p, Vector &v)
	{
		const auto &e = Eph (i, t);
		if (e.havetrue && e.havebary) {
			p = e.bpos - e.cpos, v = e.bvel - e.cvel;
			return;
		}
		const auto &b = in->bodies[i];
		double m = b.mass;
		p.Set (0, 0, 0), v.Set (0, 0, 0);
		for (int c : b.children) {
			const auto &ec = Eph (c, t);
			Vector cp, cv;
			if (ec.havebary) cp = ec.bpos, cv = ec.bvel;
			else if (ec.havetrue) { Vector op, ov; BaryOfs (c, t, op, ov); cp = ec.cpos + op, cv = ec.cvel + ov; }
			else continue;
			double mc = in->bodies[c].mass;
			p += cp * mc, v += cv * mc;
			if (!ec.parentbary) m += mc;
		}
		p /= m, v /= m;
	}

	// true state relative to the parent's true position (as the simulation computes it)
	Sample RelTrue (int i, double t)
	{
		const auto &e = Eph (i, t);
		Sample s;
		if (e.havetrue) s.p = e.cpos, s.v = e.cvel;
		else if (e.havebary) {
			Vector op, ov;
			BaryOfs (i, t, op, ov);
			s.p = e.bpos - op, s.v = e.bvel - ov;
		}
		int par = in->bodies[i].parent;
		if (e.parentbary && par >= 0) {
			Vector op, ov;
			BaryOfs (par, t, op, ov);
			s.p += op, s.v += ov;
		}
		return s;
	}

	// relative state at time t, interpolated from the sample grid
	void Rel (int i, double t, Vector &p, Vector &v)
	{
		const auto &b = in->bodies[i];
		double h = b.hstep;
		long long k = (long long)floor (t / h);
		auto &c = cache[b.body];
		auto get = [&](long long kk) -> const Sample& {
			auto it = c.find (kk);
			if (it != c.end()) return it->second;
			nsample++;
			return c[kk] = RelTrue (i, kk * h);
		};
		const Sample &s0 = get (k);
		const Sample &s1 = get (k + 1);
		double s = (t - k * h) / h, s2 = s*s, s3 = s2*s;
		double h00 = 2*s3 - 3*s2 + 1, h10 = s3 - 2*s2 + s, h01 = -2*s3 + 3*s2, h11 = s3 - s2;
		double d00 = 6*s2 - 6*s, d10 = 3*s2 - 4*s + 1, d01 = -6*s2 + 6*s, d11 = 3*s2 - 2*s;
		p = s0.p * h00 + s0.v * (h10 * h) + s1.p * h01 + s1.v * (h11 * h);
		v = (s0.p * d00 + s1.p * d01) / h + s0.v * d10 + s1.v * d11;
	}

	void Begin (const Input &input)
	{
		in = &input;
		size_t n = in->bodies.size();
		rel0.assign (n, Vector()), relv0.assign (n, Vector());
		G.assign (n, Vector()), GV.assign (n, Vector());
		active.assign (n, 0);
		eph.clear(), ephT = -1e100;
		tEval = -1e100;
		for (size_t i = 0; i < n; i++)
			if (in->bodies[i].analytic) Rel ((int)i, in->t0, rel0[i], relv0[i]);
		if (nsample > 2000000) cache.clear(), nsample = 0; // (bound the memory)
	}

	// global states of the bodies acting on a vessel at r at time t (parents first)
	void Bodies (double t, const Vector &r)
	{
		size_t n = in->bodies.size();
		double dt = t - in->t0;
		for (size_t i = 0; i < n; i++) {
			const auto &b = in->bodies[i];
			bool act = true;
			if (b.parent >= 0 && in->bodies[b.parent].parent >= 0) {
				// a moon: only near its planet
				int par = b.parent;
				act = active[par] && (r - G[par]).length() < MOON_RANGE * b.orbitRadius;
			}
			active[i] = act;
			if (!act) continue;
			if (!b.analytic) { // (moves with its initial velocity)
				G[i] = b.g0 + b.gv0 * dt, GV[i] = b.gv0;
				continue;
			}
			Vector p, v;
			Rel ((int)i, t, p, v);
			// global = initial global + change of the relative state (and of the parent's)
			Vector dp (p - rel0[i]), dv (v - relv0[i]);
			if (b.parent >= 0) {
				const auto &pb = in->bodies[b.parent];
				dp += G[b.parent] - pb.g0;
				dv += GV[b.parent] - pb.gv0;
			}
			G[i] = b.g0 + dp, GV[i] = b.gv0 + dv;
		}
		tEval = t;
	}

	Vector Accel (double t, const Vector &r)
	{
		if (t != tEval) Bodies (t, r);
		Vector acc;
		for (size_t i = 0; i < in->bodies.size(); i++) {
			if (!active[i]) continue;
			const auto &b = in->bodies[i];
			Vector rp (G[i] - r);
			double d = rp.length();
			if (d < 1.0) continue;
			acc += rp * (b.mu / (d*d*d));
			if (b.nJ) acc += Zonal (b, rp, d);
		}
		return acc;
	}

	// J2-J4 perturbation (as SingleGacc_perturbation)
	static Vector Zonal (const Input::BodySnap &b, const Vector &rpos, double d)
	{
		const double eps = 1e-10;
		double Rr = b.radius / d, Rrn = Rr*Rr;
		double Jn = b.J[0] * Rrn;
		if (fabs (Jn) <= eps) return Vector();
		Vector er (rpos / d);
		double slat = -dotp (er, b.poleY), clat = sqrt (std::max (0.0, 1.0 - slat*slat));
		double gr = 1.5 * Jn * (1.0 - 3.0*slat*slat), gp = 3.0 * Jn * clat*slat;
		if (b.nJ > 1) {
			Rrn *= Rr;
			Jn = b.J[1] * Rrn;
			if (fabs (Jn) > eps) {
				gr += 2.0 * Jn * slat * (3.0 - 5.0*slat*slat);
				gp += 1.5 * Jn * clat * (-1.0 + 5.0*slat*slat);
				if (b.nJ > 2) {
					Rrn *= Rr;
					Jn = b.J[2] * Rrn;
					if (fabs (Jn) > eps) {
						gr += -0.625 * Jn * (3.0 + slat*slat*(-30.0 + 35.0*slat*slat));
						gp += 2.5 * Jn * clat*slat * (-3.0 + 7.0*slat*slat);
					}
				}
			}
		}
		double T0 = b.mu / (d*d);
		Vector ep, ea (crossp (er, b.rotAxis));
		double lea = ea.length();
		if (lea > eps) ep = crossp (er, ea / lea);
		return er * (T0*gr) + ep * (T0*gp);
	}

	// reference body: the deepest body whose sphere of influence contains r
	// (requires Bodies at this time)
	int Ref (const Vector &r) const
	{
		int cur = -1;
		for (size_t i = 0; i < in->bodies.size(); i++)
			if (in->bodies[i].parent < 0) { cur = (int)i; break; }
		for (;;) {
			int next = -1;
			for (int c : in->bodies[cur].children)
				if (active[c] && (r - G[c]).length() < in->bodies[c].soi) { next = c; break; }
			if (next < 0) return cur;
			cur = next;
		}
	}
};

// =======================================================================

Predictor::Predictor ()
{
	ctx = std::make_unique<Context>();
	worker = std::thread (&Predictor::Run, this);
}

Predictor::~Predictor ()
{
	{
		std::lock_guard<std::mutex> lock (mtx);
		quit = true;
		abort = true;
	}
	cv.notify_all();
	if (worker.joinable()) worker.join();
}

std::shared_ptr<const Prediction> Predictor::Latest () const
{
	std::lock_guard<std::mutex> lock (mtx);
	return latest;
}

void Predictor::Reset ()
{
	{
		std::lock_guard<std::mutex> lock (mtx);
		pending.reset();
		latest.reset();
		abort = true;
	}
	lastVessel = nullptr;
	lastTarget = nullptr;
	lastNodeVersion = ~0u;
}

void Predictor::Update (const Vessel *v, const Body *target)
{
	if (!v) return;
	double sys = td.SysT1, sim = td.SimT1;
	bool changed = (v != lastVessel || target != lastTarget || v->ManeuverVersion() != lastNodeVersion);
	bool thrust = v->ThrustEngaged() || v->Burn().phase == Vessel::BurnState::BURN;
	auto cur = Latest();
	bool stale = cur && sim > cur->t0 + 0.5 * (cur->t1 - cur->t0);
	bool due = (thrust ? sys - lastRequestSys > 0.2 : sys - lastRequestSys > 5.0) ||
		(stale && sys - lastRequestSys > 0.2) || (!cur && sys - lastRequestSys > 1.0);
	if (!changed && !due) return;
	if (v->GetStatus() != FLIGHTSTATUS_FREEFLIGHT) return; // (landed: nothing to predict)

	auto in = std::make_shared<Input>();
	in->t0 = sim;
	in->vessel = v;
	in->r0 = v->GPos(), in->v0 = v->GVel();
	in->nodes = v->ManeuverNodes();
	for (const auto &n : in->nodes) in->nodeT.push_back ((n.mjd - td.MJD_ref) * 86400.0);
	if (v->Burn().phase == Vessel::BurnState::BURN && !in->nodes.empty()) {
		in->burningNode = 0;
		in->burnDvg = v->Burn().dvg;
	}
	in->target = target;
	in->tmax = sim + maxDays * 86400.0;

	size_t n = g_psys->nGrav();
	in->bodies.resize (n);
	for (size_t i = 0; i < n; i++) {
		const CelestialBody *cb = g_psys->GetGravObj ((int)i);
		auto &b = in->bodies[i];
		b.body = cb;
		b.mass = cb->Mass();
		b.mu = Ggrav * b.mass;
		b.radius = cb->Size();
		if (cb->UseComplexGravity() && !cb->usePines()) {
			b.nJ = (int)std::min<DWORD> (3, cb->nJcoeff());
			for (int j = 0; j < b.nJ; j++) b.J[j] = cb->Jcoeff (j);
		}
		b.poleY = mul (cb->GRot(), Vector (0, 1, 0));
		b.rotAxis = cb->RotAxis();
		CelestialBody::EphemState es;
		b.analytic = cb->HasEphemerisModule() || (cb->ElementsPtr() && cb->EphemerisAtTime (sim, es));
		if (!cb->HasEphemerisModule() && cb->ElementsPtr())
			b.els = std::make_shared<Elements> (*cb->ElementsPtr());
		b.g0 = cb->GPos(), b.gv0 = cb->GVel();
		if (cb == target) in->targetIdx = (int)i;
	}
	for (size_t i = 0; i < n; i++) {
		auto &b = in->bodies[i];
		const CelestialBody *p = b.body->Primary();
		for (size_t j = 0; j < n && p; j++)
			if (in->bodies[j].body == p) { b.parent = (int)j; break; }
		if (b.parent >= 0) {
			auto &pb = in->bodies[b.parent];
			pb.children.push_back ((int)i);
			b.orbitRadius = (b.g0 - pb.g0).length();
			b.soi = b.orbitRadius * pow (b.mass / pb.mass, 0.4);
			double period = 2.0 * Pi * sqrt (pow (b.orbitRadius, 3) / (pb.mu + b.mu));
			b.hstep = std::clamp (period / 400.0, 30.0, 2.0 * 86400.0);
		} else {
			b.hstep = 2.0 * 86400.0;
		}
	}
	// parents before children (bodies are evaluated in this order)
	for (size_t i = 0; i < n; i++)
		for (int c : in->bodies[i].children)
			if (c < (int)i) {
				LOGOUT_WARN ("Trajectory prediction: body order not parent-first; skipping");
				return;
			}

	if (const Vessel *tv = dynamic_cast<const Vessel*> (target)) {
		in->targetVessel = true;
		in->tr0 = tv->GPos(), in->tv0 = tv->GVel();
	}
	in->serial = ++serial;

	{
		std::lock_guard<std::mutex> lock (mtx);
		pending = in;
		abort = true; // (a running prediction is outdated)
	}
	cv.notify_all();
	lastVessel = v;
	lastTarget = target;
	lastNodeVersion = v->ManeuverVersion();
	lastRequestSys = sys;
	lastRequestSim = sim;
}

void Predictor::Run ()
{
	for (;;) {
		std::shared_ptr<Input> in;
		{
			std::unique_lock<std::mutex> lock (mtx);
			cv.wait (lock, [this] { return quit || pending; });
			if (quit) return;
			in = pending;
			pending.reset();
			abort = false;
		}
		Predict (*in, *ctx);
	}
}

// =======================================================================
// The prediction: Dormand-Prince 5(4) integration of the vessel (and the
// target vessel), nodes applied at their times

namespace {
	struct PState { Vector r, v; };

	// Dormand-Prince coefficients
	const double c2 = 1.0/5, c3 = 3.0/10, c4 = 4.0/5, c5 = 8.0/9;
	const double a21 = 1.0/5;
	const double a31 = 3.0/40, a32 = 9.0/40;
	const double a41 = 44.0/45, a42 = -56.0/15, a43 = 32.0/9;
	const double a51 = 19372.0/6561, a52 = -25360.0/2187, a53 = 64448.0/6561, a54 = -212.0/729;
	const double a61 = 9017.0/3168, a62 = -355.0/33, a63 = 46732.0/5247, a64 = 49.0/176, a65 = -5103.0/18656;
	const double b1 = 35.0/384, b3 = 500.0/1113, b4 = 125.0/192, b5 = -2187.0/6784, b6 = 11.0/84;
	const double e1 = 71.0/57600, e3 = -71.0/16695, e4 = 71.0/1920, e5 = -17253.0/339200, e6 = 22.0/525, e7 = -1.0/40;
}

void Predictor::Predict (const Input &in, Context &c)
{
	auto pred = std::make_shared<Prediction>();
	pred->t0 = in.t0;
	pred->serial = in.serial;
	pred->targetBody = in.target;
	pred->vessel = in.vessel;
	c.Begin (in);

	int nst = in.targetVessel ? 2 : 1;
	PState y[2] = { {in.r0, in.v0}, {in.tr0, in.tv0} };
	double t = in.t0;
	size_t nextNode = 0;
	while (nextNode < in.nodeT.size() && in.nodeT[nextNode] < t && (int)nextNode != in.burningNode) nextNode++;

	// the remaining velocity change of a burn in progress applies now
	if (in.burningNode == 0 && nextNode == 0) {
		c.Bodies (t, y[0].r);
		int ref = c.Ref (y[0].r);
		NodeState ns{t, in.bodies[ref].body, y[0].r - c.G[ref], y[0].v - c.GV[ref], in.burnDvg};
		pred->nodes.push_back (ns);
		y[0].v += in.burnDvg;
		nextNode = 1;
	}
	while (pred->nodes.size() < nextNode) // (skipped nodes keep their places)
		pred->nodes.push_back (NodeState{in.nodeT[pred->nodes.size()], nullptr, Vector(), Vector(), Vector()});

	auto RefState = [&](int ref, const PState &s, Vector &rr, Vector &rv) {
		rr = s.r - c.G[ref], rv = s.v - c.GV[ref];
	};

	c.Bodies (t, y[0].r);
	int ref[2] = { c.Ref (y[0].r), 0 };
	if (nst > 1) { c.Bodies (t, y[1].r); ref[1] = c.Ref (y[1].r); c.Bodies (t, y[0].r); }
	auto NewSeg = [&](std::vector<Segment> &segs, int r, int node) {
		int par = in.bodies[r].parent;
		segs.push_back (Segment{in.bodies[r].body, node, {}, c.G[r], par >= 0 ? c.G[par] : Vector()});
	};
	NewSeg (pred->segs, ref[0], (int)nextNode - 1);
	if (nst > 1) NewSeg (pred->target, ref[1], -1);
	auto AddPoint = [&](std::vector<Segment> &segs, int r, const PState &s, double tt) {
		Vector rr (s.r - c.G[r]), rv (s.v - c.GV[r]);
		segs.back().pts.push_back (Point{tt, rr, rv});
	};
	AddPoint (pred->segs, ref[0], y[0], t);
	if (nst > 1) { c.Bodies (t, y[1].r); AddPoint (pred->target, ref[1], y[1], t); c.Bodies (t, y[0].r); }

	// closest approach tracking
	double prevDist = -1.0, prevRdot = 0.0;
	auto TargetRel = [&](double tt, const PState *s, Vector &dr, Vector &dv) -> bool {
		if (in.targetIdx >= 0) { dr = s[0].r - c.G[in.targetIdx]; dv = s[0].v - c.GV[in.targetIdx]; return c.active[in.targetIdx]; }
		if (nst > 1) { dr = s[0].r - s[1].r; dv = s[0].v - s[1].v; return true; }
		return false;
	};

	double sweep = 0.0;   // angle about the reference body since the last node or SOI change
	int soiChanges = 0;
	double h = 10.0;
	Vector k[7][2][2];    // stage derivatives [stage][state][r/v]
	auto Deriv = [&](double tt, const PState *s, Vector out[2][2]) {
		for (int i = 0; i < nst; i++) {
			out[i][0] = s[i].v;
			if (i > 0) c.tEval = -1e100; // (the bodies' activity depends on the position)
			out[i][1] = c.Accel (tt, s[i].r);
		}
		if (nst > 1) c.tEval = -1e100;
	};
	Deriv (t, y, k[0]);

	size_t steps = 0;
	bool impact = false;
	while (!abort && steps++ < MAX_STEPS) {
		// step size limits: angle about the reference body, the next node, the end
		Vector rr, rv;
		c.Bodies (t, y[0].r);
		RefState (ref[0], y[0], rr, rv);
		double w = rv.length() / std::max (rr.length(), 1.0);
		double hmax = MAX_ANGLE / std::max (w, 1e-12);
		hmax = std::min (hmax, 5.0 * 86400.0);
		bool toNode = false;
		if (nextNode < in.nodeT.size() && t + std::min (h, hmax) >= in.nodeT[nextNode]) {
			hmax = in.nodeT[nextNode] - t;
			toNode = true;
		}
		double hs = std::min (h, hmax);
		if (hs <= 0.0) hs = 1e-3;

		// Dormand-Prince step
		PState ys[2], y5[2];
		auto Stage = [&](int st, double ct, std::initializer_list<std::pair<int,double>> a) {
			for (int i = 0; i < nst; i++) {
				ys[i] = y[i];
				for (auto &p : a) {
					ys[i].r += k[p.first][i][0] * (hs * p.second);
					ys[i].v += k[p.first][i][1] * (hs * p.second);
				}
			}
			Deriv (t + ct * hs, ys, k[st]);
		};
		Stage (1, c2, {{0, a21}});
		Stage (2, c3, {{0, a31}, {1, a32}});
		Stage (3, c4, {{0, a41}, {1, a42}, {2, a43}});
		Stage (4, c5, {{0, a51}, {1, a52}, {2, a53}, {3, a54}});
		Stage (5, 1.0, {{0, a61}, {1, a62}, {2, a63}, {3, a64}, {4, a65}});
		for (int i = 0; i < nst; i++) {
			y5[i].r = y[i].r + (k[0][i][0]*b1 + k[2][i][0]*b3 + k[3][i][0]*b4 + k[4][i][0]*b5 + k[5][i][0]*b6) * hs;
			y5[i].v = y[i].v + (k[0][i][1]*b1 + k[2][i][1]*b3 + k[3][i][1]*b4 + k[4][i][1]*b5 + k[5][i][1]*b6) * hs;
		}
		Deriv (t + hs, y5, k[6]);
		double err = 0.0;
		for (int i = 0; i < nst; i++) {
			Vector er = (k[0][i][0]*e1 + k[2][i][0]*e3 + k[3][i][0]*e4 + k[4][i][0]*e5 + k[5][i][0]*e6 + k[6][i][0]*e7) * hs;
			Vector ev = (k[0][i][1]*e1 + k[2][i][1]*e3 + k[3][i][1]*e4 + k[4][i][1]*e5 + k[5][i][1]*e6 + k[6][i][1]*e7) * hs;
			c.Bodies (t + hs, y5[i].r);
			Vector sr, sv;
			RefState (ref[i], y5[i], sr, sv);
			err = std::max (err, er.length() / (ATOL_R + RTOL * sr.length()));
			err = std::max (err, ev.length() / (ATOL_V + RTOL * sv.length()));
		}
		double fac = 0.9 * pow (std::max (err, 1e-10), -0.2);
		if (err > 1.0) { // reject
			h = hs * std::max (0.2, fac);
			Deriv (t, y, k[0]);
			continue;
		}
		h = hs * std::min (5.0, fac);
		if (toNode && hs < hmax) toNode = false; // (stopped short of the node)

		PState yp[2] = { y[0], y[1] };
		double tp = t;
		t += hs;
		y[0] = y5[0], y[1] = y5[1];
		for (int i = 0; i < 2; i++) k[0][i][0] = k[6][i][0], k[0][i][1] = k[6][i][1];

		// vessel: reference body, events
		c.Bodies (t, y[0].r);
		int nref = c.Ref (y[0].r);
		Vector r1, v1;
		RefState (ref[0], y[0], r1, v1);
		double rdot1 = dotp (r1, v1);
		Vector r0p (yp[0].r), v0p (yp[0].v);
		{
			// previous state relative to the (unchanged) reference at the previous time
			c.Bodies (tp, yp[0].r);
			Vector pr, pv;
			RefState (ref[0], yp[0], pr, pv);
			double rdot0 = dotp (pr, pv);
			sweep += acos (std::clamp (dotp (pr.unit(), r1.unit()), -1.0, 1.0));
			const auto &rb = in.bodies[ref[0]];
			if ((rdot0 < 0.0) != (rdot1 < 0.0)) {
				double f = rdot0 / (rdot0 - rdot1);
				Vector re (pr + (r1 - pr) * f);
				Event::Type type = (rdot0 < 0.0 ? Event::PERIAPSIS : Event::APOAPSIS);
				pred->events.push_back (Event{type, tp + f * hs, rb.body, re, re.length() - rb.radius, 0.0, pred->segs.back().node, (int)pred->segs.size() - 1});
			}
			if (r1.length() < rb.radius) { // impact
				double d0 = pr.length() - rb.radius, d1 = r1.length() - rb.radius;
				double f = d0 / (d0 - d1);
				Vector re (pr + (r1 - pr) * f);
				pred->events.push_back (Event{Event::IMPACT, tp + f * hs, rb.body, re, 0.0, 0.0, pred->segs.back().node, (int)pred->segs.size() - 1});
				impact = true;
			}
			c.Bodies (t, y[0].r);
		}

		// closest approach to the target (local minima of the distance)
		Vector dr, dvv;
		if (TargetRel (t, y, dr, dvv)) {
			double d = dr.length(), rd = dotp (dr, dvv);
			if (prevDist >= 0.0 && prevRdot < 0.0 && rd >= 0.0) {
				double f = prevRdot / (prevRdot - rd);
				double tc = tp + f * hs;
				double dc = std::min (d, prevDist);
				Vector rel;
				RefState (ref[0], y[0], rel, v1);
				pred->events.push_back (Event{Event::CLOSEST, tc, in.bodies[ref[0]].body, rel, dc, dvv.length(), pred->segs.back().node, (int)pred->segs.size() - 1});
			}
			prevDist = d, prevRdot = rd;
		}

		if (nref != ref[0]) { // sphere of influence change
			bool enter = in.bodies[nref].parent == ref[0];
			const auto &nb = in.bodies[nref];
			Vector rel (y[0].r - c.G[nref]);
			pred->events.push_back (Event{enter ? Event::SOI_ENTER : Event::SOI_EXIT, t, nb.body, rel,
				rel.length() - nb.radius, 0.0, pred->segs.back().node, (int)pred->segs.size()}); // (in the next segment)
			AddPoint (pred->segs, ref[0], y[0], t); // (end of the old segment)
			ref[0] = nref;
			NewSeg (pred->segs, ref[0], pred->segs.back().node);
			sweep = 0.0;
			soiChanges++;
		}
		AddPoint (pred->segs, ref[0], y[0], t);

		if (nst > 1) {
			c.tEval = -1e100;
			c.Bodies (t, y[1].r);
			int tref = c.Ref (y[1].r);
			if (tref != ref[1]) {
				AddPoint (pred->target, ref[1], y[1], t);
				ref[1] = tref;
				NewSeg (pred->target, ref[1], -1);
			}
			AddPoint (pred->target, ref[1], y[1], t);
			c.tEval = -1e100;
			c.Bodies (t, y[0].r);
		}

		// node: apply the velocity change
		if (toNode) {
			const auto &node = in.nodes[nextNode];
			Vector rr2, rv2;
			RefState (ref[0], y[0], rr2, rv2);
			Vector dvg = maneuver::ToGlobal (node.dv, rr2, rv2);
			pred->nodes.push_back (NodeState{t, in.bodies[ref[0]].body, rr2, rv2, dvg});
			y[0].v += dvg;
			nextNode++;
			NewSeg (pred->segs, ref[0], (int)nextNode - 1);
			AddPoint (pred->segs, ref[0], y[0], t);
			sweep = 0.0;
			c.tEval = -1e100;
			Deriv (t, y, k[0]);
		}

		// end: impact, horizon, or a full revolution after the last node
		if (impact || t >= in.tmax || soiChanges > MAX_SOI_CHANGES) break;
		if (nextNode >= in.nodeT.size() && sweep >= 2.0 * Pi) {
			// (bound orbit completed; not if an SOI exit is still ahead)
			Vector rr3, rv3;
			RefState (ref[0], y[0], rr3, rv3);
			double mu = in.bodies[ref[0]].mu;
			double energy = 0.5 * dotp (rv3, rv3) - mu / rr3.length();
			if (energy < 0.0) {
				double a = -mu / (2.0 * energy);
				if (a < in.bodies[ref[0]].soi || in.bodies[ref[0]].parent < 0) break;
			}
			if (sweep >= 4.0 * Pi) break;
		}
	}
	if (abort) return;

	pred->t1 = t;
	pred->complete = true;
	std::lock_guard<std::mutex> lock (mtx);
	latest = pred;
}

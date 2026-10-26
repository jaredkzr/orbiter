// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "VesselCollision.h"
#include "Celbody.h"
#include "Planet.h"
#include "Base.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace {
	const double FRICTION = 0.4;        // friction coefficient between vessels
	// two free docking ports closer than PORT_RANGE, approaching each other
	// within the docking limits (speed, alignment, PORT_OFFSET off each
	// other's axis): no contacts within PORT_CLEARANCE of them (the vessels
	// are docking; too fast or askew, they collide)
	const double PORT_RANGE = 5.0;      // [m]
	const double PORT_OFFSET = 1.0;     // [m]
	const double PORT_CLEARANCE = 1.5;  // [m]

	// bounding sphere of a vessel's hulls (about its centre of gravity)
	double HullRadius (const std::vector<collision::HullRef> &hl, const Vector &cg)
	{
		double r = 0.0;
		for (const auto &h : hl)
			r = std::max (r, (h.pos + mul (h.R, h.hull->centre) - cg).length() + h.hull->radius + collision::SKIN);
		return r;
	}

	// relative speed of two bodies' furthest points (bound)
	double SpeedBound (VesselBase *a, VesselBase *b, double ra, double rb)
	{
		return (a->GVel() - b->GVel()).length() + a->AngularVelocity().length()*ra + b->AngularVelocity().length()*rb;
	}
}

// ==============================================================

void VesselCollider::Begin (const std::vector<VesselBase*> &bodies, double h)
{
	pairs.clear();
	basePairs.clear();
	dynamic.clear();
	if (h <= 0.0) return;

	// candidates: vessels with hulls, moving or at rest on the ground
	struct Cand { VesselBase *v; double r; bool dyn; };
	std::vector<Cand> cand;
	// (releases of vessels that no longer exist, or are attached, docked)
	releases.erase (std::remove_if (releases.begin(), releases.end(), [&](const Release &r) {
		bool fa = false, fb = false;
		for (VesselBase *v : bodies) {
			if (v->IsComponent()) continue;
			if (v == r.a) fa = true;
			if (v == r.b) fb = true;
		}
		return !fa || !fb || r.clear; }), releases.end());

	for (VesselBase *v : bodies) {
		v->contact_deferred = false;
		if (v->IsComponent()) continue;
		bool dyn = v->fstatus == FLIGHTSTATUS_FREEFLIGHT && !v->InPlayback();
		if (!dyn && v->fstatus != FLIGHTSTATUS_LANDED) continue;
		double r = v->CollisionRadius();
		if (r < 0.0) continue;
		cand.push_back ({v, r, dyn});
	}

	for (size_t i = 0; i < cand.size(); i++)
		for (size_t j = i+1; j < cand.size(); j++) {
			const Cand &a = cand[i], &b = cand[j];
			if (!a.dyn && !b.dyn) continue;
			double d = a.v->s0->pos.dist (b.v->s0->pos);
			double reach = a.r + b.r + SpeedBound (a.v, b.v, a.r, b.r)*h + 0.1;
			if (d > reach) continue;
			pairs.push_back ({a.v, b.v});
			for (const Cand *c : {&a, &b})
				if (c->dyn && std::find (dynamic.begin(), dynamic.end(), c->v) == dynamic.end()) {
					dynamic.push_back (c->v);
					c->v->contact_deferred = true;
				}
		}

	// moving vessels near the structures of surface bases
	for (const Cand &c : cand) {
		if (!c.dyn || !c.v->proxybody || c.v->proxybody->Type() != OBJTP_PLANET) continue;
		const Planet *pl = (const Planet*)c.v->proxybody;
		for (DWORD i = 0; i < pl->nBase(); i++) {
			const Base *b = pl->GetBase (i);
			double reach = c.r + c.v->GVel().dist (b->GVel())*h + 0.1;
			// (cheap test with the base's size first: its hulls are made when first needed)
			if (c.v->s0->pos.dist (b->GPos()) > b->Size() + 2e3 + reach) continue;
			if (b->CollisionHulls().empty() || c.v->s0->pos.dist (b->GPos()) > b->CollisionRadius() + reach) continue;
			basePairs.push_back ({c.v, b});
			if (std::find (dynamic.begin(), dynamic.end(), c.v) == dynamic.end()) {
				dynamic.push_back (c.v);
				c.v->contact_deferred = true;
			}
		}
	}
}

// ==============================================================

void VesselCollider::Resolve (double h)
{
	if (pairs.empty() && basePairs.empty()) return;

	// bodies: the moving vessels of the pairs; positions relative to the
	// first of them (precision)
	const Vector O (dynamic[0]->s0->pos);
	std::map<VesselBase*, int> index;
	std::vector<contact::Body> bodies;
	for (VesselBase *v : dynamic) {
		index[v] = (int)bodies.size();
		bodies.push_back (v->ContactBody());
	}

	std::vector<contact::Point> pts;
	std::vector<VesselBase*> owner; // (points against a vessel at rest: that vessel)
	// their ground contacts
	for (VesselBase *v : dynamic)
		if (v->proxybody) v->CollectGroundContacts (*v->s0, *v->s0, index[v], h, pts);

	// velocity of a point of a vessel at rest on the ground (global position g)
	auto LandedVel = [](VesselBase *v, const Vector &g) {
		const CelestialBody *cb = v->proxybody;
		if (!cb) return v->s1->vel;
		Vector pl (tmul (cb->s0->R, g - cb->s0->pos));
		double wrot = Pi2 / cb->RotT();
		return cb->s1->vel + mul (cb->s1->R, Vector (-pl.z*wrot, 0, pl.x*wrot));
	};

	// contacts between the hulls of each pair
	std::vector<collision::HullRef> ha, hb;
	std::vector<collision::Port> pa, pb;
	std::vector<Vector> excl;
	std::vector<bool> landed;
	for (const Pair &pr : pairs) {
		VesselBase *A = pr.a, *B = pr.b;
		ha.clear(); hb.clear();
		A->CollectHulls (*A->s0, ha);
		B->CollectHulls (*B->s0, hb);

		// (no contacts close to free docking ports approaching each other)
		pa.clear(); pb.clear(); excl.clear();
		A->CollectFreePorts (*A->s0, pa);
		B->CollectFreePorts (*B->s0, pb);
		for (const collision::Port &p : pa)
			for (const collision::Port &q : pb) {
				if (p.pos.dist (q.pos) > PORT_RANGE || collision::DockCapture (p, q, PORT_OFFSET)) continue;
				excl.push_back (p.pos - O), excl.push_back (q.pos - O);
			}

		double ra = HullRadius (ha, A->s0->pos), rb = HullRadius (hb, B->s0->pos);
		double margin = SpeedBound (A, B, ra, rb)*h + 0.05;
		// (only the hulls that reach the other vessel's bounding sphere)
		auto Near = [&](std::vector<collision::HullRef> &hl, const Vector &c, double r) {
			hl.erase (std::remove_if (hl.begin(), hl.end(), [&](const collision::HullRef &x) {
				return (x.pos + mul (x.R, x.hull->centre)).dist (c) > x.hull->radius + collision::SKIN + r + margin; }), hl.end());
		};
		Near (ha, B->s0->pos, rb);
		Near (hb, A->s0->pos, ra);
		bool dynA = index.count (A) > 0, dynB = index.count (B) > 0;
		Vector cgA (A->s0->pos - O), cgB (B->s0->pos - O);

		// (a payload released from the other: hull pairs overlapping since)
		Release *rel = nullptr;
		for (Release &r : releases)
			if ((r.a == A && r.b == B) || (r.a == B && r.b == A)) rel = &r;
		bool touching = false;

		// contacts of hull x (of vessel X) with hull y (of vessel Y)
		convex::Manifold mf;
		auto Test = [&](VesselBase *X, const collision::HullRef &x, bool dynX, const Vector &cgX,
		                VesselBase *Y, const collision::HullRef &y, bool dynY, const Vector &cgY) {
			convex::Placed px{x.hull, x.R, x.pos - O, collision::SKIN}, py{y.hull, y.R, y.pos - O, collision::SKIN};
			Vector cx (px.pos + mul (x.R, x.hull->centre)), cy (py.pos + mul (y.R, y.hull->centre));
			if (cx.dist (cy) > x.hull->radius + y.hull->radius + 2.0*collision::SKIN + margin) return;
			if (!convex::Collide (px, py, margin, mf)) return;
			if (rel) {
				// (released from each other: no contacts while any parts touch)
				for (int k = 0; k < mf.npt; k++) if (mf.sep[k] < collision::SKIN) touching = true;
				return;
			}
			for (int k = 0; k < mf.npt; k++) {
				const Vector &g = mf.pt[k];        // the point (relative to O)
				bool skip = false;
				for (const Vector &e : excl) if (g.dist (e) < PORT_CLEARANCE) { skip = true; break; }
				if (skip) continue;
				contact::Point p;
				p.C = mf.sep[k];
				p.k = p.c = 0.0;
				p.mu1 = p.mu2 = FRICTION;
				p.warm = nullptr;
				if (dynX) {
					p.a = index[X], p.ra = g - cgX, p.n = mf.n;   // (out of y, towards x)
					if (dynY) p.b = index[Y], p.rb = g - cgY;
					else      p.b = -1, p.vs = LandedVel (Y, g + O);
				} else {    // (X at rest: the contact acts on Y)
					p.a = index[Y], p.ra = g - cgY, p.n = -mf.n;
					p.b = -1, p.vs = LandedVel (X, g + O);
				}
				contact::Tangents (p.n, Vector(1,0,0), p.t1, p.t2);
				owner.resize (pts.size(), nullptr);
				pts.push_back (p);
				owner.push_back (!dynX ? X : !dynY ? Y : nullptr);
			}
		};
		for (const auto &x : ha)
			for (const auto &y : hb)
				Test (A, x, dynA, cgA, B, y, dynB, cgB);
		if (rel && !touching) rel->clear = true; // (apart: they collide from now on)
	}

	// contacts with base structures (fixed to the turning ground)
	for (const BasePair &bp : basePairs) {
		VesselBase *A = bp.v;
		const Base *B = bp.base;
		ha.clear();
		A->CollectHulls (*A->s0, ha);
		double ra = HullRadius (ha, A->s0->pos);
		const Vector cgA (A->s0->pos - O);
		const CelestialBody *cb = A->proxybody;
		double wrot = Pi2 / cb->RotT();
		double margin = ((A->GVel() - B->GVel()).length() + A->AngularVelocity().length()*ra)*h + 0.05;
		convex::Manifold mf;
		for (const collision::Hull &y : B->CollisionHulls()) {
			Vector cy (B->GPos() + mul (B->GRot(), y.centre));
			if (cy.dist (A->s0->pos) > y.radius + ra + collision::SKIN + margin) continue;
			auto key = std::make_pair ((const VesselBase*)A, (const void*)&y);
			bool wasInside = inside.count (key) > 0;
			bool touching = false;
			for (const auto &x : ha) {
				convex::Placed px{x.hull, x.R, x.pos - O, collision::SKIN}, py{&y, B->GRot(), B->GPos() - O, 0.0};
				Vector cx (px.pos + mul (x.R, x.hull->centre));
				if (cx.dist (cy - O) > x.hull->radius + y.radius + collision::SKIN + margin) continue;
				if (!convex::Collide (px, py, margin, mf)) continue;
				for (int k = 0; k < mf.npt; k++) {
					if (mf.sep[k] < collision::SKIN) touching = true;
					// (deep in it when first met: placed inside, not driven in)
					if (!wasInside && mf.sep[k] < -0.2) { inside.insert (key); wasInside = true; }
				}
				if (wasInside) continue;
				for (int k = 0; k < mf.npt; k++) {
					const Vector &g = mf.pt[k];
					contact::Point p;
					p.C = mf.sep[k];
					p.k = p.c = 0.0;
					p.mu1 = p.mu2 = FRICTION;
					p.warm = nullptr;
					p.a = index[A], p.ra = g - cgA, p.n = mf.n; // (out of the structure)
					p.b = -1;
					Vector pl (tmul (cb->s0->R, g + O - cb->s0->pos));
					p.vs = cb->s1->vel + mul (cb->s1->R, Vector (-pl.z*wrot, 0, pl.x*wrot));
					contact::Tangents (p.n, Vector(1,0,0), p.t1, p.t2);
					owner.resize (pts.size(), nullptr);
					pts.push_back (p);
					owner.push_back (nullptr);
				}
			}
			if (wasInside && !touching) inside.erase (key); // (clear of it: collides from now on)
		}
	}

	std::vector<Vector> P, L;
	contact::Solve (pts, bodies, h, &P, &L);

	for (VesselBase *v : dynamic) {
		int i = index[v];
		const contact::Body &b = bodies[i];
		bool touched = P[i].length() || L[i].length() || b.vp.length() || b.wp.length();
		v->contact_force = tmul (v->s0->R, P[i]/h);
		v->contact_moment = tmul (v->s0->R, L[i]/h);
		if (!touched) continue;
		// (close to a planet's surface: positions relative to the turning ground)
		bool ground = v->proxybody && v->sp.alt < 2.0*v->Size() + 1e3;
		v->ApplyContactVelocity (bodies[i], h, ground);
		v->ContactStateChanged ();
	}

	// vessels at rest that were hit become dynamic
	for (size_t k = 0; k < owner.size(); k++)
		if (owner[k] && pts[k].ln > 0.0) owner[k]->WakeFromContact ();

	for (VesselBase *v : dynamic) v->contact_deferred = false;
}

// ==============================================================

void VesselCollider::Released (VesselBase *parent, VesselBase *payload)
{
	if (!parent || !payload || parent == payload) return;
	Release r;
	r.a = parent, r.b = payload;
	releases.push_back (r);
}

void VesselCollider::Forget (VesselBase *v)
{
	releases.erase (std::remove_if (releases.begin(), releases.end(), [v](const Release &r) { return r.a == v || r.b == v; }), releases.end());
	for (auto it = inside.begin(); it != inside.end(); ) it = it->first == v ? inside.erase (it) : std::next (it);
	pairs.clear(), basePairs.clear(), dynamic.clear();
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "OrbiterScene.h"
#include "World.h"
#include "Camera.h"
#include "Vessel.h"
#include "Planet.h"
#include "TimeData.h"
#include <cstring>

extern Camera *g_camera;
extern Vessel *g_focusobj;
extern TimeData td;

namespace audio {

namespace {
	V3 ToV3 (const Vector &v) { return V3 (v.x, v.y, v.z); }
	void Rot (const Matrix &m, double *r) { memcpy (r, m.data, 9 * sizeof (double)); }

	/// The vessel structure a body belongs to (docked or attached
	/// vessels are one)
	const void *Structure (const Body *b)
	{
		if (!b || b->Type () != OBJTP_VESSEL) return b;
		const Vessel *v = static_cast<const Vessel*>(b);
		if (const SuperVessel *s = v->SuperStruct ()) return s;
		const Vessel *root = v;
		while (const Vessel *p = root->AttachParent ()) root = p;
		return root;
	}
}

Snapshot OrbiterScene::Take (double t, double playT) const
{
	Snapshot s;
	s.t = t;
	s.playT = playT;
	s.dt = td.UiDT ();
	if (!g_camera) return s;
	s.listener = true;
	// the frame: the planet the camera is at (its target's, or the focus vessel's)
	Body *target = g_camera->Target ();
	const Planet *pl = nullptr;
	if (target && target->Type () == OBJTP_VESSEL) pl = static_cast<Vessel*>(target)->ProxyPlanet ();
	else if (target && target->Type () == OBJTP_PLANET) pl = static_cast<const Planet*>(target);
	if (!pl && g_focusobj) pl = g_focusobj->ProxyPlanet ();
	const Vector &cg = g_camera->GPos ();
	s.ear.pos = ToV3 (cg);
	Rot (g_camera->GRot (), s.ear.rot);
	// (out in space - no planet near: a black hole, deep space - the frame
	// is the camera's target's, moving with it: in global coordinates a
	// wreck falling into a hole, its listener with it, moved so far each
	// step that every path started over, unheard. No air there - a cinematic
	// world's goes with it)
	const Body *frame = pl ? static_cast<const Body*>(pl) : target;
	s.frame = frame;
	if (frame) {
		s.framePose.pos = ToV3 (frame->GPos ());
		Rot (frame->GRot (), s.framePose.rot);
		const double g[3] = { cg.x, cg.y, cg.z };
		if (pl) s.air = World::AirAt (pl, g);
	}
	s.inside = g_camera->IsInternal () && target && target->Type () == OBJTP_VESSEL;
	s.insideOf = s.inside ? Structure (target) : nullptr;
	return s;
}

bool OrbiterScene::PoseOf (const Body *b, Pose &p) const
{
	if (!b) return false;
	p.pos = ToV3 (b->GPos ());
	Rot (b->GRot (), p.rot);
	return true;
}

const void *OrbiterScene::StructureOf (const Body *b) const
{
	return Structure (b);
}

double OrbiterScene::SizeOf (const Body *b) const
{
	return b ? b->Size () : 0.0;
}

Air OrbiterScene::AirAt (const void *frame, const V3 &g) const
{
	const double gp[3] = { g.x, g.y, g.z };
	// (a frame not a planet's - a vessel, a wreck out in space: no air)
	const Body *b = static_cast<const Body*>(frame);
	if (!b || b->Type () != OBJTP_PLANET) return Air ();
	return World::AirAt (static_cast<const Planet*>(b), gp);
}

Air World::AirAt (const Planet *p, const double g[3])
{
	Air a;
	if (!p || !p->HasAtmosphere ()) return a;
	double lng, lat, rad;
	p->GlobalToEquatorial (Vector (g[0], g[1], g[2]), lng, lat, rad);
	ATMPARAM prm;
	if (!p->GetAtmParam (rad - p->Size (), lng, lat, &prm)) return a;
	const ATMCONST *ac = p->AtmParams ();
	return Air::From (prm.rho, prm.T, prm.p, ac->R, ac->gamma);
}

} // namespace audio

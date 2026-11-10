// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "WaterSpray.h"
#include "Scene.h"
#include "WaterFx.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
	// velocity of the ground (the still water) of planet hp under global position p
	VECTOR3 GroundVelocity (OBJHANDLE hp, const VECTOR3 &p)
	{
		double lng, lat, rad;
		oapiGlobalToEqu (hp, p, &lng, &lat, &rad);
		return oapiGetGroundVector (hp, lng, lat, 3);
	}

	bool Log () { static const bool on = getenv ("ORBITER_VK_SPRAY_LOG") != nullptr; return on; }
	// ORBITER_VK_WATERSPRAY=0: none (the rest of the scene's effects compared without it)
	bool Off () { static const bool off = [] { const char *e = getenv ("ORBITER_VK_WATERSPRAY"); return e && *e && !atoi (e); } (); return off; }

	// [m/s] how fast water is thrown by a blow (or a hull moving) at speed
	// v: half of it when slow, levelling off when fast (a blow at 10 m/s
	// throws spray at about 4 m/s, one at 120 m/s at 15: tens of metres up,
	// not a hundred)
	double Thrown (double v) { return 0.5 * v / (1.0 + v / 40.0); }

	// the ring of a burst is kept this long after its last emission at least [s]
	constexpr double BURST_RING_LIFE = 8.0;
}

/// what a frame's emissions are made from
struct WaterSpray::Frame {
	Scene *scene;
	ParticleSystem *sys;     // (null: none)
	WaterFx *wf;             // (null: none)
	OBJHANDLE hp;
	uint64_t key;
	VECTOR3 at, from;        // the footprint's centre on the water (global): now, last frame
	VECTOR3 up, fwd, side;   // up; the body's way over the water and to its right (horizontal)
	VECTOR3 gv, hv;          // the water's velocity; the body's horizontal velocity through it
	double vh, vn;           // [m/s] its speed over the water, sinking speed
	double L;                // [m] its scale: half its radius
	double a, b;             // [m] the footprint's half-length (along fwd) and half-width
	double g, dt;
	double dist;             // [m] of the footprint from the body (diagnostics)
	double D, sz, alpha;     // particle density setting; its size scale; opacity at full strength
};

bool WaterSpray::Reserve (ParticleSystem *sys, Ring &r, double rate, double life, uint32_t burst)
{
	if (sys->Reserve (r, rate, std::max (life, burst ? BURST_RING_LIFE : 0.0), ParticleSystem::Class::Spray, 2.0 * burst)) return true;
	if (Log ()) oapiWriteLogV ("VulkanClient: spray: no room (rate %.0f, burst %u)", rate, burst);
	return false;
}

static ParticleSystem::Emission Droplets (double dt, double alpha)
{
	ParticleSystem::Emission e{};
	e.dt = dt;
	e.alpha = alpha;
	e.slowdown = 0.8;
	e.tinted = true, e.cold = true, e.ballistic = true;
	e.volumetric = false;
	e.albedo[0] = 0.90f, e.albedo[1] = 0.93f, e.albedo[2] = 0.96f;
	e.cls = ParticleSystem::Class::Spray;
	return e;
}

void WaterSpray::Update (Scene *scene, OBJHANDLE hp, const WATERSTATE &ws, const VECTOR3 &gpos, const VECTOR3 &vel,
	double size, double camDist, uint64_t key)
{
	if (!ws.wet) entry = 0.0;
	double dt = oapiGetPause () ? 0.0 : oapiGetSimStep ();
	if (!ws.wet || ws.afloat || camDist > RANGE || !hp || dt <= 0.0 || Off ()) { havePrev = false; return; }

	Frame f;
	f.scene = scene, f.sys = scene->Particles (), f.wf = scene->Water (), f.hp = hp, f.key = key;
	if (f.sys && !f.sys->enabled) f.sys = nullptr;
	f.dt = dt;
	VECTOR3 pc;
	oapiGetGlobalPos (hp, &pc);
	double R = oapiGetSize (hp);
	f.g = R > 0.0 ? GGRAV * oapiGetMass (hp) / (R * R) : 9.81;
	{
		// the footprint's centre, on the water's surface over the wetted
		// surface's centre (under it: particles born below the sea's mean
		// surface are dropped at once)
		VECTOR3 c = ws.contact;
		f.up = unit (c - pc);
		double lng, lat, rad, datum = 0.0, seabed, h = 0.0;
		oapiGlobalToEqu (hp, c, &lng, &lat, &rad);
		oapiIsWater (hp, lng, lat, &datum, &seabed);
		oapiSeaSurface (hp, lng, lat, oapiGetSimMJD (), 0.0, &h);
		f.at = pc + f.up * (R + datum + std::max (h, 0.0) + 0.4);
	}
	f.dist = length (f.at - gpos);
	f.gv = GroundVelocity (hp, f.at);
	VECTOR3 rel = vel - f.gv;
	double vu = dotp (rel, f.up);
	f.hv = rel - f.up * vu;
	f.vh = length (f.hv), f.vn = std::max (-vu, 0.0);
	f.fwd = f.vh > 0.1 ? f.hv / f.vh : unit (crossp (f.up, fabs (f.up.y) > 0.9 ? _V(1,0,0) : _V(0,1,0)));
	f.side = unit (crossp (f.up, f.fwd));
	f.from = havePrev ? prev : f.at - f.hv * dt;
	prev = f.at, havePrev = true;

	// the body's scale and its footprint on the water: from the waterplane
	// (or the wetted) area, longer along its way than across
	f.L = std::clamp (0.5 * size, 0.15, 25.0);
	double A = std::max (std::max (ws.waterline, 0.3 * ws.wetted), 0.05);
	// (at least a tenth of its size across: a hull skimming the water throws
	// its spray from the length of the chine, not from the few square metres
	// wetted at that instant)
	f.b = std::clamp (0.5 * sqrt (0.5 * A), std::min (0.1 * size, 2.0), std::max (size, 0.1));
	f.a = std::clamp (2.0 * f.b, f.b, std::max (1.5 * size, f.b));
	f.D = f.sys ? f.sys->density : 1.0;
	f.sz = std::pow (f.D, -1.0 / 3.0);
	f.alpha = 0.85 * std::min (1.0, 1.3 / (f.D * f.sz * f.sz));

	// a blow: a new entry, or a harder blow of it
	if (ws.tWet != tWet) tWet = ws.tWet, entry = 0.0;
	if (ws.entrySpeed > 2.0 && ws.entrySpeed > entry + std::max (1.5, 0.15 * entry)) {
		Burst (f, ws.entrySpeed);
		entry = ws.entrySpeed;
	}
	// moving through it
	Sheet (f, ws.speed);
}

void WaterSpray::Burst (const Frame &f, double v)
{
	v = std::min (v, 80.0);
	double L = f.L;
	// rings and foam on the water, as wide as the blow
	if (f.wf) f.wf->Impulse (f.hp, f.at, std::max (f.b, L) * (1.0 + v / 40.0), std::clamp (v / 12.0, 0.15, 1.0));
	if (!f.sys) return;
	ParticleSystem *sys = f.sys;
	double D = f.D, sz = f.sz;
	const VECTOR3 &up = f.up, &fwd = f.fwd, &side = f.side;
	// (how much is thrown: by the blow's speed and the body's size; the
	// bounds grow with its size too, or a big body's splash would be as a
	// small one's)
	uint32_t nCrown = (uint32_t)(std::min (v * (8.0 + 6.0 * L), 800.0 + 250.0 * L) * D);
	uint32_t nCol = 0, nSide = 0;
	bool steep = f.vn > 4.0 && f.vn > 0.6 * f.vh;
	if (steep) nCol = (uint32_t)(std::min (f.vn * (3.0 + 2.5 * L), 400.0 + 150.0 * L) * D);
	if (f.vh > 6.0) nSide = (uint32_t)(std::min ((f.vh + 2.0 * v) * (2.0 + 1.5 * L), 500.0 + 150.0 * L) * D);
	if (Log ()) oapiWriteLogV ("VulkanClient: splash: blow %.1f m/s (over the water %.1f, sinking %.1f), L %.1f m, footprint %.1f x %.1f m: crown %u, column %u, sides 2 x %u; %.1f m from the body",
		v, f.vh, f.vn, L, 2.0 * f.a, 2.0 * f.b, nCrown, nCol, nSide, f.dist);

	bool crownOk = Reserve (sys, crown, 0.0, 5.0, nCrown + 2 * nSide);
	// the crown: thrown up and out all round the footprint (from its edge),
	// in layers of speed - the slow ones low and wide, the fast ones high
	// (one speed for all: a flat disc rising)
	constexpr int CROWN_LAYERS = 5, COLUMN_LAYERS = 6, SIDE_LAYERS = 3;
	if (crownOk) {
		ParticleSystem::Emission e = Droplets (f.dt, f.alpha);
		e.vel = f.gv + f.hv * 0.3;
		e.dir = -up; // (dir: opposite to the emission)
		e.planar = true; // (spread along the water)
		// (the droplets spread out to about 2.5 times their size over their
		// life, however long: growth by the second would make a long-lived
		// blow's spray clouds)
		e.size = (0.3 + 0.1 * L) * (1.0 + v / 30.0) * sz;
		e.alpha = 0.75 * f.alpha;
		VECTOR3 c[4] = { f.at + fwd * f.a + side * f.b, f.at + fwd * f.a - side * f.b, f.at - fwd * f.a - side * f.b, f.at - fwd * f.a + side * f.b };
		uint32_t n = std::max (nCrown / (4 * CROWN_LAYERS), 1u);
		for (int j = 0; j < CROWN_LAYERS; j++) {
			double k = (j + 1.0) / CROWN_LAYERS;
			double vt = Thrown (v);
			e.v0 = vt * k, e.spread = (0.3 + 0.7 * (1.0 - k)) * vt + 0.1 * Thrown (f.vh);
			e.lifetime = std::clamp (0.8 * e.v0 / f.g + 1.2, 1.5, 7.0);
			e.growth = 1.5 * e.size / e.lifetime;
			for (int i = 0; i < 4; i++) {
				e.p0 = c[i], e.p1 = c[(i + 1) & 3];
				sys->Emit (crown, e, n);
			}
		}
	}
	// a steep blow: a column thrown up from where it went in
	if (nCol && Reserve (sys, column, 0.0, 9.0, nCol)) {
		ParticleSystem::Emission e = Droplets (f.dt, f.alpha);
		e.vel = f.gv + f.hv * 0.2;
		e.dir = -up;
		e.size = (0.5 + 0.18 * L) * (1.0 + f.vn / 40.0) * sz;
		e.alpha = 0.8 * f.alpha;
		double w = 0.4 * f.b + 0.1 * L; // [m] its half-width
		e.slowdown = 0.5;
		// (in layers of speed: a column from the water up, not a ball thrown up)
		uint32_t n = std::max (nCol / (2 * COLUMN_LAYERS), 1u);
		for (int j = 0; j < COLUMN_LAYERS; j++) {
			double k = (j + 1.0) / COLUMN_LAYERS;
			double vt = 1.5 * Thrown (f.vn); // (a jet: faster than the crown)
			e.v0 = vt * k, e.spread = (0.08 + 0.08 * (1.0 - k)) * vt + 0.5;
			e.lifetime = std::clamp (2.2 * e.v0 / f.g + 1.0, 1.5, 9.0);
			e.growth = 1.2 * e.size / e.lifetime;
			e.p0 = f.at - side * w, e.p1 = f.at + side * w;
			sys->Emit (column, e, n);
			e.p0 = f.at - fwd * w, e.p1 = f.at + fwd * w;
			sys->Emit (column, e, n);
		}
	}
	// a fast shallow blow: sheets thrown out to both sides, carried on
	// along its way
	if (nSide && crownOk)
		for (double sg : { -1.0, 1.0 }) {
			ParticleSystem::Emission e = Droplets (f.dt, f.alpha);
			e.vel = f.gv + f.hv * 0.5;
			e.dir = -unit (side * (0.8 * sg) + up * 0.55 + fwd * 0.2);
			e.size = (0.2 + 0.06 * L) * (1.0 + f.vh / 60.0) * sz;
			e.alpha = 0.55 * f.alpha;
			e.slowdown = 1.2;
			e.p0 = f.at + side * (sg * f.b) - fwd * f.a, e.p1 = f.at + side * (sg * f.b) + fwd * f.a;
			for (int j = 0; j < SIDE_LAYERS; j++) {
				double k = (j + 1.0) / SIDE_LAYERS;
				double vt = Thrown (0.5 * f.vh + v);
				e.v0 = vt * k, e.spread = 0.6 * vt * (0.5 + 0.5 * k);
				e.lifetime = std::clamp (e.v0 / f.g + 1.2, 1.5, 5.0);
				e.growth = 1.5 * e.size / e.lifetime;
				sys->Emit (crown, e, std::max (nSide / SIDE_LAYERS, 1u));
			}
		}
}

void WaterSpray::Sheet (const Frame &f, double s)
{
	if (s < 3.0) return;
	double b = f.b;
	// the wake: foam and rings where it is wetted, wider and whiter the faster
	if (f.wf) f.wf->Feed (f.hp, (f.key << 16) | 0x200, f.at, b * (1.0 + s / 30.0), std::clamp (0.25 + s / 15.0, 0.0, 1.0), 0.0, WaterFx::Kind::Contact);
	if (!f.sys) return;
	ParticleSystem *sys = f.sys;
	double D = f.D, sz = f.sz;
	const VECTOR3 &up = f.up, &fwd = f.fwd, &side = f.side;
	double k = std::clamp ((s - 3.0) / 20.0, 0.0, 1.0);

	// sheets off both sides of the footprint: up and out, falling back
	{
		double rate = std::min ((8.0 + 5.0 * s) * (1.0 + 0.6 * b), 1500.0) * D; // [1/s] a side
		sheet.accum += 2.0 * rate * f.dt;
		uint32_t n = (uint32_t)(0.5 * sheet.accum);
		if (n && Reserve (sys, sheet, 2.0 * rate, 2.5, 0)) {
			sheet.accum -= 2.0 * n;
			for (double sg : { -1.0, 1.0 }) {
				ParticleSystem::Emission e = Droplets (f.dt, 0.6 * f.alpha * (0.4 + 0.6 * k));
				e.vel = f.gv + f.hv * 0.35;
				e.dir = -unit (side * (0.7 * sg) + up * 0.7 - fwd * 0.2);
				e.v0 = 0.9 * Thrown (s), e.spread = 0.3 * Thrown (s) + 0.5;
				e.lifetime = 2.5;
				e.size = (0.2 + 0.12 * b) * sz, e.growth = (0.2 + 0.1 * b) * (1.0 + s / 40.0) * sz;
				e.slowdown = 1.0;
				e.p0 = f.from + side * (sg * b), e.p1 = f.at + side * (sg * b);
				sys->Emit (sheet, e, n);
			}
		}
	}
	// fast: a rooster tail thrown up behind it
	double kt = std::clamp ((s - 12.0) / 25.0, 0.0, 1.0);
	if (kt > 0.0) {
		double rate = std::min ((5.0 + 4.0 * s) * (1.0 + 0.5 * b), 1200.0) * kt * D;
		tail.accum += rate * f.dt;
		uint32_t n = (uint32_t)tail.accum;
		if (n && Reserve (sys, tail, rate, 3.0, 0)) {
			tail.accum -= n;
			ParticleSystem::Emission e = Droplets (f.dt, 0.6 * f.alpha * (0.5 + 0.5 * kt));
			e.vel = f.gv + f.hv * 0.55;
			e.dir = -unit (up * 0.75 - fwd * 0.65);
			e.v0 = Thrown (s), e.spread = 0.25 * Thrown (s);
			e.lifetime = 3.0;
			e.size = (0.3 + 0.15 * b) * sz, e.growth = (0.3 + 0.12 * b) * sz;
			e.p0 = f.from - fwd * f.a, e.p1 = f.at - fwd * f.a;
			sys->Emit (tail, e, n);
		}
	} else tail.accum = 0.0;
	if (Log ()) oapiWriteLogV ("VulkanClient: spray sheet: %.1f m/s, footprint %.1f x %.1f m; %.1f m from the body (key %llx)", s, 2.0 * f.a, 2.0 * b, f.dist, (unsigned long long)f.key);
}

void WaterSpray::Draw (Scene *scene) const
{
	ParticleSystem *sys = scene->Particles ();
	if (!sys) return;
	double simt = oapiGetSimTime ();
	for (const Ring *r : { &crown, &column, &sheet, &tail })
		if (r->sys == sys && simt - r->lastEmit < 1.5 * r->life + 0.1)
			sys->Draw (*r, sys->SmokeTexture (), false, scene->FrameSet ()); // (if not released while idle)
}

void WaterSpray::Release (ParticleSystem *sys)
{
	for (Ring *r : { &crown, &column, &sheet, &tail })
		if (sys && r->sys == sys) sys->Release (*r);
}

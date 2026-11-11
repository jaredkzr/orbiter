// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "CrashFire.h"
#include "Scene.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace {
	bool FireLog ()
	{
		static const bool on = getenv ("ORBITER_VK_FIREBALL_LOG") != nullptr;
		return on;
	}
}

void CrashFires::Add (OBJHANDLE hp, const VECTOR3 &gpos, const VECTOR3 &dir, double speed, double M, double Mp, double R,
	double t0, double start, double tw, bool onWater)
{
	Fire f;
	f.hp = hp;
	oapiGlobalToEqu (hp, gpos, &f.lng, &f.lat, &f.rad);
	f.M = M, f.Mp = Mp, f.R = R, f.t0 = t0, f.start = start, f.tw = tw, f.onWater = onWater;
	f.seed = (uint32_t)(std::fabs (f.lng * 1e7) + std::fabs (f.lat * 3e7)) ^ 0x9e3779b9u;
	// the spill: about half the fireball's radius across, drawn out along
	// the path by the speed it crashed at (fuel thrown on with the wreck)
	f.width = 0.5 * R;
	f.half = 0.5 * R * (1.0 + std::min (speed / 40.0, 1.5));
	VECTOR3 c, up, east, north;
	Frame (f, c, up, east, north);
	VECTOR3 h = dir - up * dotp (dir, up);
	double lh = length (h);
	if (lh > 1e-6) {
		f.dirE = dotp (h, east) / lh, f.dirN = dotp (h, north) / lh;
		// (from where it ignited on along the path: the fuel thrown ahead)
		VECTOR3 cc = gpos + (h / lh) * (0.6 * (f.half - f.width));
		oapiGlobalToEqu (hp, cc, &f.lng, &f.lat, &f.rad);
	} else f.dirE = 1.0, f.dirN = 0.0, f.half = f.width;
	fires.push_back (f);
	if (FireLog ()) oapiWriteLogV ("VulkanClient: crash fire at t %.2f: M %.0f kg, R %.1f m, %.0f x %.0f m, flames from +%.2f s for t_w %.1f s (%s)",
		t0, M, R, 2.0 * f.half, 2.0 * f.width, start, tw, onWater ? "fuel slick on the water" : "on the ground");
}

void CrashFires::Strength (const Fire &f, double t, double &flames, double &smoke)
{
	// flames: rising as the fireball lifts off the spill, burning down a
	// little over t_w, then dying to embers over half of it; smoke: as
	// they burn, then a smoulder thinning over a minute
	double in = std::clamp ((t - f.start) / 0.4, 0.0, 1.0);
	double tw = f.tw;
	if (t < tw) flames = in * (1.0 - 0.3 * std::max (t, 0.0) / tw);
	else flames = in * 0.7 * std::pow (std::max (1.0 - (t - tw) / (0.5 * tw), 0.0), 1.5);
	smoke = in * (t < tw ? 1.0 : std::exp (-(t - tw) / 25.0));
}

void CrashFires::Frame (const Fire &f, VECTOR3 &c, VECTOR3 &up, VECTOR3 &east, VECTOR3 &north)
{
	VECTOR3 pp, pe, pn;
	oapiEquToGlobal (f.hp, f.lng, f.lat, f.rad, &c);
	oapiGetGlobalPos (f.hp, &pp);
	up = unit (c - pp);
	oapiEquToGlobal (f.hp, f.lng + 1e-6, f.lat, f.rad, &pe);
	oapiEquToGlobal (f.hp, f.lng, f.lat + 1e-6, f.rad, &pn);
	east = pe - c, east = unit (east - up * dotp (east, up));
	north = pn - c, north = unit (north - up * dotp (north, up));
}

void CrashFires::Update (Scene *scene)
{
	ParticleSystem *sys = scene->Particles ();
	double simt = oapiGetSimTime ();
	for (auto it = fires.begin(); it != fires.end(); ) {
		Fire &f = *it;
		double t = simt - f.t0;
		double fl, sm;
		Strength (f, t, fl, sm);
		// (gone: its smoke thinned out; or the time is before it - the replay)
		if (t < -1.0 || (t > 1.5 * f.tw && sm < 0.04)) {
			if (FireLog ()) oapiWriteLogV ("VulkanClient: crash fire of t %.2f gone at +%.1f s", f.t0, t);
			if (sys) for (Ring *r : { &f.flames, &f.stem }) if (r->sys == sys) sys->Release (*r);
			it = fires.erase (it);
			continue;
		}
		++it;
		if (FireLog ()) {
			if (!f.logged && t >= f.start) f.logged = true, oapiWriteLogV ("VulkanClient: stage crash fire at +%.2f s: flames", t);
			if (!f.loggedOut && t >= 1.5 * f.tw) f.loggedOut = true, oapiWriteLogV ("VulkanClient: stage crash fire at +%.2f s: flames out (smoulder)", t);
		}
		f.strength = fl, f.smoke = sm;
		double dt = oapiGetPause () ? 0.0 : oapiGetSimStep ();
		if (!sys || !sys->enabled || dt <= 0.0 || t < f.start) continue;
		VECTOR3 c, up, east, north;
		Frame (f, c, up, east, north);
		if (length (c - scene->CameraPos ()) > 50e3) continue; // (too far to see; it burns on)
		VECTOR3 ax = east * f.dirE + north * f.dirN, ay = crossp (up, ax);
		VECTOR3 gv = oapiGetGroundVector (f.hp, f.lng, f.lat, 3);
		double D = sys->density, sz = std::pow (D, -1.0 / 3.0);
		std::mt19937 rng (f.seed ^ (uint32_t)(simt * 1000.0));
		std::uniform_real_distribution<double> U (0.0, 1.0);
		auto spot = [&](double k) { // (a point of the spill, within k of it)
			double r = k * std::sqrt (U (rng)), a = 6.2831853 * U (rng);
			return c + ax * (r * f.half * std::cos (a)) + ay * (r * f.width * std::sin (a));
		};
		// its flames: tongues of a few metres over the spill (lower as they die down)
		if (fl > 0.01) {
			if (!f.flames.tex) f.flames.tex = scene->FireballTexture (4), f.flames.emissive = true;
			double s0 = std::clamp (0.16 * f.R, 1.5, 8.0);
			double rate = std::clamp (3.0 * f.R, 10.0, 300.0) * D;
			f.flames.accum += rate * (0.3 + 0.7 * fl) * dt;
			uint32_t n = (uint32_t)f.flames.accum;
			f.flames.accum -= n;
			const int K = 6; // (emissions at points over the spill)
			if (n && sys->Reserve (f.flames, rate, 1.0, ParticleSystem::Class::Fire, 0.0, f.flames.tex, true)) {
				for (int k = 0; k < K && n; k++) {
					uint32_t m = (n + (K - 1 - k)) / (K - k);
					n -= m;
					if (!m) continue;
					ParticleSystem::Emission e{};
					e.p1 = spot (1.0) + up * (0.3 * s0), e.p0 = e.p1 - gv * dt;
					e.vel = gv, e.dir = -up, e.dt = dt;
					e.v0 = 1.0 + 2.5 * fl, e.spread = 0.6 + 0.4 * fl;
					e.lifetime = 0.6 + 0.4 * fl;
					e.size = s0 * sz * (0.5 + 0.5 * fl), e.growth = 0.6 * s0 * sz;
					e.alpha = 0.1 + 0.35 * fl, e.slowdown = 1.0;
					e.emissive = true, e.ramp = true, e.erode = true;
					e.tinted = true, e.albedo[0] = 1.0f, e.albedo[1] = 0.72f, e.albedo[2] = 0.38f;
					e.cls = ParticleSystem::Class::Fire;
					sys->Emit (f.flames, e, m);
				}
			}
		}
		// its smoke: a black stem rising from it (burning fuel smokes black,
		// on the water too), thinning as it smoulders
		if (sm > 0.03) {
			if (!f.stem.tex) f.stem.tex = sys->SmokeTexture ();
			// (from about its middle, overlapping: one stem - puffs from all over
			// it rose as a scatter of balls)
			double Rc = std::clamp (0.25 * f.R, 2.5, 25.0);
			double rate = 10.0 * D;
			f.stem.accum += rate * dt;
			uint32_t n = (uint32_t)f.stem.accum;
			f.stem.accum -= n;
			if (n && sys->Reserve (f.stem, rate, 30.0, ParticleSystem::Class::WreckSmoke, 0.0, f.stem.tex, false)) {
				ParticleSystem::Emission e{};
				e.p1 = spot (0.15) + up * (0.5 * Rc), e.p0 = e.p1 - gv * dt;
				e.vel = gv, e.dir = -up, e.dt = dt;
				e.v0 = 2.5 + 2.5 * fl, e.spread = 1.0;
				e.lifetime = 30.0;
				e.size = Rc * sz, e.growth = (0.12 * Rc + 0.6) * sz;
				e.alpha = 0.6 * sm, e.slowdown = 0.4;
				e.tinted = true, e.volumetric = true;
				e.albedo[0] = e.albedo[1] = e.albedo[2] = 0.08f;
				e.cls = ParticleSystem::Class::WreckSmoke;
				sys->Emit (f.stem, e, n);
			}
		}
	}
}

void CrashFires::CollectLights (Scene *scene, std::vector<LightSource> &lights)
{
	double simt = oapiGetSimTime ();
	for (const Fire &f : fires) {
		if (f.strength <= 0.02) continue;
		VECTOR3 c, up, east, north;
		Frame (f, c, up, east, north);
		// (the burning spill: about the sun's light near it, flickering)
		double flick = 0.85 + 0.15 * std::sin (simt * 21.0 + f.seed) * std::sin (simt * 6.7);
		float I = (float)(std::min (1.0, f.R / 10.0) * 1.6 * f.strength * flick);
		LightSource l{};
		l.pos = c + up * (0.15 * f.R), l.dir = _V(0, 0, 1);
		l.col[0] = I, l.col[1] = 0.5f * I, l.col[2] = 0.17f * I;
		l.range = std::max (4.0 * f.R, 30.0);
		l.att[0] = 1.0, l.att[1] = 0.0, l.att[2] = 1.0 / std::max (l.range * l.range * 0.01, 1.0);
		// (a small source radius: soft shadows near it; the spill's size made
		// its RT shadow rays incoherent over the sea in its range for its whole
		// t_w, rtl trace +2-3 ms)
		l.radius = std::min (0.1 * f.width, 2.0);
		l.spot = false;
		l.owner = 0;
		l.shadowOfs = 0.0;
		lights.push_back (l);
	}
}

void CrashFires::DrawParticles (Scene *scene)
{
	ParticleSystem *sys = scene->Particles ();
	if (!sys) return;
	double simt = oapiGetSimTime ();
	for (const Fire &f : fires)
		for (const Ring *r : { &f.stem, &f.flames })
			if (r->sys == sys && simt - r->lastEmit < 1.5 * r->life + 0.1)
				sys->Draw (*r, r->tex, r->emissive, scene->FrameSet ());
}

void CrashFires::Release (ParticleSystem *sys)
{
	for (Fire &f : fires)
		for (Ring *r : { &f.flames, &f.stem })
			if (sys && r->sys == sys) sys->Release (*r);
	fires.clear ();
}

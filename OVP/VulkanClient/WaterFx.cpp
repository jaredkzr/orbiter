// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "WaterFx.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

	// the model of a disturbance (DESIGN 2.2 of hover item G4); tunable within 0.5x-2x
	constexpr double JET_RING_GAIN = 0.40;      // ring slope at the footprint edge per unit jet strength
	constexpr double CONTACT_RING_GAIN = 0.10;  // (a gear leg in the water)
	constexpr double RING_SLOPE_MAX = 0.30;
	constexpr double CONTACT_FOAM = 0.6;        // foam target of a gear leg per unit strength
	constexpr double WALL_STRESS = 0.63 * 43.0; // wall jet: stress Fs / (0.63 r^2); water roughens above 43 Pa (a 6 m/s breeze)
	constexpr double RW_MAX = 80.0;             // [m]
	constexpr double CAVITY_MAX = 0.3;          // of R0
	constexpr double J_RISE = 0.3, J_DECAY = 0.4;      // [s]
	constexpr double FOAM_RISE = 2.0, FOAM_DECAY = 8.0; // [s]
	constexpr double RING_L0 = 1.6;             // longest ring wave: this many footprint radii
	constexpr double RING_WAVE_RATIO = 0.6;     // wavelength ratio of successive ring waves
	constexpr double RING_DECAY_R = 4.0;        // ring damping length [ring source radii]
	constexpr double RING_SRC_FRAC = 0.25;      // jet ring source radius: this fraction of the wall-jet radius Rw ...
	constexpr double RING_SRC_MAX = 6.0;        // ... but at most this [m]
	constexpr double SPLASH_PULSE = 0.6;        // [s]
	constexpr double FOAM_CHURN = 0.25, CHOP_CHURN = 1.5; // [m/s] the noise's drift
	constexpr double FOAM_WRAP = 2048.0, CHOP_WRAP = 1024.0; // [m] (the noise periods divide them)
	constexpr double FAR_MARGIN = 6000.0;       // sources this far beyond their reach are not packed [m]
	constexpr double RELEASED_MAX = 120.0;      // [s] a released source is retired after this
	const double TWO_PI = 2.0 * PI;

	const char *KindName (WaterFx::Kind k)
	{
		return k == WaterFx::Kind::Jet ? "jet" : k == WaterFx::Kind::Contact ? "contact" : "splash";
	}

	double SurfaceG (OBJHANDLE body)
	{
		double R = oapiGetSize (body);
		return R > 0.0 ? GGRAV * oapiGetMass (body) / (R * R) : 9.81;
	}

	VECTOR3 ToPlanet (OBJHANDLE body, const VECTOR3 &gpos)
	{
		VECTOR3 bp;
		MATRIX3 R;
		oapiGetGlobalPos (body, &bp);
		oapiGetRotationMatrix (body, &R);
		return tmul (R, gpos - bp);
	}

	bool Released (double tRel, double now) { return tRel >= 0.0 && now >= tRel; }

}

WaterFx::WaterFx ()
{
	if (const char *e = getenv ("ORBITER_VK_WATERFX")) if (*e && !atoi (e)) enabled = false; // (empty: unset)
	if (const char *e = getenv ("ORBITER_VK_WATERFX_LOG")) if (*e && atoi (e)) log = true;
}

static double RingSrc (WaterFx::Kind kind, double R0, double Rw)
{
	// (not std::clamp: a footprint wider than RING_SRC_MAX would make its bounds cross)
	return kind == WaterFx::Kind::Jet ? std::max (R0, std::min (RING_SRC_FRAC * Rw, RING_SRC_MAX)) : R0;
}

void WaterFx::MakeRoom (double now)
{
	if (list.size() < (size_t)MAX_SOURCES) return;
	auto weakest = list.end();
	for (auto it = list.begin(); it != list.end(); ++it)
		if (Released (it->tRel, now) && (weakest == list.end() || it->foam + it->ringA < weakest->foam + weakest->ringA)) weakest = it;
	if (weakest == list.end()) weakest = std::min_element (list.begin(), list.end(), [](const Source &a, const Source &b) { return a.t0 < b.t0; });
	list.erase (weakest);
}

void WaterFx::Feed (OBJHANDLE body, uint64_t key, const VECTOR3 &gpos, double R0, double J, double Fs, Kind kind)
{
	if (!enabled || !body) return;
	J = std::clamp (J, 0.0, 1.0);
	if (J < 0.02) return;
	R0 = std::max (R0, 0.05);
	double now = oapiGetSimTime ();
	VECTOR3 lp = ToPlanet (body, gpos);
	double g = SurfaceG (body);
	double Rw = Fs > 0.0 ? std::min (std::max (sqrt (Fs / WALL_STRESS), 2.0 * R0), RW_MAX) : 2.0 * R0;
	double depth = Fs > 0.0 ? std::min (Fs / (PI * R0 * R0 * 1000.0 * g), CAVITY_MAX * R0) : 0.0;
	double target = kind == Kind::Contact ? CONTACT_FOAM * J : J;
	for (Source &s : list) {
		if (s.body != body || s.key != key || s.tRel >= 0.0) continue;
		if (length (lp - s.c) <= std::max (2.0, std::max (R0, 0.25 * Rw))) {
			s.c = lp, s.R0 = R0, s.Rw = Rw, s.depth = depth, s.g = g;
			s.Rr = std::max (s.Rr, RingSrc (s.kind, R0, Rw));
			s.Jfed = J, s.foamTarget = target, s.fed = true;
			return;
		}
		s.tRel = now, s.key = 0; // (moved off: its rings detach, a new source is born)
		break;
	}
	MakeRoom (now);
	Source s;
	s.body = body, s.key = key, s.kind = kind;
	s.c = s.a = lp;
	VECTOR3 up = unit (lp);
	s.t1 = unit (crossp (up, fabs (up.y) > 0.9 ? _V(1,0,0) : _V(0,1,0)));
	s.R0 = R0, s.Rw = Rw, s.g = g;
	s.Rr = RingSrc (kind, R0, Rw);
	s.Jfed = J, s.J = 0.0, s.depth = depth, s.foam = 0.0, s.foamTarget = target;
	s.ringGain = kind == Kind::Contact ? CONTACT_RING_GAIN : JET_RING_GAIN;
	s.t0 = now, s.tRel = -1.0, s.fed = true;
	list.push_back (s);
	if (first) {
		first = false;
		char nm[64];
		oapiGetObjectName (body, nm, 64);
		oapiWriteLogV ("VulkanClient: waterfx: first source on %s (%s)", nm, KindName (kind));
	}
}

void WaterFx::Impulse (OBJHANDLE body, const VECTOR3 &gpos, double R0, double strength)
{
	if (!enabled || !body) return;
	double now = oapiGetSimTime ();
	strength = std::clamp (strength, 0.0, 1.0);
	if (strength < 0.02) return;
	MakeRoom (now);
	Source s;
	s.body = body, s.key = 0, s.kind = Kind::Splash;
	s.c = s.a = ToPlanet (body, gpos);
	VECTOR3 up = unit (s.c);
	s.t1 = unit (crossp (up, fabs (up.y) > 0.9 ? _V(1,0,0) : _V(0,1,0)));
	s.R0 = s.Rr = std::max (R0, 0.05);
	s.Rw = 2.5 * s.R0, s.g = SurfaceG (body);
	s.Jfed = s.J = s.foam = s.foamTarget = strength;
	s.ringA = std::min (0.45 * strength, RING_SLOPE_MAX);
	s.depth = 0.0;
	s.t0 = now, s.tRel = now + SPLASH_PULSE, s.fed = false;
	list.push_back (s);
	if (first) {
		first = false;
		char nm[64];
		oapiGetObjectName (body, nm, 64);
		oapiWriteLogV ("VulkanClient: waterfx: first source on %s (splash)", nm);
	}
}

void WaterFx::Update ()
{
	double now = oapiGetSimTime ();
	if (now < lastT - 1e-3) list.clear (); // (time went back: playback, rewind)
	double dt = lastT < 0.0 ? 0.0 : now - lastT;
	if (dt > 0.0) {
		double kJr = 1.0 - exp (-dt / J_RISE), kJd = exp (-dt / J_DECAY);
		double kFr = 1.0 - exp (-dt / FOAM_RISE), kFd = exp (-dt / FOAM_DECAY);
		for (Source &s : list) {
			bool fed = s.tRel < 0.0 ? s.fed : now < s.tRel;
			if (s.tRel < 0.0 && !s.fed) s.tRel = now;
			if (fed) {
				s.J += (s.Jfed - s.J) * kJr;
				s.foam += (s.foamTarget - s.foam) * kFr;
				if (s.kind != Kind::Splash) s.ringA = std::min (s.ringGain * s.J, RING_SLOPE_MAX);
			} else {
				s.J *= kJd;
				s.foam *= kFd;
			}
			s.fed = false;
			double k0 = TWO_PI / (RING_L0 * s.Rr), cg0 = 0.5 * sqrt (s.g / k0), Lr = RING_DECAY_R * s.Rr;
			double kj = k0;
			for (int j = 0; j < 3; j++, kj /= RING_WAVE_RATIO)
				s.psi[j] = fmod (s.psi[j] + sqrt (s.g * kj) * dt, TWO_PI);
			s.run = std::min (s.run + cg0 * dt, 4.0 * Lr);
			if (Released (s.tRel, now)) s.inner += cg0 * dt;
		}
		// retire the spent: no foam, no jet, its ring train run out; or too long released
		list.erase (std::remove_if (list.begin(), list.end(), [now](const Source &s) {
			if (!Released (s.tRel, now)) return false;
			if (now - s.tRel > RELEASED_MAX) return true;
			return s.foam < 0.01 && s.J < 0.01 && s.inner > 4.0 * RING_DECAY_R * s.Rr;
		}), list.end());
	}
	lastT = now;

	if (log && now - logT >= 2.0 && !list.empty()) {
		logT = now;
		VECTOR3 cg;
		oapiCameraGlobalPos (&cg);
		OBJHANDLE cb = oapiCameraProxyGbody ();
		int onCam = 0;
		for (const Source &s : list) if (s.body == cb) onCam++;
		char nm[64] = "";
		if (cb) oapiGetObjectName (cb, nm, 64);
		oapiWriteLogV ("VulkanClient: waterfx: %d sources (%d on %s)", (int)list.size(), onCam, nm);
		int i = 0;
		for (const Source &s : list) {
			VECTOR3 cp = ToPlanet (s.body, cg);
			double dist = length (cp - s.c);
			if (dist < 10e3) {
				double age = std::max (0.0, now - s.t0);
				bool rel = Released (s.tRel, now);
				double front = s.R0 + s.run;
				double inner = s.inner;
				char bn[64];
				oapiGetObjectName (s.body, bn, 64);
				if (rel) oapiWriteLogV ("VulkanClient: waterfx %s #%d %s released %.1f s R0 %.2f Rr %.2f Rw %.1f J %.2f depth %.2f foam %.2f ring %.3f front %.1f inner %.1f dist %.0f m",
					bn, i, KindName (s.kind), now - s.tRel, s.R0, s.Rr, s.Rw, s.J, s.depth, s.foam, s.ringA, front, inner, dist);
				else oapiWriteLogV ("VulkanClient: waterfx %s #%d %s fed %.1f s R0 %.2f Rr %.2f Rw %.1f J %.2f depth %.2f foam %.2f ring %.3f front %.1f inner %.1f dist %.0f m",
					bn, i, KindName (s.kind), age, s.R0, s.Rr, s.Rw, s.J, s.depth, s.foam, s.ringA, front, inner, dist);
			}
			i++;
		}
	}
}

int WaterFx::Pack (OBJHANDLE body, const VECTOR3 &cam, float hdr[4], float src[][4]) const
{
	hdr[0] = hdr[1] = hdr[2] = hdr[3] = 0.0f;
	if (!enabled || !body) return 0;
	double now = oapiGetSimTime ();
	struct Cand { double d; const Source *s; };
	Cand cand[MAX_SOURCES];
	int nc = 0;
	for (const Source &s : list) {
		if (s.body != body || nc >= MAX_SOURCES) continue;
		double front0 = s.R0 + s.run;
		double infl = std::max (s.Rw, front0 + RING_L0 * s.Rr);
		double d = length (s.c - cam) - infl;
		if (d > FAR_MARGIN) continue;
		cand[nc++] = { d, &s };
	}
	std::sort (cand, cand + nc, [](const Cand &a, const Cand &b) { return a.d < b.d; });
	int n = std::min (nc, MAX_GPU);
	for (int i = 0; i < n; i++) {
		const Source &s = *cand[i].s;
		float (*v)[4] = src + i * VEC4;
		double k0 = TWO_PI / (RING_L0 * s.Rr);
		double Lr = RING_DECAY_R * s.Rr;
		double age = std::max (0.0, now - s.t0);
		double front0 = s.R0 + s.run;
		double inner = s.inner;
		double depthNow = s.depth * std::min (1.0, s.J / std::max (s.Jfed, 0.02));
		VECTOR3 pc = s.c - cam, pa = s.a - cam;
		v[0][0] = (float)pc.x, v[0][1] = (float)pc.y, v[0][2] = (float)pc.z, v[0][3] = (float)s.R0;
		v[1][0] = (float)pa.x, v[1][1] = (float)pa.y, v[1][2] = (float)pa.z, v[1][3] = (float)s.Rw;
		v[2][0] = (float)s.t1.x, v[2][1] = (float)s.t1.y, v[2][2] = (float)s.t1.z, v[2][3] = (float)fmod (FOAM_CHURN * age, FOAM_WRAP);
		v[3][0] = (float)s.J, v[3][1] = (float)depthNow, v[3][2] = (float)s.foam, v[3][3] = (float)s.ringA;
		v[4][0] = (float)k0, v[4][1] = (float)front0, v[4][2] = (float)inner, v[4][3] = (float)fmod (CHOP_CHURN * age, CHOP_WRAP);
		for (int j = 0; j < 3; j++)
			v[5][j] = (float)s.psi[j];
		v[5][3] = (float)Lr;
	}
	hdr[0] = (float)n;
	return n;
}

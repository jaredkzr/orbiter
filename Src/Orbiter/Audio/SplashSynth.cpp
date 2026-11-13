// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "SplashSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	constexpr float TWO_PI = 6.2831853f;
	/// Output gains: the slam, the bubbles, the spray's rush (set so a
	/// full-strength entry's first second is about SPLASH_RMS)
	constexpr float SLAM_GAIN = 2.0f, BUBBLE_GAIN = 0.55f, RUSH_GAIN = 2.5f, WAKE_GAIN = 0.4f;
	constexpr float OUT_GAIN = 1.0f; // (the whole: a full entry's first second about SPLASH_RMS)
}

std::unique_ptr<SplashSynth> SplashSynth::Make (Engine &e, uint32_t seed)
{
	std::unique_ptr<SplashSynth> s (new SplashSynth (seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

void SplashSynth::Entry (float strength, float size, float speed)
{
	pending.strength.store (std::clamp (strength, 0.0f, 4.0f), std::memory_order_relaxed);
	pending.size.store (std::max (size, 0.1f), std::memory_order_relaxed);
	pending.speed.store (std::max (speed, 0.0f), std::memory_order_relaxed);
	pending.count.fetch_add (1, std::memory_order_release);
}

void SplashSynth::Set (float waterSpeed, float size, float wakeGain)
{
	pWake.store (std::clamp (wakeGain, 0.0f, 1e3f), std::memory_order_relaxed);
	pSpeed.store (std::max (waterSpeed, 0.0f), std::memory_order_relaxed);
	pSize.store (std::max (size, 0.1f), std::memory_order_relaxed);
}

float SplashSynth::RadiusLogUniform (float rmin, float rmax, float alpha)
{
	// a power law p(r) ~ r^-alpha between rmin and rmax (many small, few large)
	const float u = rng.Uniform ();
	const float a1 = 1.0f - alpha;
	const float lo = std::pow (rmin, a1), hi = std::pow (rmax, a1);
	return std::pow (lo + u * (hi - lo), 1.0f / a1);
}

void SplashSynth::Spawn (float r, float amp)
{
	Bubble *b = nullptr;
	for (auto &x : bubbles) if (!x.on) { b = &x; break; }
	if (!b) { // (all ringing: the quietest makes way)
		b = &bubbles[0];
		for (auto &x : bubbles) if (x.env < b->env) b = &x;
	}
	const float f = std::min ((float)Minnaert (r), 0.45f * rate);
	b->f = f;
	b->d = 0.043f * f + 0.0014f * std::pow (f, 1.5f);   // [1/s]
	b->rise = 0.1f * b->d;                                // (its pitch rising as it rises: van den Doel's xi)
	b->a = amp, b->env = amp, b->t = 0.0f, b->on = true;
	// (started as a sine of amplitude amp: y[-1] = 0, y[-2] = -amp sin w / r)
	const float w = TWO_PI * f / rate, rr = std::exp (-b->d / rate);
	b->c = 2.0f * rr * std::cos (w), b->r2 = rr * rr;
	b->y1 = 0.0f, b->y2 = -amp * std::sin (w) / rr;
}

void SplashSynth::Render (float *out, unsigned n, float dop)
{
	int c = pending.count.load (std::memory_order_acquire);
	if (c != played) {
		played = c;
		es = pending.strength.load (std::memory_order_relaxed);
		eSize = pending.size.load (std::memory_order_relaxed);
		const float v = pending.speed.load (std::memory_order_relaxed);
		et = 0.0f;
		// the spray up for about 2 v_spray / g, its drops' speed some of the
		// entry's; the cavity's largest bubbles about a tenth of the body
		eSpray = std::clamp (0.6f * v / 9.81f, 0.3f, 4.0f);
		eRmax = std::clamp (0.02f * eSize, 0.01f, 0.08f); // (up to 8 cm: some 40 Hz)
		bubbleDebt = 0.0f;
		slamHp.HighPass (80.0f, 0.7f, rate);
		slamLp.LowPass (std::clamp (6000.0f / std::sqrt (eSize), 500.0f, 6000.0f), 0.7f, rate);
		// the cavity's large bubbles: a few, at once - low gloops
		const int nBig = std::clamp ((int)(3.0f + 2.0f * eSize), 3, 24);
		for (int k = 0; k < nBig; k++) {
			float r = RadiusLogUniform (0.2f * eRmax, eRmax, 1.5f);
			Spawn (r, 0.5f * std::sqrt (r / eRmax));
		}
	}
	const float speed = speedS.Step (pSpeed.load (std::memory_order_relaxed), dsp::Smooth::K (0.3f, n, rate));
	const float size = pSize.load (std::memory_order_relaxed);
	rushBp1.HighPass (400.0f, 0.6f, rate);
	rushBp2.LowPass (5000.0f, 0.6f, rate);
	const float dt = 1.0f / rate;

	// bubbles born this block: the spray's drops falling back (a rate
	// falling over the spray's time), the wake's (with the speed)
	float sprayRate = 0.0f;
	if (et < eSpray) sprayRate = es * 900.0f * std::exp (-2.5f * et / eSpray) / eSpray; // [1/s]
	bubbleDebt += sprayRate * n * dt;
	while (bubbleDebt >= 1.0f) {
		bubbleDebt -= 1.0f;
		// (drops of millimetres entrain bubbles of a fraction of a millimetre
		// to several: 0.4-8 mm, 0.4-8 kHz, many more small than large)
		float r = RadiusLogUniform (0.0004f, 0.008f, 2.6f);
		Spawn (r, 1.6f * std::sqrt (es) * (0.3f + 0.7f * rng.Uniform ()));
	}
	const float wakeRate = speed > 0.05f ? 25.0f * speed * std::sqrt (size) : 0.0f;
	wakeDebt += wakeRate * n * dt;
	while (wakeDebt >= 1.0f) {
		wakeDebt -= 1.0f;
		float r = RadiusLogUniform (0.001f, 0.015f, 2.0f);
		Spawn (r, pWake.load (std::memory_order_relaxed) * WAKE_GAIN / BUBBLE_GAIN * std::min (1.0f, 0.3f + 0.15f * speed) * (0.4f + 0.6f * rng.Uniform ()));
	}

	// the bubbles' pitch (rising) and decay, per block
	const float tb = n * dt;
	for (auto &x : bubbles) {
		if (!x.on) continue;
		x.t += tb;
		x.env = x.a * std::exp (-x.d * x.t);
		if (x.env < 1e-3f * x.a) { x.on = false; continue; }
		const float w = TWO_PI * x.f * dop * (1.0f + x.rise * x.t) / rate;
		x.c = 2.0f * std::sqrt (x.r2) * std::cos (std::min (w, 3.0f));
	}
	for (unsigned i = 0; i < n; i++) {
		float s = 0.0f;
		const float p = pink.Next (rng.White ());
		if (et < 8.0f) {
			// the slam: the struck water's slap, over in some tens of ms
			s += SLAM_GAIN * std::sqrt (es) * std::exp (-et / 0.015f) * slamLp.Process (slamHp.Process (rng.White ()));
			// the spray sheet's rush, dying as its drops come down
			if (et < 2.0f * eSpray) {
				const float env = (1.0f - std::exp (-et / 0.02f)) * std::exp (-2.0f * et / eSpray);
				s += RUSH_GAIN * std::sqrt (es) * env * rushBp2.Process (rushBp1.Process (p));
			}
			et += dt;
		}
		float b = 0.0f;
		for (auto &x : bubbles) {
			if (!x.on) continue;
			const float y = x.c * x.y1 - x.r2 * x.y2;
			x.y2 = x.y1, x.y1 = y;
			b += y;
		}
		out[i] = OUT_GAIN * (s + BUBBLE_GAIN * b);
	}
}

} // namespace audio

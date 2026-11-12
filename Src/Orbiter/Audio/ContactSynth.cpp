// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ContactSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	constexpr float TWO_PI = 6.2831853f;
}

std::unique_ptr<ContactSynth> ContactSynth::Make (Engine &e, uint32_t seed)
{
	std::unique_ptr<ContactSynth> s (new ContactSynth (seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

void ContactSynth::Set (const Params &p)
{
	pRollSpeed.store (std::max (0.0f, p.rollSpeed), std::memory_order_relaxed);
	pRollLoad.store (std::clamp (p.rollLoad, 0.0f, 2.0f), std::memory_order_relaxed);
	pSqueal.store (std::clamp (p.squeal, 0.0f, 1.0f), std::memory_order_relaxed);
	pScrape.store (std::clamp (p.scrape, 0.0f, 1.0f), std::memory_order_relaxed);
	pScrapeSpeed.store (std::max (0.0f, p.scrapeSpeed), std::memory_order_relaxed);
	for (int k = 0; k < NMOTOR; k++) pMotor[k].store (std::clamp (p.motor[k], 0.0f, 1.0f), std::memory_order_relaxed);
}

void ContactSynth::Strike (Blow kind, float strength)
{
	if (kind < 0 || kind >= NBLOW) return;
	pending[kind].strength.store (std::clamp (strength, 0.0f, 1.5f), std::memory_order_relaxed);
	pending[kind].count.fetch_add (1, std::memory_order_release);
}

void ContactSynth::Start (int kind, float s, float delay)
{
	// (into a free voice, or the quietest)
	Voice *v = nullptr;
	for (auto &x : voices) if (x.kind < 0) { v = &x; break; }
	if (!v) {
		v = &voices[0];
		for (auto &x : voices) if (x.s < v->s) v = &x;
	}
	*v = Voice ();
	v->kind = kind, v->s = s, v->delay = delay;
}

float ContactSynth::Voice1 (Voice &v, float chirp, float crunch)
{
	// one sample of a blow; ends the voice when it has died away
	const float t = v.t, dt = 1.0f / rate;
	float y = 0.0f;
	switch (v.kind) {
	case TOUCHDOWN: {
		// the strut's thump (70 Hz falling to 45 Hz) and the tyre's chirp
		float f = 45.0f + 25.0f * std::exp (-t / 0.03f);
		v.ph += TWO_PI * f * dt;
		y = 1.6f * std::exp (-t / 0.12f) * std::sin (v.ph);
		y += 0.5f * std::exp (-t / 0.07f) * chirp;
		if (t > 0.6f) v.kind = -1;
		break; }
	case HULL: {
		// a heavy thud (55 Hz falling to 30 Hz) and a crunch of low noise
		float f = 30.0f + 25.0f * std::exp (-t / 0.05f);
		v.ph += TWO_PI * f * dt;
		y = 2.2f * std::exp (-t / 0.25f) * std::sin (v.ph);
		y += 2.0f * std::exp (-t / 0.3f) * crunch;
		if (t > 1.5f) v.kind = -1;
		break; }
	case CLANG: case DOCK: case UNDOCK: {
		// metal: inharmonic partials (a bell's ratios), low; a thump under them
		const float f0 = v.kind == UNDOCK ? 140.0f : 170.0f;
		v.ph += TWO_PI * f0 * dt, v.ph2 += TWO_PI * f0 * 2.43f * dt, v.ph3 += TWO_PI * f0 * 4.47f * dt;
		y = 0.7f * std::exp (-t / 0.7f) * std::sin (v.ph) + 0.4f * std::exp (-t / 0.4f) * std::sin (v.ph2) + 0.2f * std::exp (-t / 0.2f) * std::sin (v.ph3);
		y += 1.4f * std::exp (-t / 0.06f) * std::sin (TWO_PI * 60.0f * t);
		if (t > 3.0f) v.kind = -1;
		break; }
	case GEARLOCK: {
		// the gear locking: a heavy thunk (60 Hz falling to 40), then its latch
		float f = 40.0f + 20.0f * std::exp (-t / 0.02f);
		v.ph += TWO_PI * f * dt;
		y = 1.6f * std::exp (-t / 0.1f) * std::sin (v.ph) + 0.5f * crunch * std::exp (-t / 0.05f);
		if (t > 0.6f) v.kind = -1;
		break; }
	case LATCH: {
		// (a latch: a short, low clunk)
		y = 1.5f * std::exp (-t / 0.05f) * std::sin (TWO_PI * (80.0f + 60.0f * std::exp (-t / 0.01f)) * t);
		y += 0.3f * std::exp (-t / 0.03f) * std::sin (TWO_PI * 310.0f * t);
		if (t > 0.4f) v.kind = -1;
		break; }
	default: v.kind = -1;
	}
	v.t += dt;
	return v.s * y;
}

void ContactSynth::Render (float *out, unsigned n, float dop)
{
	// new blows (docking: the clunk, then two latches; undocking: the latches,
	// then the push-off)
	for (int k = 0; k < NBLOW; k++) {
		int c = pending[k].count.load (std::memory_order_acquire);
		if (c == played[k]) continue;
		played[k] = c;
		float s = pending[k].strength.load (std::memory_order_relaxed);
		if (k == DOCK) { Start (DOCK, s); Start (LATCH, 0.8f, 0.45f); Start (LATCH, 0.8f, 0.62f); }
		else if (k == UNDOCK) { Start (LATCH, 0.8f); Start (LATCH, 0.7f, 0.15f); Start (UNDOCK, 0.6f * s, 0.4f); }
		else if (k == GEARLOCK) { Start (GEARLOCK, s); Start (LATCH, 0.5f * s, 0.12f); }
		else Start (k, s);
	}

	const float speed = rollS.Step (pRollSpeed.load (std::memory_order_relaxed), dsp::Smooth::K (0.1f, n, rate));
	const float load = loadS.Step (pRollLoad.load (std::memory_order_relaxed), dsp::Smooth::K (0.1f, n, rate));
	const float squeal = squealS.Step (pSqueal.load (std::memory_order_relaxed), dsp::Smooth::K (0.03f, n, rate));
	const float scrape = scrapeS.Step (pScrape.load (std::memory_order_relaxed), dsp::Smooth::K (0.05f, n, rate));
	const float sspeed = pScrapeSpeed.load (std::memory_order_relaxed);

	// rolling: low rumble, brighter with speed; the wheel's turn and the
	// runway's joints (every 6 m) as a pulse
	rollLp.LowPass (std::clamp (90.0f + 2.0f * speed, 90.0f, 350.0f) * dop, 0.7f, rate);
	rollBp.BandPass (std::clamp (60.0f + 1.0f * speed, 60.0f, 200.0f) * dop, 1.2f, rate);
	const float rollAmp = std::sqrt (std::max (load, 0.0f)) * std::pow (std::min (speed / 40.0f, 1.5f), 1.3f);
	const float jointRate = speed / 6.0f * dop;
	// squeal: a tyre's skid, a nasal band near 800 Hz and its overtone
	squealBp.BandPass (780.0f * dop, 14.0f, rate);
	squealBp2.BandPass (1560.0f * dop, 16.0f, rate);
	// scrape: grinding - mid noise in grains, a low rumble under it
	scrapeBp.BandPass (std::clamp (250.0f + 6.0f * sspeed, 250.0f, 900.0f) * dop, 1.0f, rate);
	scrapeHp.HighPass (120.0f * dop, 0.7f, rate);
	scrapeAm.SetCutoff (40.0f, rate);
	thudLp.LowPass (180.0f * dop, 0.7f, rate);
	chirpBp.BandPass (750.0f * dop, 2.0f, rate);
	hydraulicBp.BandPass (450.0f * dop, 1.5f, rate);
	// motors: base pitch by kind (servo 180 Hz, a door's actuator 140, the
	// gear's hydraulics 90), up 20 % at full speed
	static const float motorF0[NMOTOR] = {180.0f, 140.0f, 90.0f};
	float mspeed[NMOTOR], mf[NMOTOR];
	bool motors = false;
	for (int k = 0; k < NMOTOR; k++) {
		mspeed[k] = motorS[k].Step (pMotor[k].load (std::memory_order_relaxed), dsp::Smooth::K (0.08f, n, rate));
		mf[k] = motorF0[k] * (0.85f + 0.35f * mspeed[k]) * dop;
		if (mspeed[k] > 1e-3f) motors = true;
	}
	crunchLp.LowPass (180.0f * dop, 0.7f, rate);

	for (unsigned i = 0; i < n; i++) {
		float w = rng.White ();
		float p = pink.Next (w);
		float s = 0.0f;
		if (rollAmp > 1e-4f) {
			rollPh += jointRate / rate;
			float joint = 0.0f;
			if (rollPh >= 1.0f) rollPh -= 1.0f;
			if (rollPh < 0.04f) joint = std::sin (rollPh / 0.04f * 3.14159f); // (a soft bump)
			s += 1.7f * rollAmp * (2.0f * rollLp.Process (p) + 1.5f * rollBp.Process (p) + 0.8f * joint * rollBp.Process (w));
		}
		if (squeal > 1e-4f)
			s += squeal * (6.0f * squealBp.Process (w) + 1.2f * squealBp2.Process (w));
		if (scrape > 1e-4f) {
			float grains = std::max (0.0f, 1.0f + 40.0f * scrapeAm.Lp (rng.White ()));
			s += scrape * (2.0f * grains * scrapeBp.Process (w) + 1.5f * scrapeHp.Process (thudLp.Process (p)));
		}
		if (motors) {
			for (int k = 0; k < NMOTOR; k++) {
				if (mspeed[k] <= 1e-3f) continue;
				float y = 0.0f;
				for (int h = 0; h < 3; h++) {
					motorPh[k][h] += TWO_PI * mf[k] * (h + 1) / rate;
					if (motorPh[k][h] > TWO_PI) motorPh[k][h] -= TWO_PI;
					y += std::sin (motorPh[k][h]) / (1.0f + 1.5f * h);
				}
				// (the gear's hydraulics: the fluid's hiss under the hum)
				if (k == GEAR) y += 1.5f * hydraulicBp.Process (p);
				s += 0.9f * mspeed[k] * y;
			}
		}
		// (the blows' noises, filtered once for all voices)
		const float chirp = chirpBp.Process (w), crunch = crunchLp.Process (p);
		for (auto &v : voices) {
			if (v.kind < 0) continue;
			if (v.delay > 0.0f) { v.delay -= 1.0f / rate; continue; }
			s += Voice1 (v, chirp, crunch);
		}
		out[i] = 0.3f * s;
	}
}

} // namespace audio

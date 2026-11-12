// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ContactSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	constexpr float TWO_PI = 6.2831853f;
	/// The rolling rumble's gain (its RMS at 40 m/s, full load: about 0.18 -
	/// gentle, 4 dB under the contacts' calibration)
	constexpr float ROLL_GAIN = 1.0f;
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
		// the strut's thump (70 Hz falling to 45 Hz), and the tyres' squeal as
		// they spin up: a tone sliding down (900 to 650 Hz), stuttering
		// (stick and slip), gone in about half a second; nearly as loud for a
		// gentle touchdown as a hard one (it is the speed that makes it)
		float f = 45.0f + 25.0f * std::exp (-t / 0.03f);
		v.ph += TWO_PI * f * dt;
		y = 1.6f * std::exp (-t / 0.12f) * std::sin (v.ph);
		float fs = 650.0f + 250.0f * std::exp (-t / 0.15f);
		v.ph2 += TWO_PI * fs * dt;
		v.ph3 += TWO_PI * 31.0f * dt;
		float stutter = 0.7f + 0.3f * std::sin (v.ph3) * std::sin (0.37f * v.ph3 + 1.0f);
		float env = (1.0f - std::exp (-t / 0.006f)) * std::exp (-t / 0.16f);
		float sq = std::sin (v.ph2) + 0.3f * std::sin (2.0f * v.ph2 + 0.5f) + 0.12f * std::sin (3.0f * v.ph2 + 1.3f);
		y += 2.2f / std::sqrt (std::max (v.s, 0.25f)) * env * stutter * sq;
		if (t > 0.8f) v.kind = -1;
		break; }
	case HULL: {
		// a heavy thud (55 Hz falling to 30 Hz) and a crunch of low noise
		float f = 30.0f + 25.0f * std::exp (-t / 0.05f);
		v.ph += TWO_PI * f * dt;
		y = 3.2f * std::exp (-t / 0.25f) * std::sin (v.ph);
		y += 2.6f * std::exp (-t / 0.3f) * crunch;
		if (t > 1.5f) v.kind = -1;
		break; }
	case CLANG: {
		// a vessel bumping another: a dull knock, the structure's short,
		// damped ring under it
		float f = 55.0f + 30.0f * std::exp (-t / 0.02f);
		v.ph += TWO_PI * f * dt, v.ph2 += TWO_PI * 170.0f * dt, v.ph3 += TWO_PI * 413.0f * dt;
		y = 2.0f * std::exp (-t / 0.09f) * std::sin (v.ph);
		y += 0.5f * std::exp (-t / 0.18f) * std::sin (v.ph2) + 0.15f * std::exp (-t / 0.07f) * std::sin (v.ph3);
		y += 1.5f * std::exp (-t / 0.02f) * crunch;
		if (t > 1.0f) v.kind = -1;
		break; }
	case DOCK: case UNDOCK: {
		// docking: two structures meeting - a heavy, dull clunk (75 Hz down to
		// 42), the knock of the contact (a burst of low noise), a short,
		// damped body resonance; undocking's push-off: a lighter thud
		const bool dock = v.kind == DOCK;
		float f = 42.0f + 33.0f * std::exp (-t / 0.03f);
		v.ph += TWO_PI * (dock ? f : 1.2f * f) * dt, v.ph2 += TWO_PI * 130.0f * dt;
		y = (dock ? 4.5f : 2.5f) * std::exp (-t / (dock ? 0.16f : 0.1f)) * std::sin (v.ph);
		y += (dock ? 3.5f : 1.8f) * std::exp (-t / 0.025f) * crunch;
		y += 0.6f * std::exp (-t / 0.05f) * std::sin (v.ph2);
		if (t > 0.9f) v.kind = -1;
		break; }
	case GEARLOCK: {
		// the gear locking: a heavy thunk (60 Hz falling to 40), then its latch
		float f = 40.0f + 20.0f * std::exp (-t / 0.02f);
		v.ph += TWO_PI * f * dt;
		y = 1.6f * std::exp (-t / 0.1f) * std::sin (v.ph) + 0.5f * crunch * std::exp (-t / 0.05f);
		if (t > 0.6f) v.kind = -1;
		break; }
	case LATCH: {
		// a latch engaging (or letting go): punchy - the clack of steel on
		// steel (a few milliseconds of mid noise), a thunk under it (130 Hz
		// down to 70)
		float f = 70.0f + 60.0f * std::exp (-t / 0.008f);
		v.ph += TWO_PI * f * dt;
		y = 3.6f * std::exp (-t / 0.035f) * std::sin (v.ph);
		y += 5.0f * std::exp (-t / 0.004f) * chirp;
		if (t > 0.3f) v.kind = -1;
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
		if (k == DOCK) { Start (DOCK, s); Start (LATCH, 1.0f, 0.45f); Start (LATCH, 1.0f, 0.62f); }
		else if (k == UNDOCK) { Start (LATCH, 1.0f); Start (LATCH, 0.9f, 0.15f); Start (UNDOCK, 0.9f * s, 0.4f); }
		else if (k == GEARLOCK) { Start (GEARLOCK, s); Start (LATCH, 0.5f * s, 0.12f); }
		else Start (k, s);
	}

	const float speed = rollS.Step (pRollSpeed.load (std::memory_order_relaxed), dsp::Smooth::K (0.1f, n, rate));
	const float load = loadS.Step (pRollLoad.load (std::memory_order_relaxed), dsp::Smooth::K (0.1f, n, rate));
	const float squeal = squealS.Step (pSqueal.load (std::memory_order_relaxed), dsp::Smooth::K (0.03f, n, rate));
	const float scrape = scrapeS.Step (pScrape.load (std::memory_order_relaxed), dsp::Smooth::K (0.05f, n, rate));
	const float sspeed = pScrapeSpeed.load (std::memory_order_relaxed);

	// rolling: a gentle, low rumble (under 150 Hz), swelling with the
	// wheels' turn (a tyre isn't quite round: 2.5 m around); the runway's
	// joints (every 6 m) soft, low thumps
	rollLp.LowPass (std::clamp (60.0f + 1.0f * speed, 60.0f, 150.0f) * dop, 0.6f, rate);
	rollBp.LowPass (35.0f * dop, 0.7f, rate);
	const float rollAmp = std::sqrt (std::max (load, 0.0f)) * std::pow (std::min (speed / 40.0f, 1.5f), 1.3f);
	const float jointRate = speed / 6.0f * dop, wheelRate = speed / 2.5f * dop;
	// squeal: a tyre's skid, a nasal band near 800 Hz and its overtone
	squealBp.BandPass (780.0f * dop, 14.0f, rate);
	squealBp2.BandPass (1560.0f * dop, 16.0f, rate);
	// scrape: grinding - mid noise in grains, a low rumble under it
	scrapeBp.BandPass (std::clamp (250.0f + 6.0f * sspeed, 250.0f, 900.0f) * dop, 1.0f, rate);
	scrapeHp.HighPass (120.0f * dop, 0.7f, rate);
	scrapeAm.SetCutoff (40.0f, rate);
	thudLp.LowPass (180.0f * dop, 0.7f, rate);
	chirpBp.BandPass (1200.0f * dop, 1.2f, rate); // (a latch's clack)
	hydraulicBp.BandPass (450.0f * dop, 1.5f, rate);
	// motors: base pitch by kind (servo 180 Hz, a door's actuator 140, the
	// gear's hydraulics 90), up 20 % at full speed
	static const float motorF0[NMOTOR] = {180.0f, 140.0f, 90.0f};
	float mspeed[NMOTOR], mf[NMOTOR];
	bool motors = false;
	for (int k = 0; k < NMOTOR; k++) {
		mspeed[k] = motorS[k].Step (pMotor[k].load (std::memory_order_relaxed), dsp::Smooth::K (0.08f, n, rate));
		mf[k] = motorF0[k] * (0.85f + 0.35f * mspeed[k]) * dop;
		motorWob[k].SetCutoff (3.0f, rate);
		if (mspeed[k] > 1e-3f) motors = true;
	}
	crunchLp.LowPass (180.0f * dop, 0.7f, rate);

	for (unsigned i = 0; i < n; i++) {
		float w = rng.White ();
		float p = pink.Next (w);
		float s = 0.0f;
		if (rollAmp > 1e-4f) {
			rollPh += jointRate / rate;
			if (rollPh >= 1.0f) rollPh -= 1.0f, jointT = 0.0f; // (a joint: its thump starts)
			wheelPh += wheelRate / rate;
			if (wheelPh >= 1.0f) wheelPh -= 1.0f;
			jointT += 1.0f / rate;
			float joint = std::exp (-jointT / 0.04f) * std::sin (TWO_PI * 45.0f * jointT);
			float turn = 1.0f + 0.3f * std::sin (TWO_PI * wheelPh);
			s += rollAmp * (ROLL_GAIN * turn * (rollLp.Process (p) + 1.5f * rollBp.Process (p)) + 0.4f * joint);
		}
		if (squeal > 1e-4f) {
			// a skid's squeal: a tone near 780 Hz, wavering (stick and slip),
			// its overtone, a little of the rubber's hiss in its band
			squealPh += TWO_PI * 780.0f * dop * (1.0f + 0.015f * std::sin (squealWob)) / rate;
			squealWob += TWO_PI * 23.0f / rate;
			if (squealPh > TWO_PI) squealPh -= TWO_PI;
			if (squealWob > TWO_PI) squealWob -= TWO_PI;
			const float amp = 0.75f + 0.25f * std::sin (2.7f * squealWob + 0.4f);
			s += squeal * (amp * (0.9f * std::sin (squealPh) + 0.25f * std::sin (2.0f * squealPh + 0.7f)) + 1.5f * squealBp.Process (w));
		}
		if (scrape > 1e-4f) {
			float grains = std::max (0.0f, 1.0f + 40.0f * scrapeAm.Lp (rng.White ()));
			s += scrape * (2.0f * grains * scrapeBp.Process (w) + 1.5f * scrapeHp.Process (thudLp.Process (p)));
		}
		if (motors) {
			for (int k = 0; k < NMOTOR; k++) {
				if (mspeed[k] <= 1e-3f) continue;
				float y = 0.0f;
				// (its load wavering: a few per cent in pitch, a quarter in level)
				const float wob = std::clamp (60.0f * motorWob[k].Lp (rng.White ()), -1.0f, 1.0f);
				for (int h = 0; h < 3; h++) {
					motorPh[k][h] += TWO_PI * mf[k] * (1.0f + 0.03f * wob) * (h + 1) / rate;
					if (motorPh[k][h] > TWO_PI) motorPh[k][h] -= TWO_PI;
					y += std::sin (motorPh[k][h]) / (1.0f + 1.5f * h);
				}
				// (the gear's hydraulics: the fluid's hiss under the hum)
				if (k == GEAR) y += 1.5f * hydraulicBp.Process (p);
				// (a servo's and a door's actuator: small motors, well under
				// the gear's hydraulics)
				static const float motorGain[NMOTOR] = {0.3f, 0.35f, 0.9f};
				s += motorGain[k] * mspeed[k] * (1.0f + 0.25f * wob) * y;
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

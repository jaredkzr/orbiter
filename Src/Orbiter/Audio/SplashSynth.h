// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Water: a vessel meeting it (a splashdown, a ditching, a wreck falling
// in) and moving through it. Liquid sound is, almost all of it, bubbles
// (Minnaert 1933; van den Doel, "Physically based models for liquid
// sounds", 2005):
//
//  - a bubble of radius r rings at f = 3.26 m/s / r (1 mm: 3.3 kHz, 1 cm:
//    330 Hz), damped within tens of cycles (thermal and radiation losses:
//    d = 0.043 f + 0.0014 f^1.5 per second), its pitch rising a little as
//    it rises to the surface
//  - an entry: the slam (the water struck: a short broadband slap), the
//    cavity's large bubbles (low, few - more for a bigger body), the spray's
//    small ones as its drops fall back (many, over the time they are up:
//    about the entry speed / g), the rush of the spray sheet
//  - afloat or moving: bubbles at a rate growing with the speed through
//    the water (the water lapping, the wake)
//
// Strength: a part of the entry's kinetic energy (SPLASH_EFFICIENCY)
// radiated as sound; the emitter's level makes it so. Output normalized:
// about SPLASH_RMS over the first second of a full-strength entry.
// =======================================================================

#ifndef __SPLASHSYNTH_H
#define __SPLASHSYNTH_H

#include "Synth.h"

namespace audio {

class SplashSynth : public SynthSource {
public:
	static SourcePtr<SplashSynth> Make (Engine &e, uint32_t seed);

	static constexpr double SPLASH_EFFICIENCY = 3e-6;
	static constexpr double SPLASH_RMS = 0.3;
	/// Minnaert's frequency [Hz] of a bubble of radius r [m]
	static double Minnaert (double r) { return 3.26 / r; }

	/// An entry: its strength (1: the emitter's full level), the body's size
	/// [m], the entry speed [m/s]
	void Entry (float strength, float size, float speed);
	/// Afloat or moving through the water: its speed through it [m/s] (0:
	/// still, or out of it); the wake's gain against the emitter's level
	void Set (float waterSpeed, float size, float wakeGain);

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit SplashSynth (uint32_t seed): rng (seed) {}
	struct EntryEvent { float strength, size, speed; };
	dsp::Events<EntryEvent, 8> entries;   // (in order: each begins the splash afresh)
	std::atomic<float> pSpeed{0.0f}, pSize{1.0f}, pWake{1.0f};

	// (audio thread)
	dsp::Rng rng;
	dsp::Pink pink;
	dsp::Biquad slamHp, slamLp, rushBp1, rushBp2;
	dsp::Smooth speedS;
	/// a bubble: a damped resonator (y = 2 r cos w y1 - r^2 y2), its pitch
	/// (w) rising, updated each block; env: its amplitude now, as it decays
	struct Bubble { float y1 = 0.0f, y2 = 0.0f, c = 0.0f, r2 = 0.0f, f = 0.0f, rise = 0.0f, d = 0.0f, a = 0.0f, env = 0.0f, t = 0.0f; bool on = false; };
	static constexpr int NBUBBLE = 384;
	Bubble bubbles[NBUBBLE];
	// the entry sounding: its time, strength, the spray's time aloft, the
	// cavity's largest bubbles' radius
	float et = 1e9f, es = 0.0f, eSpray = 1.0f, eRmax = 0.01f, eSize = 1.0f;
	float bubbleDebt = 0.0f, wakeDebt = 0.0f;
	void Spawn (float r, float amp);
	float RadiusLogUniform (float rmin, float rmax, float alpha);
};

} // namespace audio

#endif // !__SPLASHSYNTH_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// An explosion's blast wave, as it reaches a listener:
//
//  - its strength and length from the charge (a TNT equivalent W [kg]) and
//    the distance R, by Kinney & Graham's fits for a chemical explosion in
//    sea-level air (Explosive Shocks in Air, 1985), scaled to the air
//    there (Sachs: distances by (P/P0)^1/3, times also by c0/c)
//  - its shape: the Friedlander wave - an instant rise to the peak
//    overpressure dP, a fall through zero at the positive phase's end td, a
//    longer, shallower negative phase: p(t) = dP (1 - t/td) exp (-b t/td)
//  - then the reflections from the ground and terrain about (a few,
//    weaker, later as the listener is further), and the rumble they
//    blur into: low, dying away over seconds
//
// The waveform is normalized (1: dP); the emitter's level makes it dP at
// the listener (the world spreads it, absorbs its highs over the
// distance, delays it, carries it into a cockpit muffled, not across
// vacuum).
// =======================================================================

#ifndef __BLASTSYNTH_H
#define __BLASTSYNTH_H

#include "Synth.h"

namespace audio {

class BlastSynth : public SynthSource {
public:
	static std::unique_ptr<BlastSynth> Make (Engine &e, uint32_t seed);

	/// Peak overpressure [Pa] at scaled distance Z = R / W^1/3 [m/kg^1/3] in
	/// sea-level air (Kinney & Graham)
	static double Overpressure (double Z);
	/// The positive phase's length [s] at scaled distance Z, for a charge W
	/// [kg TNT], in sea-level air (Kinney & Graham)
	static double Duration (double Z, double W);
	/// TNT equivalent [kg] of a vessel's propellant [kg] going up (liquid
	/// propellants mixing and burning in a breakup: about a tenth by mass);
	/// doubled near the ground (a surface burst: its energy into a
	/// hemisphere, reflected - 1.8 as fits put it)
	static double TntEquivalent (double propellant, bool surface);
	static constexpr double YIELD = 0.1, SURFACE = 1.8;

	/// The listener's distance [m] and the blast's positive phase there [s]
	/// (simulation thread; set until it is heard)
	void Set (double R, double td);
	/// The blast arrives: from now on it plays
	void Fire () { fire.store (true, std::memory_order_release); }
	/// It has played to its end
	bool Done () const override { return done.load (std::memory_order_acquire); }

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit BlastSynth (uint32_t seed): rng (seed) {}
	std::atomic<float> pR{1000.0f}, pTd{0.02f};
	std::atomic<bool> fire{false}, done{false};

	// (audio thread)
	dsp::Rng rng;
	dsp::Pink pink;
	dsp::Biquad rumbleLp, rumbleLp2;
	dsp::OnePole rise;
	bool started = false;
	float t = 0.0f, td = 0.02f, len = 6.0f, tailT60 = 2.0f;
	static constexpr int NECHO = 6;
	float echoT[NECHO] = {}, echoA[NECHO] = {};
	static float Friedlander (float t, float td);
};

} // namespace audio

#endif // !__BLASTSYNTH_H

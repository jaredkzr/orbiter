// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The roar of a rocket engine, synthesized from its throttle and size:
//
//  - roar: the jet's turbulent mixing noise, a broad hump of pink noise
//    whose peak lies at a Strouhal number of about 0.2-0.3 (f = St U / D:
//    lower for a bigger engine: tens of hertz for a launcher, hundreds for
//    a small engine), flickering with the turbulence
//  - crackle: the shocklets of a supersonic jet, sharp one-sided pops at
//    random, with a heavy-tailed spread of strengths
//  - rumble: the sub-bass (30-80 Hz; big engines are felt)
//  - the turbopumps' whine: a faint tone and its harmonic, spooling up after
//    ignition with the throttle, winding down after shutdown
//  - ignition (a thump and a burst of crackle) and shutdown (a chuff)
//
// The output is normalized (about -10 dBFS RMS at full throttle); the
// emitter's level makes it as loud as it physically is.
// =======================================================================

#ifndef __ROCKETSYNTH_H
#define __ROCKETSYNTH_H

#include "Synth.h"
#include <algorithm>

namespace audio {

class RocketSynth : public SynthSource {
public:
	bool Continuous () const override { return true; }
	float Activity () const override { return std::max (pLevel.load (std::memory_order_relaxed), outLevel.load (std::memory_order_relaxed)); }
	struct Params {
		float level = 0.0f;     // throttle 0..1
		float peak = 200.0f;    // roar's spectral peak [Hz]
		float crackle = 0.5f;   // 0..1
		float rumble = 0.0f;    // 0..1 (the sub-bass)
		float whine = 0.15f;    // 0..1 (the turbopumps)
		float pump = 1500.0f;   // the pumps' whine at full speed [Hz]
	};
	static std::unique_ptr<RocketSynth> Make (Engine &e, uint32_t seed);

	/// Set from the simulation thread
	void Set (const Params &p);

	/// The spectral peak for an engine of a thrust [N] (its size: the
	/// nozzle's diameter goes about with the square root of the thrust)
	static float PeakFor (double maxThrust);
	/// The sub-bass for an engine of a thrust [N]: some for every engine, more
	/// for big ones
	static float RumbleFor (double maxThrust);
	/// The turbopumps' whine [Hz] for an engine of a thrust [N] (bigger
	/// pumps turn slower)
	static float PumpFor (double maxThrust);

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit RocketSynth (uint32_t seed): rng (seed) {}
	std::atomic<float> pLevel{0.0f}, pPeak{200.0f}, pCrackle{0.5f}, pRumble{0.0f}, pWhine{0.15f}, pPump{1500.0f};
	std::atomic<float> outLevel{0.0f}; // (the spooled level, for Activity)

	// (audio thread)
	dsp::Rng rng;
	dsp::Pink pink, pinkHi;
	dsp::Biquad roarBp, roarLp, hiss, crackHp, rumbleLp, rumbleHp, chuffBp;
	dsp::OnePole flutter, crackShape;
	dsp::Smooth level, peakS;
	float flutterK = 0.0f;
	float thump = 0.0f, thumpPh = 0.0f, burst = 0.0f, chuff = 0.0f;
	float pumpSpeed = 0.0f, pumpPh1 = 0.0f, pumpPh2 = 0.0f; // (0..1 of full speed)
	dsp::OnePole pumpWobble;
	bool lit = false;
	float crackRun = 0.0f; // (time to the next crackle pop, in samples)
	bool first = true;
};

} // namespace audio

#endif // !__ROCKETSYNTH_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The air rushing over a vessel: the turbulent boundary layer's broadband
// rush (brighter the faster it flies: its frequencies go with speed), the
// low, irregular buffeting of separated flow (at a high angle of attack,
// and through the transonic range), gusts. Normalized like the other
// synthesizers: the emitter's level is the dynamic pressure's.
// =======================================================================

#ifndef __WINDSYNTH_H
#define __WINDSYNTH_H

#include "Synth.h"
#include <algorithm>

namespace audio {

class WindSynth : public SynthSource {
public:
	bool Continuous () const override { return true; }
	float Activity () const override { return pSpeed.load (std::memory_order_relaxed) > 0.5f || pBuffet.load (std::memory_order_relaxed) > 0.0f ? 1.0f : 0.0f; }
	struct Params {
		float speed = 0.0f;   // airspeed [m/s]
		float buffet = 0.0f;  // 0..1
	};
	static std::unique_ptr<WindSynth> Make (Engine &e, uint32_t seed);
	void Set (const Params &p);

	/// Buffeting for an angle of attack [rad] and a Mach number: separated
	/// flow beyond about 12 deg, shock buffet from Mach 0.8 to 1.2
	static float BuffetFor (double aoa, double mach);

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit WindSynth (uint32_t seed): rng (seed) {}
	std::atomic<float> pSpeed{0.0f}, pBuffet{0.0f};
	dsp::Rng rng;
	dsp::Pink pink;
	dsp::Biquad rushBp, rushLp, buffLp, buffHp;
	dsp::OnePole gust, buffEnv;
	dsp::Smooth speed, buffet;
	float gustK = 0.0f, buffK = 0.0f;
	bool first = true;
};

} // namespace audio

#endif // !__WINDSYNTH_H

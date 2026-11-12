// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// A sonic boom: the N-wave of a supersonic body's shock cone sweeping
// over the listener - a jump up, a linear fall through zero, a jump back,
// tens to hundreds of milliseconds apart (the double crack), the jumps
// rounded by a few milliseconds of turbulence; and a rumble after it,
// echoes from the ground and the air. Played once, at full scale (the
// way's gain makes it as strong as it is).
//
// Overpressure and length (Carlson's simplified method, NASA SP-1122):
//   dp = K_r sqrt(p_v p_g) (M^2-1)^(1/8) h^(-3/4) l^(3/4) K_s
//   T  = K_t 3.42 / a_v  M / (M^2-1)^(3/8) h^(1/4) l^(3/4) sqrt(K_s)
// with l the body's length, h its height above the listener, K_r = 2
// (reflection from the ground), K_s about 0.08 (shape); K_t = 0.3 fits
// Concorde's measured N-wave (93 Pa, 0.3 s at Mach 2, 17 km).
// =======================================================================

#ifndef __BOOMSYNTH_H
#define __BOOMSYNTH_H

#include "Synth.h"

namespace audio {

class BoomSynth : public SynthSource {
public:
	/// duration: the N-wave's length [s]
	static std::unique_ptr<BoomSynth> Make (Engine &e, float duration, uint32_t seed);

	/// Peak overpressure [Pa] and duration [s] of the boom of a body of
	/// length l [m] at Mach M, h [m] above the listener, with the pressures
	/// at the body (pv) and the listener (pg) [Pa], and the speed of sound
	/// at the body (av) [m/s]
	static void Strength (double l, double M, double h, double pv, double pg, double av, double &dp, double &T);

	/// The whole sound's length [s]
	float Length () const { return dur + 1.5f; }

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	BoomSynth (float d, uint32_t seed): dur (d), rng (seed) {}
	float dur;
	dsp::Rng rng;
	dsp::Pink pink;
	dsp::Biquad rumbleLp;
	dsp::OnePole edge;
	unsigned pos = 0;
	bool setup = false;
};

} // namespace audio

#endif // !__BOOMSYNTH_H

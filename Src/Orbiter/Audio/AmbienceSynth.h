// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The air and the sea about a listener at rest outdoors: its output is the
// pressure at the listener [Pa].
//
//  - wind: the flow about the head (and anything near it) - a turbulent
//    rumble, mostly below a couple of hundred hertz, its level some 0.1 %
//    of the wind's dynamic pressure (1/2 rho U^2: 63 dB in a 7 m/s wind on
//    Earth; Mars' thin air, a hundredth the density, near 30 dB less),
//    gusting (turbulence of some 20 %, over seconds)
//  - the sea: its whitecaps' bubbles (the breaking waves cover a fraction
//    W = 3.84e-6 U^3.41 of the water: Monahan & O'Muircheartaigh 1980),
//    the wash of its waves, swelling with each wave (the peak period Tp)
//
// (Rain isn't simulated; nor is what the wind moves - grass, trees.)
// =======================================================================

#ifndef __AMBIENCESYNTH_H
#define __AMBIENCESYNTH_H

#include "Synth.h"

namespace audio {

class AmbienceSynth : public SynthSource {
public:
	static std::unique_ptr<AmbienceSynth> Make (Engine &e, uint32_t seed);

	/// The wind's noise at the ear, of its dynamic pressure
	static constexpr double WIND_EAR = 0.001;
	/// Whitecap coverage for a wind 10 m up [m/s] (Monahan)
	static double Whitecaps (double U) { return 3.84e-6 * std::pow (std::max (U, 0.0), 3.41); }
	/// The sea's noise [Pa] near open water for a wind [m/s]: 0.02 Pa
	/// (60 dB) at 7 m/s, as the square root of the whitecaps (incoherent)
	static double SeaPressure (double U) { return 0.02 * std::sqrt (Whitecaps (U) / Whitecaps (7.0)); }

	/// Set: the wind's noise [Pa] and speed [m/s]; the sea's [Pa], its
	/// waves' period [s]
	void Set (double windP, double windSpeed, double seaP, double period);

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit AmbienceSynth (uint32_t seed): rng (seed) {}
	std::atomic<float> pWind{0.0f}, pSpeed{0.0f}, pSea{0.0f}, pPeriod{8.0f};

	// (audio thread)
	dsp::Rng rng;
	dsp::Pink pink, pink2;
	dsp::Biquad windLp, windLp2, whooshBp, washHp, washLp, foamHp, foamLp;
	dsp::OnePole gust, gust2;
	dsp::Smooth windS, seaS;
	float wavePh = 0.0f;
};

} // namespace audio

#endif // !__AMBIENCESYNTH_H

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
	static SourcePtr<AmbienceSynth> Make (Engine &e, uint32_t seed);

	/// The wind's noise at the ear, of its dynamic pressure (0.001 till
	/// 2026-11-16: a 4.5 m/s breeze 56 dB, over a vessel's mechanisms outside
	/// - by ear the scene a roar; now 42 dB: a quiet outdoor background)
	static constexpr double WIND_EAR = 0.0002;
	/// Whitecap coverage for a wind 10 m up [m/s] (Monahan)
	static double Whitecaps (double U) { return 3.84e-6 * std::pow (std::max (U, 0.0), 3.41); }
	/// The sea's noise [Pa] near open water for a wind [m/s]: 0.02 Pa
	/// (60 dB) at 7 m/s, as the square root of the whitecaps (incoherent)
	static double SeaPressure (double U) { return 0.02 * std::sqrt (Whitecaps (U) / Whitecaps (7.0)); }
	/// How much of that is heard [0..1], by the water about the listener: the
	/// share of it within SEA_R (open water on one side of a beach: about
	/// half - full; scattered marsh or a ditch: little) and the distance to
	/// the nearest (as 1/r beyond SEA_NEAR). 2026-11-16: any water within
	/// 300 m played full surf - the marsh by KSC's runway as a beach,
	/// drowning the vessels
	static constexpr double SEA_R = 400.0, SEA_NEAR = 30.0;
	static double SeaNearness (double share, double nearest)
	{
		if (share <= 0.0) return 0.0;
		return std::min (1.0, std::sqrt (std::min (share, 1.0) / 0.5)) * std::min (1.0, SEA_NEAR / std::max (nearest, SEA_NEAR));
	}

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

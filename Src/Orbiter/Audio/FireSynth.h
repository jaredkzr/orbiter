// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The roar of a fire: burning wreckage (Debris::Ignite).
//
// A turbulent flame turns a small part of its heat release into sound
// (about 3e-6 for open, turbulent hydrocarbon flames: Strahle; Rajaram &
// Lieuwen): broadband, a hump an octave or two wide whose peak falls as the
// fire grows (some hundreds of hertz for a small fire, tens for a large
// one), throbbing at the fire's puffing frequency (the large eddies shed
// from its base: about 1.5 / sqrt(D) Hz for a fire D metres across) and
// fluctuating with its turbulence.
//
// The output is normalized (about FIRE_RMS); the emitter's level makes it
// as loud as the fire's acoustic power.
// =======================================================================

#ifndef __FIRESYNTH_H
#define __FIRESYNTH_H

#include "Synth.h"

namespace audio {

class FireSynth : public SynthSource {
public:
	static std::unique_ptr<FireSynth> Make (Engine &e, uint32_t seed);

	/// Part of the heat release radiated as sound
	static constexpr double EFFICIENCY = 3e-6;
	/// Heat of combustion of a propellant mix, burning in the air [J/kg]
	static constexpr double HEAT = 10e6;
	/// The normalized output's RMS
	static constexpr double FIRE_RMS = 0.3;
	/// The spectrum's peak [Hz] for a heat release [W]
	static double PeakFor (double heatRelease);
	/// Its diameter [m] for a heat release [W]: a pool of burning
	/// propellant releases about 2 MW per square metre
	static double DiameterFor (double heatRelease);

	/// Set from the simulation thread: peak [Hz], diameter [m], level 0..1
	void Set (double peak, double diameter, double level);

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit FireSynth (uint32_t seed): rng (seed) {}
	std::atomic<float> pPeak{100.0f}, pDiam{5.0f}, pLevel{0.0f};

	// (audio thread)
	dsp::Rng rng;
	dsp::Pink pink;
	dsp::Biquad hp, lp, lp2, rumble;
	dsp::OnePole flicker;
	dsp::Smooth levelS;
	float puffPh = 0.0f;
};

} // namespace audio

#endif // !__FIRESYNTH_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The structure under load, heard in its cabin: creaks and groans.
//
// A loaded structure creaks where its joints and panels stick and slip:
// a burst of micro-slips, each a tiny impulse, coming tens to a couple of
// hundred times a second (heard as a pitch, gliding as the slip speeds up
// or slows), ringing the structure's modes - panels some hundreds of hertz
// (a creak), frames and longerons lower (a groan, slower, longer). They
// come at random, more often the faster the load changes (stick-slip
// releases on a changing load) and the higher it is.
//
// Subtle: only through the structure, in the cabin; a fraction of a
// pascal at the most.
// =======================================================================

#ifndef __CREAKSYNTH_H
#define __CREAKSYNTH_H

#include "Synth.h"

namespace audio {

class CreakSynth : public SynthSource {
public:
	static SourcePtr<CreakSynth> Make (Engine &e, uint32_t seed);
	enum Kind { CREAK = 0, GROAN = 1 };
	/// One (strength 0..1)
	void Strike (Kind k, float strength);

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit CreakSynth (uint32_t seed): rng (seed) {}
	struct StrikeEvent { int kind; float strength; };
	dsp::Events<StrikeEvent, 16> strikes;   // (in order, each one)

	// (audio thread)
	dsp::Rng rng;
	struct Voice {
		bool on = false;
		float t = 0.0f, len = 0.3f, s = 0.0f;
		float rate0 = 60.0f, rate1 = 90.0f;   // the slips' rate, gliding [1/s]
		float next = 0.0f;                    // time to the next slip [s]
		dsp::Biquad modeA, modeB;
		float ga = 1.0f, gb = 0.5f;
	};
	Voice voices[4];
	void Start (Kind k, float s);
};

} // namespace audio

#endif // !__CREAKSYNTH_H

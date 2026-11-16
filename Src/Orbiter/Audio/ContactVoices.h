// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// A vessel's contacts as heard: recordings of blows (a SampleBank's takes)
// played as one-shots, ringing the vessel's body (the design:
// ~/audio-gen/casting/contacts-design.md, layers A-D).
//
//  - one-shots (OneShot: a clip, its gain, rate, a low-pass, its share in
//    the body), up to VOICES at once - a new one takes the quietest's place
//  - the body: MODES damped modes of the airframe, rung by every one-shot
//    (a send), the lowest about 25 Hz for a 30 m vessel, 60 Hz for a 5 m
//    one; ringing 0.6 s (the lowest) to 0.15 s (the highest)
//
// Two of them per vessel, by profile: AIR (outside - the recordings, the
// body under them, -10 dB) and STRUCTURE (its own structure, in the cabin -
// the body, the recordings dulled under it, -12 dB below 800 Hz).
//
// The one-shots come from the simulation thread (Fire: an emitter releases
// them when their sound arrives), in order, each once; nothing is
// allocated while rendering.
// =======================================================================

#ifndef __AUDIOCONTACTVOICES_H
#define __AUDIOCONTACTVOICES_H

#include "Synth.h"
#include "Assets.h"

namespace audio {

class ContactVoices : public SynthSource {
public:
	enum Profile { AIR = 0, STRUCTURE = 1 };
	/// One-shots' kinds (the design's layers): the blow, the strut, the
	/// cabin's rattle
	enum Kind { IMPACT = 1, STRUT = 2, RATTLE = 3 };
	static constexpr int VOICES = 16, MODES = 12;

	/// size: the vessel's [m] (its body's modes); seed: their spread
	static SourcePtr<ContactVoices> Make (Engine &e, Profile p, double size, uint32_t seed);
	/// A one-shot of a clip (mono): its gain, playback rate, low-pass [Hz],
	/// its share in the body (0..1)
	static OneShot Shot (int kind, const Clip *clip, float gain, float rate, float cutoff, float send);
	bool Fire (const OneShot &s, float delay) override;

	/// The body's lowest mode for a vessel of a size [Hz]
	static float BodyFreq (double size);
	/// (tests, benchmarks) a block rendered here, not through the audio
	/// graph: only while not started
	void RenderBlock (float *out, unsigned n, float dop = 1.0f) { Render (out, n, dop); }
	/// (tests) one-shots playing; a mode's frequency [Hz] and decay [s]
	int Playing () const { return playing.load (std::memory_order_relaxed); }
	float ModeFreq (int i) const { return modeF[i]; }
	float ModeTau (int i) const { return modeTau[i]; }

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	ContactVoices (Profile p, double size, uint32_t seed);
	Profile profile;
	struct Start { const Clip *clip; float gain, rate, cutoff, send, delay; };
	dsp::Events<Start, 64> starts;
	std::atomic<int> playing{0};
	float modeF[MODES] = {}, modeTau[MODES] = {}, modeAmp[MODES] = {};
	bool modesSet = false;

	// (audio thread)
	struct Voice {
		const Clip *clip = nullptr;
		double pos = 0.0;
		float rate = 1.0f, gain = 0.0f, send = 0.0f;
		unsigned wait = 0;     // [samples] before it starts
		dsp::OnePole lp;
	};
	Voice voices[VOICES];
	dsp::Resonator modes[MODES];
	float modeIn[MODES] = {};  // (each mode's input scale: alike rung whatever its decay)
	dsp::OnePole dull;         // (STRUCTURE: the recordings through the structure)
	void Begin (const Start &s);
	void SetModes ();
};

} // namespace audio

#endif // !__AUDIOCONTACTVOICES_H

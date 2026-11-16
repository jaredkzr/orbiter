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
// Two of them per vessel, by profile: AIR (outside - the recordings) and
// STRUCTURE (its own structure, in the cabin - the recordings dulled). The
// body's modes are there but off (by ear a gong: the recordings carry it).
//
// Beds (stage 2): sounds going on while it rolls or brakes - ROLL, BRAKE -
// from steady recordings (up to two takes each): random stretches of them
// (grains, GRAIN_MIN..GRAIN_MAX long) one after another, each crossfading
// into the next over GRAIN_XFADE at equal power, so no loop is heard; their
// level and rate eased to what SetBed gives (the level: a take's RMS as
// 1 - SetBedTakes gives each take's 1 / RMS). In the cabin the rolling a
// rumble (below 400 Hz), the brakes below 900 Hz. Stage 3: SCRAPE, the hull
// grinding on the ground - its brightness a low-pass set with it (SetBed's
// cutoff, eased), in the cabin below 600 Hz (the grind through the hull).
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
	enum Kind { IMPACT = 1, STRUT = 2, RATTLE = 3, CHIRP = 4, TEAR = 5, CLATTER = 6, CRASH = 7 };
	static constexpr int VOICES = 16, MODES = 12;
	enum Bed { ROLL = 0, BRAKE = 1, SCRAPE = 2, BEDS = 3 };
	static constexpr float GRAIN_MIN = 0.35f, GRAIN_MAX = 0.8f, GRAIN_XFADE = 0.06f;   // [s]

	/// size: the vessel's [m] (its body's modes); seed: their spread
	static SourcePtr<ContactVoices> Make (Engine &e, Profile p, double size, uint32_t seed);
	/// A one-shot of a clip (mono): its gain, playback rate, low-pass [Hz],
	/// its share in the body (0..1); len: only so long of it [s], faded out
	/// over its last 40 % (0: all of it)
	static OneShot Shot (int kind, const Clip *clip, float gain, float rate, float cutoff, float send, float len = 0.0f);
	bool Fire (const OneShot &s, float delay) override;
	/// A bed's takes (mono; b may be null) and their gains (1 / RMS: a take
	/// at its gain is unit RMS); the bed silent until it has one. False: not
	/// taken (try again)
	bool SetBedTakes (int bed, const Clip *a, float ga, const Clip *b = nullptr, float gb = 1.0f);
	/// A bed's level (unit RMS: as a one-shot's gain) and playback rate, eased
	/// to (rising over about 0.1 s, falling over 0.15 s); cutoff: a low-pass
	/// on it [Hz] (0: none), eased as the rate
	void SetBed (int bed, float gain, float rate, float cutoff = 0.0f);
	/// (tests) a bed's level now (eased)
	float BedLevel (int bed) const { return bedNow[bed].load (std::memory_order_relaxed); }

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
	struct Start { const Clip *clip; float gain, rate, cutoff, send, delay, len; };
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
		unsigned len = 0, played = 0;   // [samples] to play (0: all), played
		dsp::OnePole lp;
	};
	// the beds
	struct BedTakes { int bed; const Clip *take[2]; float g[2]; };
	dsp::Events<BedTakes, 8> bedTakes;
	std::atomic<float> bedGain[BEDS], bedRate[BEDS], bedNow[BEDS], bedCut[BEDS];
	struct Grain { const Clip *clip = nullptr; float g = 1.0f; double pos = 0.0; unsigned left = 0, len = 0; };
	struct BedState {
		const Clip *take[2] = {};
		float tg[2] = {1.0f, 1.0f};
		int n = 0;
		Grain gr[2];
		int cur = 0;           // (the grain playing; the other: fading in, while its len - left < xfade)
		float gain = 0.0f, rate = 1.0f;
		float held = 0.0f;     // (its level while it was wanted: going to nothing, cut 60 dB under it)
		dsp::OnePole lp[2];    // (STRUCTURE: dulled)
		dsp::OnePole tone[2];  // (its cutoff: brightness)
		float cut = 0.0f;      // (the cutoff now [Hz], eased; 0: none)
		dsp::Rng rng;
	};
	BedState beds[BEDS];
	static constexpr int XF_TAB = 4096;
	float xfTab[XF_TAB + 1] = {};   // (sin over a quarter turn: the crossfades' gains)
	unsigned xfade = 2880;          // [samples]
	void NewGrain (BedState &b, Grain &g, float rateNow);
	float BedSample (BedState &b, float dop);
	Voice voices[VOICES];
	dsp::Resonator modes[MODES];
	float modeIn[MODES] = {};  // (each mode's input scale: alike rung whatever its decay)
	dsp::OnePole dull;         // (STRUCTURE: the recordings through the structure)
	void Begin (const Start &s);
	void SetModes ();
};

} // namespace audio

#endif // !__AUDIOCONTACTVOICES_H

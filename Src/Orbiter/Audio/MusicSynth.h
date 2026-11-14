// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Generative music: ambient, made as it plays, following a mood (the
// situation: Music.h). Not a sound of the world - it isn't heard through
// air or hull - but the player's, on the music bus.
//
// The composer: a tonic for the session and a mode for each mood; chords
// on the mode's degrees (a weighted walk, coming home to the tonic now and
// then), voiced about a register, held seconds to half a minute and
// crossfaded (common tones held over); under them a bass (on the chord's
// root, or a pedal on the tonic); now and then a phrase of bells; in some
// moods a soft pulse, an arpeggio of the chord. All of it additive (sine
// partials: no aliasing), the pads' partials doubled and detuned a few
// cents (a chorus, spread left and right), through a feedback delay
// network's reverb (eight lines, Hadamard mixing, T60 some 7 s).
//
// A mood's change shows at the next chord (brought forward to a second or
// two); levels, brightness and the pulse glide. Silence: all of it fades
// (a second or so) and new notes stop.
// =======================================================================

#ifndef __MUSICSYNTH_H
#define __MUSICSYNTH_H

#include "Synth.h"
#include <memory>
#include <vector>

namespace audio {
namespace music {

enum Mood : int {
	SILENT,     ///< nothing (a critical moment)
	MENU,       ///< the launchpad
	GROUND,     ///< on or near a planet: landed, flying in its air
	ASCENT,     ///< a powered climb to orbit
	ORBIT,      ///< in space about a body
	DEEP,       ///< far from any body: between the planets, beyond a moon
	APPROACH,   ///< closing in on another vessel
	REENTRY,    ///< falling into the air, fast
	NMOOD
};
const char *Name (Mood m);

class Composer {
public:
	Composer (float rate, uint32_t seed);
	~Composer ();
	Composer (const Composer&) = delete;
	Composer &operator= (const Composer&) = delete;

	void SetMood (Mood m);
	/// How far along the mood is (0..1): an ascent's progress (brighter,
	/// a stronger pulse), an approach's closeness
	void SetIntensity (float x);
	/// Render n frames (two channels, not interleaved)
	void Render (float *l, float *r, unsigned n);

	// (tests)
	Mood GetMood () const;
	int Tonic () const;             ///< the session's tonic (a pitch class, 0: C)
	int Chords () const;            ///< chords played so far
	int Bells () const;             ///< bells struck so far
	int Pulses () const;            ///< pulse notes so far
	int Sounding () const;          ///< notes sounding now
	std::vector<int> Chord () const; ///< the chord's notes (MIDI): the bass, then the pads
	/// The overall level now (0..1 of the mood's)
	float Level () const;

	/// The output's RMS (each channel) for a mood at its level, about
	static constexpr float RMS = 0.032f; // (-30 dBFS)

private:
	struct Impl;
	std::unique_ptr<Impl> p;
};

} // namespace music

/// The composer as a source on the music bus (stereo: the engine's channels)
class MusicSynth : public SynthSource {
public:
	static SourcePtr<MusicSynth> Make (Engine &e, uint32_t seed);
	~MusicSynth () override;
	void SetMood (music::Mood m) { mood.store ((int)m, std::memory_order_relaxed); }
	void SetIntensity (float x) { intensity.store (x, std::memory_order_relaxed); }

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	MusicSynth () = default;
	std::atomic<int> mood{music::SILENT};
	std::atomic<float> intensity{0.0f};
	std::unique_ptr<music::Composer> comp;
	std::vector<float> l, r;
};

} // namespace audio

#endif // !__MUSICSYNTH_H

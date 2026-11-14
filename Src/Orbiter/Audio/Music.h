// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The music: generative (MusicSynth), on the music bus, following the
// situation of the vessel in focus - not a sound of the world (never
// through the air or the hull), the player's only.
//
//   menu       the launchpad (and the scene behind it)
//   ground     on the ground, or flying in a planet's air
//   ascent     a powered climb from liftoff to orbit (its progress: the
//              pulse grows, the pads brighten)
//   reentry    falling into the air fast (or in the radio's blackout): a
//              low drone
//   approach   within 3 km of another vessel or the docking target, not
//              docked (closer: a stronger pulse)
//   deep       far from any body: about the Sun, or beyond 30 radii of
//              the body whose gravity holds the vessel
//   orbit      anywhere else in space
//
// A new situation must hold 4 s before the music follows (staging, a
// bounce). A critical moment - the focus vessel breaking up, losing a
// part, burning - silences it (it fades in a second or so) for 45 s, or
// until another vessel is in focus.
//
// On with the settings' "Music" (SoundMusicOn); renders are without it
// unless ORBITER_AUDIO_MUSIC=1 (ORBITER_AUDIO_MUSIC=0: off anyway).
// =======================================================================

#ifndef __AUDIOMUSIC_H
#define __AUDIOMUSIC_H

#include "MusicSynth.h"
#include "World.h"
#include <memory>

namespace audio {

class System;

class Music {
public:
	explicit Music (System &s);
	~Music ();

	/// The flight as the music sees it
	struct Situation {
		bool menu = false;      ///< the launchpad
		bool vessel = false;    ///< a vessel in focus (none: orbit)
		bool destroyed = false;
		bool landed = false;    ///< on the ground, or held on a pad
		bool air = false;       ///< in a planet's air (above some 10 Pa)
		bool ascent = false;    ///< climbing to orbit under power
		bool reentry = false;   ///< falling into the air fast
		bool docked = false;
		double target = -1.0;   ///< to the nearest vessel or docking target [m] (< 0: none)
		bool deep = false;      ///< far from any body
		double progress = 0.0;  ///< the ascent's, the approach's closeness (0..1)
	};
	static music::Mood MoodFor (const Situation &s);

	/// Each frame: dt [s] of real time; menu: the launchpad is showing
	void Update (double dt, bool menu);
	/// A critical moment: silence for a while
	void Alarm ();
	void Clear ();
	music::Mood Current () const { return mood; }

	static constexpr double HOLD = 4.0;       ///< [s] a new situation, before the music follows
	static constexpr double ALARM = 45.0;     ///< [s] the silence after a critical moment
	static constexpr double APPROACH_DIST = 3000.0; ///< [m]
	static constexpr double DEEP_RADII = 30.0; ///< [radii of the gravity's body]
	static constexpr double REENTRY_V = 2000.0; ///< [m/s]

private:
	Situation Observe (bool menu) const;
	System &sys;
	SourcePtr<MusicSynth> synth;
	music::Mood mood = music::SILENT, candidate = music::SILENT;
	double since = 0.0, alarm = 0.0, off = 0.0;
	BodyRef focus;               // (another vessel in focus: the critical moment past)
};

} // namespace audio

#endif // !__AUDIOMUSIC_H

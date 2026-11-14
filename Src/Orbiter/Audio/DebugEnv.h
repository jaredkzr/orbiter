// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The sound's options from the environment - tests, renders, development
// (LINUX.md lists them) - read once, in one place. Elsewhere: Dbg ().
// "Tristate": -1 not set (the settings decide), 0 off, 1 on.
// =======================================================================

#ifndef __AUDIODEBUGENV_H
#define __AUDIODEBUGENV_H

#include <string>

namespace audio {

struct DebugEnv {
	std::string render;        ///< ORBITER_AUDIO_RENDER=<file.wav>: mixed offline into it
	int device = -1;           ///< ORBITER_AUDIO=0|1: no sound / the device even headless (tristate)
	bool offscreen = false;    ///< ORBITER_VK_OFFSCREEN (not 0): off the speakers
	int adapt = -1;            ///< ORBITER_AUDIO_ADAPT=0|1: the listening level (tristate)
	bool async = false;        ///< ORBITER_AUDIO_ASYNC=1: files decoded in the background in a render too
	int log = 0;               ///< ORBITER_AUDIO_LOG (set: 1; 2: silent sources too)
	int callouts = -1;         ///< ORBITER_AUDIO_CALLOUTS=0|1 (tristate)
	int music = -1;            ///< ORBITER_AUDIO_MUSIC=0|1 (tristate)
	int hrtf = -1;             ///< ORBITER_AUDIO_HRTF=0|1 (tristate)
	int realism = -1;          ///< ORBITER_AUDIO_REALISM=physical|cinematic (0, 1; -1 not set)
	int voice = -1;            ///< ORBITER_AUDIO_VOICE=orbiter|xrsound (1, 0; -1 not set)
	int capcom = 0;            ///< ORBITER_AUDIO_CAPCOM=1|2 (0: they take turns)
	int voices = 0;            ///< ORBITER_AUDIO_VOICES=<n>: the voice cap (0: World's)
	bool vessels = true;       ///< ORBITER_AUDIO_VESSELS=0: the vessels' own sounds off
	bool radio = true;         ///< ORBITER_AUDIO_RADIO=0: no radio
	bool ambience = true;      ///< ORBITER_AUDIO_AMBIENCE=0: no ambience
	bool contacts = false;     ///< ORBITER_AUDIO_CONTACTS=1: the contacts' sounds on
	bool renderVolumes = false;///< ORBITER_AUDIO_RENDER_VOLUMES=1: a render at the settings' volumes
	std::string solo;          ///< ORBITER_AUDIO_SOLO=<buses>: a render's buses heard (empty: all)
	std::string testTone;      ///< ORBITER_AUDIO_TEST_TONE=<vessel>[:<Hz>[:<level>]]
	std::string flightTrace;   ///< ORBITER_AUDIO_FLIGHT_TRACE=<file>
	bool radioCodec = true;    ///< ORBITER_RADIO_CODEC=0: the deep-space codec off (listening tests)
	std::string contactSamples;///< ORBITER_CONTACT_SAMPLES=<dir>

	/// The process's (read at the first call)
	static const DebugEnv &Get ();
	/// Read from an environment (tests: a lookup of their own)
	static DebugEnv Read (const char *(*get) (const char *));
};

inline const DebugEnv &Dbg () { return DebugEnv::Get (); }

} // namespace audio

#endif // !__AUDIODEBUGENV_H

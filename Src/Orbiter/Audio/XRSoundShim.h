// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// XRSound on Orbiter's own sound: add-ons written for XRSound (XRSound.h,
// XRSound::CreateInstance) and Lua's xrsound scripts play through the
// core's engine, with no XRSound module. An adapter on the public sound API
// (oapiAudio*): what it plays, any add-on could.
//
//  - a vessel's sounds (LoadWav/PlayWav) by their playback type:
//      BothViewFar/Medium/Close: on the vessel, heard as its other sounds
//        (through the air, delayed; through its hull in the cockpit;
//        silent across vacuum)
//      InternalOnly: the cockpit bus, heard in the cockpit of the vessel in
//        focus only
//      Radio: the voice bus, the vessel in focus only (inside and out)
//      Wind: outside, as loud as the air is thick
//      Global: anywhere (the interface bus)
//    a module's: Global
//  - levels: XRSound's volume 1 (a file at full scale) is a pressure on
//    the physical buses and on vessels - 1 Pa (94 dB) in the cockpit and
//    for the wind; on the vessel, at 2 m, Close 4 Pa, Medium 8, Far 16; a
//    gain on the voice's and the interface's buses
//  - the default sounds: what Orbiter synthesizes (engines, RCS, the air,
//    contacts, booms) stays synthesized; switching one off
//    (SetDefaultSoundEnabled: oapiAudioDefaultSound) silences that
//    synthesizer on the vessel, its tune kept. An engine given a file of
//    its own (LoadWav with MainEngines, RetroEngines, HoverEngines, or a
//    class cfg naming one) plays the file, as loud as the engines'
//    throttle, in place of the synthesizer.
//  - the voice: the callouts (Callouts.h) take a vessel's own lines and
//    folders from here over their table (Sound/Voice/callouts.cfg); XRSound's
//    default lines are spoken in Orbiter's voice (Voice: "Callout voice")
//  - the sounds of animations a vessel class's XRSound cfg names
//    ([animation_<n>]: opening, moving, open, closing, closed)
// =======================================================================

#ifndef __XRSOUNDSHIM_H
#define __XRSOUNDSHIM_H

#include <map>
#include <string>
#include <vector>

class Vessel;
class Body;

namespace audio {

class System;

namespace xr {
	/// Each time step (System::Step): where the sounds may be heard, the
	/// engines' files, the animations' sounds
	void Step (System &sys, const Vessel *focus, bool cockpit, double simt);
	/// A session ends (a body deleted: World::Forget - every sound of it
	/// knows it's gone)
	void Clear ();

	/// A vessel's own file for a default sound or group (by XRSound's
	/// cfg key: "Touchdown", "AltitudeCalloutsGroup"...): set: true, file
	/// empty if switched off
	bool LineFor (const Vessel *v, const std::string &key, std::string &file);
	/// A vessel's code uses XRSound (it plays its own switches' clicks)
	bool HasInstance (const Vessel *v);
	/// The file that plays for a line XRSound names: one of XRSound's default
	/// lines (XRSound/Default/<name>.wav) in Orbiter's voice (Sound/Voice/
	/// <name>.flac; "Callout voice": Orbiter, ORBITER_AUDIO_VOICE), else as named
	std::string Voice (const std::string &file);
}

} // namespace audio

#endif // !__XRSOUNDSHIM_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// XRSound on Orbiter's own sound: add-ons written for XRSound (XRSound.h,
// XRSound::CreateInstance) and Lua's xrsound scripts play through the
// core's engine, with no XRSound module.
//
//  - a vessel's sounds (LoadWav/PlayWav) by their playback type:
//      BothViewFar/Medium/Close: on the vessel, heard as its other sounds
//        (through the air, delayed; through its hull in the cockpit;
//        silent across vacuum), a little louder Far than Close
//      InternalOnly: the cockpit of the vessel in focus (the voice bus)
//      Radio: the vessel in focus, inside and out (the voice bus)
//      Wind: outside, as loud as the air is thick
//      Global: anywhere (the interface bus)
//    a module's: Global
//  - the default sounds: what Orbiter synthesizes (engines, RCS, the air,
//    contacts, booms) stays synthesized; switching one off
//    (SetDefaultSoundEnabled) silences that synthesizer on the vessel. An
//    engine given a file of its own (LoadWav with MainEngines,
//    RetroEngines, HoverEngines, or a class cfg naming one) plays the file,
//    as loud as the engines' throttle, in place of the synthesizer. The
//    callouts (Callouts.h) take a vessel's own lines and folders from here.
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
	/// A body is deleted, a session ends
	void Forget (const Body *body);
	void Clear ();

	/// A vessel's own file for a default sound or group (by XRSound's
	/// cfg key: "Touchdown", "AltitudeCalloutsGroup"...): set: true, file
	/// empty if switched off
	bool LineFor (const Vessel *v, const std::string &key, std::string &file);
	/// A vessel's code uses XRSound (it plays its own switches' clicks)
	bool HasInstance (const Vessel *v);
}

} // namespace audio

#endif // !__XRSOUNDSHIM_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// XRSound's configuration files, read as XRSound reads them:
// XRSound/XRSound.cfg, and over it a vessel class's
// XRSound/XRSound-<class>.cfg (its own callouts, the sounds of its
// animations). Section by section: "key = value" lines, '#' comments;
// keys in any case. A file "none" (or empty) switches a sound off.
// =======================================================================

#ifndef __XRCONFIG_H
#define __XRCONFIG_H

#include <map>
#include <string>
#include <vector>

namespace audio {

struct XrConfig {
	/// The sounds of an animation ([animation_<n>])
	struct Anim {
		int index = -1;
		int openCloseID = -1, movingID = -1;
		std::string opening, open, moving, closing, closed;
		std::string playback;   // PlaybackType (BothViewClose...: empty, the default)
		bool gear = false;      // IsLandingGear
	};

	std::map<std::string, std::string> system, files, groups, atc; // (keys in lower case)
	std::vector<Anim> anims;

	/// The configuration for a vessel class (empty: the global file alone),
	/// read once
	static const XrConfig &For (const std::string &cls);
	/// Forget what was read (a new session: the files may have changed)
	static void Reload ();

	/// The file for a key of [SOUND_FILES] (empty: none, or "none")
	std::string File (const std::string &key) const;
	/// A sound group's folder ([SOUND_GROUPS]; empty: none)
	std::string Group (const std::string &key) const;
	/// A number in [SOUND_FILES] or [SYSTEM] (def: not there)
	double Number (const std::string &key, double def) const;
	/// The landing gear's animation (-1: none named)
	int GearAnim () const;

	/// Read a file over what is there (false: no such file)
	bool Read (const std::string &path);
};

} // namespace audio

#endif // !__XRCONFIG_H

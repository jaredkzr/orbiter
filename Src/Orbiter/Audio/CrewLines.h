// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The crew's lines, from data (Sound/Voice/callouts.cfg): each line's
// file, priority, how long it may wait for its turn, its gain - by key.
// When a line is said is the code's (FlightState, Callouts); what is said
// is the table's. XRSound's cfgs and a vessel's code may give a line a file
// of their own (the XRSound layer, over this one: Callouts::Line).
// =======================================================================

#ifndef __AUDIOCREWLINES_H
#define __AUDIOCREWLINES_H

#include <map>
#include <string>

namespace audio {

struct CrewLine {
	std::string key, file;   ///< (file: "{n}" - one of the numbered variants)
	bool warning = false;    ///< said first, its cue before it
	double wait = 2.0;       ///< [s] dropped if not said by then
	double gain = 1.0;
};

class CrewLines {
public:
	/// Read a table (false: no such file); a line per key, keys in any case
	bool Read (const std::string &path);
	/// A key's line (null: none)
	const CrewLine *Find (const std::string &key) const;
	size_t Size () const { return lines.size (); }
	/// The table the game uses (Sound/Voice/callouts.cfg, read once)
	static const CrewLines &Game ();

private:
	std::map<std::string, CrewLine> lines; // (by key, lower case)
};

} // namespace audio

#endif // !__AUDIOCREWLINES_H

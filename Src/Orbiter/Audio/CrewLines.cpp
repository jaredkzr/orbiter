// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "CrewLines.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace audio {

namespace {
	std::string Trim (const std::string &x)
	{
		const size_t a = x.find_first_not_of (" \t\r"), b = x.find_last_not_of (" \t\r");
		return a == std::string::npos ? std::string () : x.substr (a, b - a + 1);
	}
	std::string Lower (std::string s)
	{
		for (auto &c : s) c = (char)std::tolower ((unsigned char)c);
		return s;
	}
}

bool CrewLines::Read (const std::string &path)
{
	std::ifstream in (path);
	if (!in) return false;
	std::string line;
	int n = 0;
	while (std::getline (in, line)) {
		n++;
		const size_t hash = line.find ('#');
		if (hash != std::string::npos) line.resize (hash);
		if (Trim (line).empty ()) continue;
		std::vector<std::string> f;
		size_t at = 0;
		for (;;) {
			const size_t bar = line.find ('|', at);
			f.push_back (Trim (line.substr (at, bar == std::string::npos ? std::string::npos : bar - at)));
			if (bar == std::string::npos) break;
			at = bar + 1;
		}
		if (f.size () < 4 || f[0].empty ()) { fprintf (stderr, "Audio: %s:%d: <key> | <file> | <priority> | <wait> [| <gain> [| <cooldown>]]\n", path.c_str (), n); continue; }
		CrewLine c;
		c.key = f[0], c.file = f[1];
		c.warning = Lower (f[2]) == "warning";
		c.wait = atof (f[3].c_str ());
		if (f.size () > 4 && !f[4].empty ()) c.gain = atof (f[4].c_str ());
		if (f.size () > 5 && !f[5].empty ()) c.cooldown = atof (f[5].c_str ());
		lines[Lower (c.key)] = c;
	}
	return true;
}

const CrewLine *CrewLines::Find (const std::string &key) const
{
	auto it = lines.find (Lower (key));
	return it == lines.end () ? nullptr : &it->second;
}

const CrewLines &CrewLines::Game ()
{
	static CrewLines t;
	static bool read = false;
	if (!read) {
		read = true;
		if (!t.Read ("Sound/Voice/callouts.cfg")) fprintf (stderr, "Audio: no Sound/Voice/callouts.cfg: the crew is silent\n");
	}
	return t;
}

} // namespace audio

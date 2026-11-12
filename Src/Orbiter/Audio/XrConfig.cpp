// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "XrConfig.h"
#include "AudioSystem.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>

namespace audio {

namespace {
	std::string Lower (std::string s)
	{
		for (auto &c : s) c = (char)std::tolower ((unsigned char)c);
		return s;
	}
	std::string Trim (const std::string &s)
	{
		size_t a = s.find_first_not_of (" \t\r\n"), b = s.find_last_not_of (" \t\r\n");
		return a == std::string::npos ? std::string () : s.substr (a, b - a + 1);
	}
	std::map<std::string, std::unique_ptr<XrConfig>> &Cache ()
	{
		static std::map<std::string, std::unique_ptr<XrConfig>> c;
		return c;
	}
}

bool XrConfig::Read (const std::string &file)
{
	std::string path = System::FindSound (file);
	if (path.empty ()) return false;
	std::ifstream in (path);
	if (!in) return false;
	std::string line, section;
	Anim *anim = nullptr;
	while (std::getline (in, line)) {
		size_t h = line.find ('#');
		if (h != std::string::npos) line.resize (h);
		line = Trim (line);
		if (line.empty ()) continue;
		if (line.front () == '[') {
			size_t e = line.find (']');
			section = Lower (Trim (line.substr (1, e == std::string::npos ? std::string::npos : e - 1)));
			anim = nullptr;
			if (section.compare (0, 10, "animation_") == 0) {
				int idx = atoi (section.c_str () + 10);
				auto it = std::find_if (anims.begin (), anims.end (), [idx](const Anim &a) { return a.index == idx; });
				if (it == anims.end ()) { anims.push_back (Anim ()); anims.back ().index = idx; anim = &anims.back (); }
				else anim = &*it;
			}
			continue;
		}
		size_t eq = line.find ('=');
		if (eq == std::string::npos) continue;
		std::string key = Lower (Trim (line.substr (0, eq))), value = Trim (line.substr (eq + 1));
		if (anim) {
			if (key == "openclosesoundid") anim->openCloseID = atoi (value.c_str ());
			else if (key == "movingsoundid") anim->movingID = atoi (value.c_str ());
			else if (key == "opening") anim->opening = value;
			else if (key == "open") anim->open = value;
			else if (key == "moving") anim->moving = value;
			else if (key == "closing") anim->closing = value;
			else if (key == "closed") anim->closed = value;
			else if (key == "playbacktype") anim->playback = value;
			else if (key == "islandinggear") anim->gear = atoi (value.c_str ()) != 0;
		}
		else if (section == "system") system[key] = value;
		else if (section == "sound_files") files[key] = value;
		else if (section == "sound_groups") groups[key] = value;
		else if (section == "atc") atc[key] = value;
	}
	return true;
}

const XrConfig &XrConfig::For (const std::string &cls)
{
	auto &cache = Cache ();
	auto it = cache.find (cls);
	if (it != cache.end ()) return *it->second;
	auto c = std::make_unique<XrConfig>();
	c->Read ("XRSound/XRSound.cfg");
	if (!cls.empty ()) c->Read ("XRSound/XRSound-" + cls + ".cfg");
	return *(cache[cls] = std::move (c));
}

void XrConfig::Reload ()
{
	Cache ().clear ();
}

std::string XrConfig::File (const std::string &key) const
{
	auto it = files.find (Lower (key));
	if (it == files.end ()) return std::string ();
	std::string v = it->second;
	if (v.empty () || Lower (v) == "none") return std::string ();
	return v;
}

std::string XrConfig::Group (const std::string &key) const
{
	auto it = groups.find (Lower (key));
	if (it == groups.end () || it->second.empty () || Lower (it->second) == "none") return std::string ();
	return it->second;
}

double XrConfig::Number (const std::string &key, double def) const
{
	std::string k = Lower (key);
	const std::string *s = nullptr;
	auto it = files.find (k);
	if (it != files.end ()) s = &it->second;
	else if ((it = system.find (k)) != system.end ()) s = &it->second;
	if (!s || s->empty ()) return def;
	char *end = nullptr;
	double v = strtod (s->c_str (), &end);
	return end == s->c_str () ? def : v;
}

int XrConfig::GearAnim () const
{
	for (auto &a : anims) if (a.gear) return a.index;
	return -1;
}

} // namespace audio

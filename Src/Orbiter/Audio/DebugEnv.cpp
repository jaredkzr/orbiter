// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "DebugEnv.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace audio {

DebugEnv DebugEnv::Read (const char *(*get) (const char *))
{
	DebugEnv d;
	auto str = [get] (const char *k) { const char *v = get (k); return std::string (v ? v : ""); };
	auto set = [get] (const char *k) { const char *v = get (k); return v && *v; };
	auto first = [get] (const char *k) { const char *v = get (k); return v && *v ? *v : '\0'; };
	auto tri = [&] (const char *k) { return set (k) ? (first (k) != '0' ? 1 : 0) : -1; };
	d.render = str ("ORBITER_AUDIO_RENDER");
	d.device = tri ("ORBITER_AUDIO");
	d.offscreen = set ("ORBITER_VK_OFFSCREEN") && first ("ORBITER_VK_OFFSCREEN") != '0';
	d.adapt = tri ("ORBITER_AUDIO_ADAPT");
	d.async = first ("ORBITER_AUDIO_ASYNC") == '1';
	if (const char *v = get ("ORBITER_AUDIO_LOG")) {   // (set at all: on)
		d.log = atoi (v) >= 2 ? 2 : 1;
		d.logStates = std::tolower ((unsigned char)*v) != 'e';   // ("events": the moments, not every source's state)
	}
	d.callouts = tri ("ORBITER_AUDIO_CALLOUTS");
	d.music = tri ("ORBITER_AUDIO_MUSIC");
	d.hrtf = tri ("ORBITER_AUDIO_HRTF");
	if (set ("ORBITER_AUDIO_REALISM")) d.realism = std::tolower ((unsigned char)first ("ORBITER_AUDIO_REALISM")) == 'c' ? 1 : 0;
	if (set ("ORBITER_AUDIO_VOICE")) d.voice = std::tolower ((unsigned char)first ("ORBITER_AUDIO_VOICE")) != 'x' ? 1 : 0;
	const char c = first ("ORBITER_AUDIO_CAPCOM");
	d.capcom = c == '1' ? 1 : c == '2' ? 2 : 0;
	if (const char *v = get ("ORBITER_AUDIO_VOICES")) d.voices = std::max (1, atoi (v));
	d.vessels = first ("ORBITER_AUDIO_VESSELS") != '0';
	d.radio = first ("ORBITER_AUDIO_RADIO") != '0';
	d.ambience = first ("ORBITER_AUDIO_AMBIENCE") != '0';
	d.contacts = first ("ORBITER_AUDIO_CONTACTS") != '0';
	d.renderVolumes = first ("ORBITER_AUDIO_RENDER_VOLUMES") == '1';
	d.solo = str ("ORBITER_AUDIO_SOLO");
	d.testTone = str ("ORBITER_AUDIO_TEST_TONE");
	d.flightTrace = str ("ORBITER_AUDIO_FLIGHT_TRACE");
	d.radioCodec = first ("ORBITER_RADIO_CODEC") != '0';
	if (const char *v = get ("ORBITER_XR_PA")) {
		double a = 0.0, b = 0.0;
		if (sscanf (v, "%lf,%lf", &a, &b) == 2 && a > 0.0 && b > 0.0) d.xrCockpitPa = a, d.xrClosePa = b;
	}
	return d;
}

const DebugEnv &DebugEnv::Get ()
{
	static const DebugEnv d = Read ([] (const char *k) -> const char * { return getenv (k); });
	return d;
}

} // namespace audio

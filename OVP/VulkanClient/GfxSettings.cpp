// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "GfxSettings.h"
#include "OrbiterAPI.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace vkc {

namespace {
	const char *GRAPHICS_CFG = "Config/VK/Graphics.cfg";
	const char *SKY_CFG = "Config/VK/Sky.cfg";
	const float X = NAN; // (not part of the presets)

	bool Same (const std::string &a, const char *b) // (ignoring case)
	{
		size_t i = 0;
		for (; i < a.size () && b[i]; i++)
			if (tolower ((unsigned char)a[i]) != tolower ((unsigned char)b[i])) return false;
		return i == a.size () && !b[i];
	}

	std::string Trim (const std::string &s)
	{
		size_t a = s.find_first_not_of (" \t\r\n"), b = s.find_last_not_of (" \t\r\n");
		return a == std::string::npos ? std::string () : s.substr (a, b - a + 1);
	}

	/// key = value lines of a file (';' comments)
	std::vector<std::pair<std::string, std::string>> ReadKeys (const char *path)
	{
		std::vector<std::pair<std::string, std::string>> kv;
		std::ifstream ifs (path);
		std::string line;
		while (std::getline (ifs, line)) {
			line = line.substr (0, line.find (';'));
			size_t eq = line.find ('=');
			if (eq == std::string::npos) continue;
			std::string k = Trim (line.substr (0, eq)), v = Trim (line.substr (eq + 1));
			if (!k.empty () && !v.empty ()) kv.push_back ({ k, v });
		}
		return kv;
	}

	const char *AA_CHOICES[] = { "Off", "Multisampling (4\xC3\x97)", "Temporal (TAA)", "AMD FSR 3" };
	const char *SKY_CHOICES[] = { "Realistic", "Enhanced" };
	const char *SMOKE_CHOICES[] = { "Sprites", "Screen-space", "Volumetric" };
}

// One setting: where it is kept, how it is shown
struct GfxSettings::Item {
	const char *key;
	const char *file;   // GRAPHICS_CFG or SKY_CFG
	const char *env;    // environment variable overriding it (NULL: none)
	const char *group, *label, *help;
	GraphicsSetting::Type type;
	GraphicsSetting::Apply apply;
	bool GfxSettings::*b = nullptr;
	float GfxSettings::*f = nullptr;
	int GfxSettings::*i = nullptr;
	float preset[4] = { X, X, X, X };
	const char *requires = nullptr;
	bool rt = false; // needs ray queries
	int nchoice = 0;
	const char *const *choice = nullptr;
	float vmin = 0, vmax = 1, step = 0, display = 1;
	const char *format = nullptr;
};

const std::vector<GfxSettings::Item> &GfxSettings::Items ()
{
	using T = GraphicsSetting;
	static std::vector<Item> items;
	if (items.size ()) return items;
	auto toggle = [](const char *key, const char *env, const char *group, const char *label, const char *help,
		T::Apply apply, bool GfxSettings::*b, std::initializer_list<float> preset = {}) {
		Item it{ key, GRAPHICS_CFG, env, group, label, help, T::TOGGLE, apply };
		it.b = b;
		int n = 0;
		for (float p : preset) it.preset[n++] = p;
		return it;
	};
	auto slider = [](const char *key, const char *env, const char *group, const char *label, const char *help,
		float GfxSettings::*f, float vmin, float vmax, float step, float display, const char *format) {
		Item it{ key, GRAPHICS_CFG, env, group, label, help, T::SLIDER, T::LIVE };
		it.f = f, it.vmin = vmin, it.vmax = vmax, it.step = step, it.display = display, it.format = format;
		return it;
	};
	const char *G1 = "Anti-aliasing and resolution", *G2 = "Lighting", *G3 = "Effects", *G4 = "Sky";

	Item aa{ "AntiAliasing", GRAPHICS_CFG, "ORBITER_VK_TAA", G1, "Anti-aliasing",
		"Smooths jagged edges. FSR 3 also upscales the scene from a lower render resolution.", T::CHOICE, T::NEXT_SESSION };
	aa.i = &GfxSettings::antiAliasing, aa.nchoice = 4, aa.choice = AA_CHOICES;
	items.push_back (aa);
	items.push_back (slider ("RenderScale", "ORBITER_VK_RENDER_SCALE", G1, "Render resolution",
		"The 3D scene is rendered at this share of the window's size and upscaled. Lower is faster.",
		&GfxSettings::renderScale, 0.5f, 1.0f, 0.01f, 100.0f, "%.0f %%"));
	items.push_back (slider ("Sharpness", "ORBITER_VK_FSR_SHARPNESS", G1, "Sharpening",
		"Sharpening after FSR 3's upscaling.", &GfxSettings::sharpness, 0.0f, 1.0f, 0.01f, 100.0f, "%.0f %%"));
	items.back ().requires = "AntiAliasing=3";

	items.push_back (toggle ("RayTracing", "ORBITER_VK_RT_REFLECT", G2, "Ray tracing",
		"Hardware ray tracing for the reflections and the lighting below.", T::NEXT_SESSION, &GfxSettings::rayTracing, {0, 0, 1, 1}));
	items.back ().rt = true;
	items.push_back (toggle ("RTReflections", nullptr, G2, "Ray traced reflections",
		"Reflections of objects outside the view, on shiny surfaces (with screen-space reflections).",
		T::NEXT_SESSION, &GfxSettings::rtReflections, {0, 0, 1, 1}));
	items.back ().rt = true, items.back ().requires = "RayTracing";
	items.push_back (toggle ("RTLighting", "ORBITER_VK_RT_LIGHTING", G2, "Ray traced shadows and lighting",
		"Shadows, ambient occlusion and the light of nearby planets traced in the scene (instead of shadow maps).",
		T::NEXT_SESSION, &GfxSettings::rtLighting, {0, 0, 0, 1}));
	items.back ().rt = true, items.back ().requires = "RayTracing";
	items.push_back (toggle ("AmbientOcclusion", "ORBITER_VK_AO", G2, "Ambient occlusion",
		"Soft contact shadows in creases and corners (screen-space; ray traced lighting replaces it).",
		T::LIVE, &GfxSettings::ao, {0, 1, 1, 1}));
	items.push_back (toggle ("Reflections", "ORBITER_VK_SSR", G2, "Screen-space reflections",
		"Reflections of what is on the screen in water and on shiny surfaces.", T::LIVE, &GfxSettings::ssr, {0, 1, 1, 1}));
	items.push_back (toggle ("EngineLight", "ORBITER_VK_ENGINE_LIGHT", G2, "Engine light",
		"Engines light up their vessel and the ground.", T::LIVE, &GfxSettings::engineLight, {0, 1, 1, 1}));
	items.push_back (toggle ("AutoExposure", "ORBITER_VK_AUTOEXPOSURE", G2, "Automatic exposure",
		"The exposure adapts to the scene's brightness, as an eye or a camera does.", T::LIVE, &GfxSettings::autoExposure));
	items.push_back (slider ("Bloom", "ORBITER_VK_BLOOM", G2, "Bloom",
		"The glow around bright lights.", &GfxSettings::bloom, 0.0f, 1.0f, 0.01f, 100.0f, "%.0f %%"));

	items.push_back (toggle ("Clouds", "ORBITER_VK_CLOUDS", G3, "Volumetric clouds",
		"Clouds with depth that can be flown through.", T::LIVE, &GfxSettings::clouds, {0, 1, 1, 1}));
	items.push_back (toggle ("Plumes", "ORBITER_VK_PLUMES", G3, "Volumetric exhaust plumes",
		"Rocket exhaust drawn as glowing gas, shaped by the air pressure.", T::LIVE, &GfxSettings::plumes, {0, 1, 1, 1}));
	items.push_back (toggle ("HeatShimmer", "ORBITER_VK_SHIMMER", G3, "Heat shimmer",
		"The view behind a hot exhaust ripples.", T::LIVE, &GfxSettings::shimmer, {0, 0, 1, 1}));
	items.back ().requires = "Plumes";
	items.push_back (toggle ("Particles", "ORBITER_VK_PARTICLES", G3, "Particle effects",
		"Smoke, exhaust and dust.", T::LIVE, &GfxSettings::particles, {1, 1, 1, 1}));
	items.push_back (slider ("ParticleDensity", "ORBITER_VK_PARTICLE_DENSITY", G3, "Particle density",
		"More, smaller particles: smoother smoke (slower).", &GfxSettings::particleDensity, 1.0f, 8.0f, 1.0f, 1.0f, "%.0f\xC3\x97"));
	{ float p[4] = { 1, 2, 3, 4 }; std::copy (p, p + 4, items.back ().preset); }
	items.back ().requires = "Particles";
	Item sm{ "SmokeRendering", GRAPHICS_CFG, "ORBITER_VK_SMOKE", G3, "Smoke",
		"Volumetric: smoke clouds with depth, lit and shadowed. Screen-space: cheaper, flatter. Sprites: cheapest.",
		T::CHOICE, T::LIVE };
	sm.i = &GfxSettings::smokeRendering, sm.nchoice = 3, sm.choice = SMOKE_CHOICES, sm.requires = "Particles";
	{ float p[4] = { 0, 1, 2, 2 }; std::copy (p, p + 4, sm.preset); }
	items.push_back (sm);
	items.push_back (toggle ("SmokeTrails", "ORBITER_VK_SMOKE_TRAILS", G3, "Launch smoke trails",
		"Rocket smoke that stays in the sky for minutes, carried by the wind.", T::LIVE, &GfxSettings::smokeTrails, {0, 1, 1, 1}));
	items.back ().requires = "Particles";
	items.push_back (slider ("CameraShake", "ORBITER_VK_SHAKE", G3, "Camera shake",
		"The cockpit view jolts in turbulence and buffeting; any view jolts with the thud of an explosion near it.", &GfxSettings::cameraShake, 0.0f, 2.0f, 0.05f, 100.0f, "%.0f %%"));

	Item sky{ "DisplayMode", SKY_CFG, "ORBITER_VK_SKY_MODE", G4, "Sky display",
		"Realistic: as the dark-adapted eye sees it. Enhanced: as a long-exposure photograph (deeper colours, more contrast).",
		T::CHOICE, T::NEXT_SESSION };
	sky.nchoice = 2, sky.choice = SKY_CHOICES;
	items.push_back (sky);
	Item it = toggle ("Nebulae", "ORBITER_VK_SKY_NEBULAE", G4, "Nebulae", "Glowing gas clouds of the Milky Way.",
		T::NEXT_SESSION, &GfxSettings::nebulae, {0, 1, 1, 1});
	it.file = SKY_CFG;
	items.push_back (it);
	it = toggle ("Galaxies", "ORBITER_VK_SKY_OBJECTS", G4, "Galaxies and star clusters", "Other galaxies and the globular clusters.",
		T::NEXT_SESSION, &GfxSettings::galaxies, {0, 1, 1, 1});
	it.file = SKY_CFG;
	items.push_back (it);
	it = toggle ("ZodiacalLight", "ORBITER_VK_ZODIACAL", G4, "Zodiacal light", "Sunlight scattered by the dust between the planets.",
		T::NEXT_SESSION, &GfxSettings::zodiacal, {0, 1, 1, 1});
	it.file = SKY_CFG;
	items.push_back (it);
	return items;
}

std::vector<float> GfxSettings::Values () const
{
	std::vector<float> v;
	for (auto &it : Items ()) {
		if (it.b) v.push_back (this->*it.b ? 1.0f : 0.0f);
		else if (it.f) v.push_back (this->*it.f);
		else if (it.i) v.push_back ((float)(this->*it.i));
		else v.push_back (skyEnhanced ? 1.0f : 0.0f); // (DisplayMode)
	}
	return v;
}

namespace {
	// the environment's anti-aliasing (as before the settings): -1 where not set
	int EnvTaa () { const char *e = getenv ("ORBITER_VK_TAA"); return e ? (atoi (e) != 0) : -1; }
	int EnvMsaa () { const char *e = getenv ("ORBITER_VK_MSAA"); return e ? std::max (1, atoi (e)) : -1; }
	const char *EnvUpscaler () { return getenv ("ORBITER_VK_UPSCALER"); }
}

void GfxSettings::Load ()
{
	auto setValue = [this](const Item &it, float v, bool clamp = true) {
		if (it.b) this->*it.b = v != 0.0f;
		else if (it.f) this->*it.f = clamp ? std::clamp (v, it.vmin, it.vmax) : v; // (the environment's as given)
		else if (it.i) this->*it.i = std::clamp ((int)std::lround (v), 0, it.nchoice - 1);
		else skyEnhanced = v != 0.0f;
	};
	auto parse = [](const Item &it, const std::string &s) -> float {
		if (it.type == GraphicsSetting::TOGGLE)
			return (Same (s, "true") || Same (s, "on") || atoi (s.c_str ()) != 0) ? 1.0f : 0.0f;
		if (it.type == GraphicsSetting::CHOICE) {
			for (int c = 0; c < it.nchoice; c++)
				if (Same (s, it.choice[c])) return (float)c;
			if (!strcmp (it.key, "AntiAliasing")) { // (short names)
				static const char *names[] = { "off", "msaa", "taa", "fsr" };
				for (int c = 0; c < 4; c++) if (Same (s, names[c])) return (float)c;
			}
			if (!strcmp (it.key, "SmokeRendering")) { // (ORBITER_VK_SMOKE's words)
				static const char *names[] = { "sprites", "screen", "volume" };
				for (int c = 0; c < 3; c++) if (Same (s, names[c])) return (float)c;
			}
			return (float)atoi (s.c_str ());
		}
		return (float)atof (s.c_str ());
	};
	const auto &items = Items ();
	const char *off = getenv ("ORBITER_VK_OFFSCREEN");
	if (const char *e = getenv ("ORBITER_VK_GRAPHICS_CFG")) gfxPath = skyPath = e;
	else if (!(off && *off && *off != '0')) gfxPath = GRAPHICS_CFG, skyPath = SKY_CFG;
	else oapiWriteLog ((char*)"VulkanClient: offscreen: the saved graphics settings are not used (ORBITER_VK_GRAPHICS_CFG)");
	for (auto &kv : ReadKeys (gfxPath.c_str ()))
		for (auto &it : items)
			if (it.file == GRAPHICS_CFG && Same (kv.first, it.key)) setValue (it, parse (it, kv.second));
	for (auto &kv : ReadKeys (skyPath.c_str ()))
		for (auto &it : items)
			if (it.file == SKY_CFG && Same (kv.first, it.key)) setValue (it, parse (it, kv.second));

	// the environment
	locked.assign (items.size (), std::string ());
	for (size_t n = 0; n < items.size (); n++) {
		const Item &it = items[n];
		if (!strcmp (it.key, "AntiAliasing")) {
			if (EnvTaa () < 0 && EnvMsaa () < 0 && !EnvUpscaler ()) continue;
			// (as before the settings: temporal anti-aliasing unless ORBITER_VK_TAA=0,
			// then 4x multisampling unless ORBITER_VK_MSAA; FSR 3 unless ORBITER_VK_UPSCALER=taa)
			locked[n] = EnvTaa () >= 0 ? "ORBITER_VK_TAA" : EnvMsaa () > 0 ? "ORBITER_VK_MSAA" : "ORBITER_VK_UPSCALER";
			int t = EnvTaa (), m = EnvMsaa ();
			const char *u = EnvUpscaler ();
			aaLocked = true;
			lockTaa = t != 0;
			lockSamples = m > 0 ? m : t == 0 ? 4 : 1;
			lockFsr = lockTaa && lockSamples == 1 && !(u && !strcmp (u, "taa"));
			antiAliasing = lockSamples > 1 ? AA_MSAA : !lockTaa ? AA_OFF : lockFsr ? AA_FSR : AA_TAA;
			continue;
		}
		const char *e = it.env ? getenv (it.env) : nullptr;
		if (!e) continue;
		locked[n] = it.env;
		if (!strcmp (it.key, "DisplayMode")) skyEnhanced = Same (e, "enhanced");
		else setValue (it, parse (it, e), false);
	}
	inEffect = Values ();
	oapiWriteLogV ("VulkanClient: graphics settings: anti-aliasing %s, render scale %.2f, ray tracing %s",
		AA_CHOICES[antiAliasing], renderScale, rayTracing ? "on" : "off");
}

void GfxSettings::Save () const
{
	if (gfxPath.empty ()) return; // (offscreen)
	bool one = gfxPath == skyPath; // (ORBITER_VK_GRAPHICS_CFG: all in one file)
	std::error_code ec;
	if (!one) std::filesystem::create_directories ("Config/VK", ec);
	const auto &items = Items ();
	std::vector<float> v = Values ();
	auto text = [&](size_t n) -> std::string {
		const Item &it = items[n];
		char buf[64];
		if (it.type == GraphicsSetting::TOGGLE) return v[n] != 0.0f ? "TRUE" : "FALSE";
		if (it.type == GraphicsSetting::CHOICE) {
			if (!strcmp (it.key, "AntiAliasing")) { static const char *names[] = { "Off", "MSAA", "TAA", "FSR" }; return names[(int)v[n]]; }
			return it.choice[(int)v[n]];
		}
		snprintf (buf, sizeof(buf), "%g", v[n]);
		return buf;
	};
	// (a locked setting keeps its value in the file)
	std::vector<std::pair<std::string, std::string>> file[2];
	file[0] = ReadKeys (gfxPath.c_str ());
	auto fileValue = [&](size_t n) -> std::string {
		const Item &it = items[n];
		for (auto &kv : (it.file == GRAPHICS_CFG || one ? file[0] : file[1])) if (Same (kv.first, it.key)) return kv.second;
		return std::string ();
	};
	file[1] = ReadKeys (skyPath.c_str ());

	// Graphics.cfg: written whole
	{
		std::ofstream ofs (gfxPath);
		ofs << "; Graphics settings of the Vulkan client (the settings screen's Graphics page).\n"
			   "; An ORBITER_VK_* environment variable overrides a setting.\n\n";
		const char *group = nullptr;
		for (size_t n = 0; n < items.size (); n++) {
			const Item &it = items[n];
			if (it.file != GRAPHICS_CFG && !one) continue;
			if (!group || strcmp (group, it.group)) ofs << (group ? "\n" : "") << "; " << it.group << "\n", group = it.group;
			std::string val = locked.size () > n && locked[n].size () ? fileValue (n) : text (n);
			if (val.empty ()) continue; // (locked, not in the file)
			ofs << it.key << " = " << val << "\n";
		}
	}
	// Sky.cfg: its sky settings replaced, the rest (and the comments) kept
	if (!one) {
		std::vector<std::string> lines;
		{
			std::ifstream ifs (skyPath);
			std::string line;
			while (std::getline (ifs, line)) lines.push_back (line);
		}
		for (size_t n = 0; n < items.size (); n++) {
			const Item &it = items[n];
			if (it.file != SKY_CFG || (locked.size () > n && locked[n].size ())) continue;
			std::string val = text (n);
			bool done = false;
			for (auto &line : lines) {
				std::string body = line.substr (0, line.find (';'));
				size_t eq = body.find ('=');
				if (eq == std::string::npos || !Same (Trim (body.substr (0, eq)), it.key)) continue;
				size_t c = line.find (';');
				std::string comment = c == std::string::npos ? "" : "        " + line.substr (c);
				line = std::string (it.key) + " = " + val + comment;
				done = true;
			}
			if (!done) lines.push_back (std::string (it.key) + " = " + val);
		}
		std::ofstream ofs (skyPath);
		for (auto &l : lines) ofs << l << "\n";
	}
}

const std::vector<GraphicsSetting> &GfxSettings::List (bool hardwareRT, bool fsr)
{
	const auto &items = Items ();
	std::vector<float> v = Values ();
	if (!started) inEffect = v; // (no session yet: what is set applies when one starts)
	list.resize (items.size ());
	for (size_t n = 0; n < items.size (); n++) {
		const Item &it = items[n];
		GraphicsSetting &s = list[n];
		s = GraphicsSetting{};
		s.key = it.key, s.group = it.group, s.label = it.label, s.help = it.help;
		s.type = it.type, s.apply = it.apply;
		s.nchoice = it.nchoice, s.choice = it.choice;
		s.vmin = it.vmin, s.vmax = it.vmax, s.step = it.step, s.format = it.format, s.display = it.display;
		s.value = v[n];
		s.effective = n < inEffect.size () ? inEffect[n] : v[n];
		std::copy (it.preset, it.preset + 4, s.preset);
		s.lockedBy = n < locked.size () && locked[n].size () ? locked[n].c_str () : nullptr;
		s.requires = it.requires;
		if (it.rt && !hardwareRT) s.unavailable = "Needs a graphics card with hardware ray tracing";
		if (!strcmp (it.key, "AntiAliasing") && !fsr) {
			s.nchoice = 3; // (FSR 3 is not built in)
			s.value = std::min (s.value, 2.0f), s.effective = std::min (s.effective, 2.0f);
		}
		if (SceneSamples () > 1 && (!strcmp (it.key, "Reflections") || !strcmp (it.key, "HeatShimmer")))
			s.unavailable = "Not with multisampling";
		if (!strcmp (it.key, "RenderScale") && !TemporalAA ())
			s.unavailable = "Needs temporal anti-aliasing or FSR 3 to upscale";
	}
	return list;
}

void GfxSettings::Set (int index, float value)
{
	const auto &items = Items ();
	if (index < 0 || index >= (int)items.size ()) return;
	if ((int)locked.size () > index && locked[index].size ()) return;
	const Item &it = items[index];
	if (it.b) this->*it.b = value != 0.0f;
	else if (it.f) this->*it.f = std::clamp (value, it.vmin, it.vmax);
	else if (it.i) this->*it.i = std::clamp ((int)std::lround (value), 0, it.nchoice - 1);
	else skyEnhanced = value != 0.0f;
	if (it.apply == GraphicsSetting::LIVE && index < (int)inEffect.size ()) inEffect[index] = Values ()[index];
}

GfxSettings &Gfx ()
{
	static GfxSettings s;
	return s;
}

}

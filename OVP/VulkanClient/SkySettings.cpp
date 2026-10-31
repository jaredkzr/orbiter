// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "SkySettings.h"
#include "OrbiterAPI.h"
#include "GraphicsAPI.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {
	const char *SETTINGS = "Config/VK/Sky.cfg";
	const char *DATA[] = {
		"Config/CSphere/Sky/galaxy.cfg",    // the Milky Way's model
		"Config/CSphere/Sky/dust.bin",      // the 3D dust map
		"Config/CSphere/Sky/stars.bin",     // the 3D star catalogue
		"Config/CSphere/Sky/galaxies.cfg",  // the other galaxies
		"Config/CSphere/Sky/globulars.csv"  // the globular clusters
	};
	const double DEFAULT_BG_INTENSITY = 0.3; // (Orbiter's default CSphereBgIntensity: the measured sky)

	bool Same (const std::string &a, const char *b) // (ignoring case)
	{
		size_t i = 0;
		for (; i < a.size () && b[i]; i++)
			if (tolower ((unsigned char)a[i]) != tolower ((unsigned char)b[i])) return false;
		return i == a.size () && !b[i];
	}

	bool Bool (const std::string &v)
	{
		return Same (v, "true") || Same (v, "on") || atoi (v.c_str ()) != 0;
	}
}

void SkySettings::LoadOptions (oapi::GraphicsClient *gc)
{
	if (const bool *b = (const bool*)gc->GetConfigParam (CFGPRM_CSPHEREUSESTARDOTS)) stars = *b;
	if (const bool *b = (const bool*)gc->GetConfigParam (CFGPRM_CSPHEREUSEBGIMAGE)) milkyWay = *b;
	if (const StarRenderPrm *p = (const StarRenderPrm*)gc->GetConfigParam (CFGPRM_STARRENDERPRM))
		starLimit = (float)std::clamp (p->mag_lo, 3.0, 12.0);
	if (const double *v = (const double*)gc->GetConfigParam (CFGPRM_CSPHEREINTENS))
		intensity = (float)std::clamp (*v / DEFAULT_BG_INTENSITY, 0.0, 10.0);
	if (const char *e = getenv ("ORBITER_VK_SKY_STARLIMIT")) starLimit = (float)atof (e);
}

void SkySettings::Load (oapi::GraphicsClient *gc)
{
	LoadOptions (gc);
	std::ifstream ifs (SETTINGS);
	std::string line;
	while (std::getline (ifs, line)) {
		line = line.substr (0, line.find (';'));
		size_t eq = line.find ('=');
		if (eq == std::string::npos) continue;
		std::string key, val;
		std::istringstream (line.substr (0, eq)) >> key;
		std::istringstream (line.substr (eq + 1)) >> val;
		if (key.empty () || val.empty ()) continue;
		if (key == "DisplayMode") enhanced = Same (val, "Enhanced");
		else if (key == "EnhancedContrast") contrast = (float)std::clamp (atof (val.c_str ()), 0.5, 3.0);
		else if (key == "EnhancedHighlights") highlight = (float)std::clamp (atof (val.c_str ()), 1.0, 1e6);
		else if (key == "EnhancedSaturation") saturation = (float)std::clamp (atof (val.c_str ()), 0.0, 3.0);
		else if (key == "EnhancedGain") gain = (float)std::clamp (atof (val.c_str ()), -4.0, 8.0);
		else if (key == "Nebulae") nebulae = Bool (val);
		else if (key == "Galaxies") galaxies = Bool (val);
		else if (key == "ZodiacalLight") zodiacal = Bool (val);
		else if (key == "BakeBudget") bakeBudget = (float)std::clamp (atof (val.c_str ()), 0.25, 50.0);
		else oapiWriteLogV ("VulkanClient: %s: unknown setting %s", SETTINGS, key.c_str ());
	}
	// (tests)
	if (const char *e = getenv ("ORBITER_VK_SKY_MODE")) enhanced = Same (e, "enhanced");
	if (const char *e = getenv ("ORBITER_VK_SKY_CONTRAST")) contrast = (float)atof (e);
	if (const char *e = getenv ("ORBITER_VK_SKY_HIGHLIGHTS")) highlight = (float)atof (e);
	if (const char *e = getenv ("ORBITER_VK_SKY_SATURATION")) saturation = (float)atof (e);
	if (const char *e = getenv ("ORBITER_VK_SKY_ENHANCE")) gain = (float)atof (e);
	if (const char *e = getenv ("ORBITER_VK_SKY_NEBULAE")) nebulae = atoi (e) != 0;
	if (const char *e = getenv ("ORBITER_VK_SKY_OBJECTS")) galaxies = atoi (e) != 0;
	if (const char *e = getenv ("ORBITER_VK_ZODIACAL")) zodiacal = atoi (e) != 0;
	if (const char *e = getenv ("ORBITER_VK_SKY_BUDGET")) bakeBudget = (float)atof (e);
	if (const char *e = getenv ("ORBITER_VK_STARS")) stars = atoi (e) != 0;
	oapiWriteLogV ("VulkanClient: sky: %s display, stars %s (to V = %.1f), diffuse light %s (x %.2f), nebulae %s, galaxies %s, zodiacal light %s, bake budget %.1f ms",
		enhanced ? "enhanced" : "realistic", stars ? "on" : "off", starLimit, milkyWay ? "on" : "off", intensity,
		nebulae ? "on" : "off", galaxies ? "on" : "off", zodiacal ? "on" : "off", bakeBudget);
}

std::vector<std::string> SkySettings::MissingData ()
{
	std::vector<std::string> missing;
	for (const char *p : DATA)
		if (FILE *f = fopen (p, "rb")) fclose (f);
		else missing.push_back (p);
	return missing;
}

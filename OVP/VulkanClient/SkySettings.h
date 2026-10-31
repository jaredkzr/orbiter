// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Settings of the simulated sky. Orbiter's own celestial sphere options
// (Orbiter.cfg: EnableBackgroundStars, StarPrm, EnableBackgroundImage,
// CSphereBgIntensity) switch the stars and the Milky Way and set the
// faintest star drawn and the Milky Way's brightness; the rest is in
// Config/VK/Sky.cfg. The sky's data (Config/CSphere/Sky) are required.
// =======================================================================

#ifndef __SKYSETTINGS_H
#define __SKYSETTINGS_H

#include <string>
#include <vector>

namespace oapi { class GraphicsClient; }

struct SkySettings {
	bool stars = true;          ///< the stars drawn as points
	bool milkyWay = true;       ///< the diffuse light (Milky Way, nebulae, galaxies, zodiacal light)
	float starLimit = 8.0f;     ///< faintest star drawn as a point (V, from the observer)
	float intensity = 1.0f;     ///< of the diffuse light (1: as measured)
	bool enhanced = false;      ///< display: a long-exposure photograph rather than the eye's view
	float contrast = 2.0f;      ///< enhanced: the diffuse light's contrast (exponent of its luminance)
	float highlight = 20.0f;    ///< enhanced: where its highlights are compressed (relative to the Milky Way's mean)
	float saturation = 1.8f;    ///< enhanced: its saturation
	float gain = 1.0f;          ///< enhanced: its gain [stops]
	bool nebulae = true;
	bool galaxies = true;       ///< the other galaxies and the globular clusters
	bool zodiacal = true;
	float bakeBudget = 2.0f;    ///< GPU time per frame for recomputing the diffuse light [ms]

	/// The settings: Orbiter's options, Config/VK/Sky.cfg, then the test
	/// overrides of the environment (ORBITER_VK_SKY_MODE=enhanced, ...)
	void Load (oapi::GraphicsClient *gc);
	/// Only the options Orbiter can change while running (OPTCAT_CELSPHERE)
	void LoadOptions (oapi::GraphicsClient *gc);

	/// The sky's data files that are missing
	static std::vector<std::string> MissingData ();
};

#endif // !__SKYSETTINGS_H

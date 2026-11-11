// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The client's graphics settings: Config/VK/Graphics.cfg (and the sky's
// parts of Config/VK/Sky.cfg), changed on the settings screen's Graphics
// page (GraphicsClient::clbkGetSettings). An ORBITER_VK_* environment
// variable still overrides a setting (it is then shown locked).
// Offscreen runs (tests, benchmarks, scenario images) neither read nor
// write these files, so that the user's choices don't change them, unless
// ORBITER_VK_GRAPHICS_CFG names a file for all the settings.
// =======================================================================

#ifndef __GFXSETTINGS_H
#define __GFXSETTINGS_H

#include "GraphicsAPI.h"
#include <string>
#include <vector>

namespace vkc {

using oapi::GraphicsSetting;

struct GfxSettings {
	enum AntiAliasing { AA_OFF, AA_MSAA, AA_TAA, AA_FSR };

	int antiAliasing = AA_FSR;  ///< (AA_TAA where FSR 3 is not built in)
	float renderScale = 1.0f;   ///< scene images: display size times this (upscaled)
	float sharpness = 0.5f;     ///< FSR 3's sharpening
	bool rayTracing = true;     ///< ray tracing (where the hardware has ray queries)
	bool rtReflections = true;
	bool rtLighting = true;     ///< ray traced shadows, ambient occlusion, planet light
	bool ao = true;             ///< screen-space ambient occlusion (without ray traced lighting)
	bool ssr = true;            ///< screen-space reflections
	float bloom = 0.2f;
	bool autoExposure = true;
	bool clouds = true;         ///< volumetric clouds
	bool plumes = true;         ///< volumetric exhaust plumes
	bool shimmer = true;        ///< heat shimmer behind exhausts
	bool particles = true;      ///< particle streams (exhaust, smoke, dust)
	float particleDensity = 4.0f;
	enum SmokeRendering { SMOKE_SPRITES, SMOKE_SCREEN, SMOKE_VOLUMETRIC };
	int smokeRendering = SMOKE_VOLUMETRIC; ///< large smoke (launch clouds): sprites, screen-space or volumetric
	bool smokeTrails = true;    ///< launch smoke trails that stay in the sky, carried by the wind
	enum Fireballs { FIREBALL_SPRITES, FIREBALL_VOLUME, FIREBALL_FULL };
	int fireballs = FIREBALL_VOLUME; ///< crash fireballs: sprites, marched volumes (half resolution), or at the full resolution
	bool engineLight = true;    ///< engines light their surroundings
	float cameraShake = 1.0f;
	bool skyEnhanced = false;   ///< sky display: enhanced (long exposure) or realistic
	bool nebulae = true, galaxies = true, zodiacal = true;

	/// Read the configuration and the environment (once, at start)
	void Load ();
	/// Write the configuration
	void Save () const;

	/// The scene's multisampling (1 to 8 samples), the temporal
	/// anti-aliasing and the FSR 3 upscaler as set (with the environment's
	/// ORBITER_VK_MSAA, ORBITER_VK_TAA and ORBITER_VK_UPSCALER)
	int SceneSamples () const { return aaLocked ? lockSamples : antiAliasing == AA_MSAA ? 4 : 1; }
	bool TemporalAA () const { return aaLocked ? lockTaa : antiAliasing == AA_TAA || antiAliasing == AA_FSR; }
	bool UseFsr () const { return aaLocked ? lockFsr : antiAliasing == AA_FSR; }

	/// The settings for the user interface (values current); hardware:
	/// ray queries are available, fsr: the FSR 3 upscaler is built in
	const std::vector<GraphicsSetting> &List (bool hardwareRT, bool fsr);
	/// Change a setting by its index in List
	void Set (int index, float value);
	/// The values in effect (for List's "effective"): set when a session
	/// starts, and at once for live settings
	void MarkSessionStart () { inEffect = Values (); started = true; }

private:
	struct Item;
	static const std::vector<Item> &Items ();
	std::vector<float> Values () const;
	std::vector<GraphicsSetting> list;
	std::vector<float> inEffect;
	std::vector<std::string> locked; // environment variable per item ("" if none)
	bool started = false;
	std::string gfxPath, skyPath; // ("": none)
	// anti-aliasing set by the environment (ORBITER_VK_TAA, _MSAA, _UPSCALER)
	bool aaLocked = false, lockTaa = true, lockFsr = true;
	int lockSamples = 1;
};

/// The client's settings
GfxSettings &Gfx ();

}

#endif // !__GFXSETTINGS_H

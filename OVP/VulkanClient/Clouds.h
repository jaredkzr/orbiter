// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Volumetric clouds near the camera: the planet's cloud tiles around the
// camera are rendered into a coverage map, through which a compute pass
// ray-marches a cloud shell (shaped by 3D noise) at half resolution; the
// result is composited over the opaque scene. Farther away, the 2D cloud
// layer takes over.
// =======================================================================

#ifndef __CLOUDS_H
#define __CLOUDS_H

#include "VkContext.h"
#include "VkMath.h"
#include <algorithm>
#include <vector>

class PlanetSurface;
class TextureManager;

class VolumetricClouds {
public:
	static constexpr double RANGE = 60e3;        ///< volumetric clouds up to this distance [m]
	static constexpr double MAX_CAM_ALT = 40e3;  ///< ... when the camera is below this altitude [m]

	bool Init (vkc::Context *ctx, TextureManager *texmgr, VkDescriptorSetLayout atmLayout);
	void Shutdown ();

	struct Frame {
		PlanetSurface *clouds;      // the planet's cloud layer
		VkDescriptorSet atmSet;     // the planet's atmosphere
		VECTOR3 camPlanet;          // camera position, planet frame
		double planetRadius, cloudAlt;
		VkImageView prepass;        // prepass image (view depth), render size
		float fx, fy;               // projection factors
		float ambient[3];
		float windOffset[3];        // noise drift (planet frame) [m]
	};
	/// Coverage map and ray-march (outside rendering scopes, after the prepass)
	void Render (const Frame &f);

	/// Cloud decks of a gas giant (Config/VK/GasGiant/<name>.cfg; altitudes
	/// above the 1-bar level)
	struct Deck { double base, top, ext, scale; float albedo[3], coverage, contrast; bool towers; float holes, stretch; };
	struct Decks {
		std::vector<Deck> decks;
		float haze[3] = { 0.5f, 0.4f, 0.3f };
		double hazeExt = 2e-6, hazeH = 30e3, towerTop = 15e3, stretch = 3.0, tint = 0.6;
		float gasAbs[3] = { 0.06f, 0.09f, 0.15f };   // the gas's absorption per bar (rgb)
		float gasRay[3] = { 0.055f, 0.09f, 0.19f };   // its Rayleigh scattering per bar (rgb)
		float gasDeep[3] = { 0.0f, 0.0f, 0.0f };      // more absorption per bar below gasDeepP (rgb)
		double gasDeepP = 1.0;                        // [bar]
		float mapLevels[3] = { 0.15f, 0.45f, 0.6f };  // the planet map's luminance: belts below, zones above; its white (VPlanet)
		double chromoBase = 0.0, chromoTop = 0.0, chromoStrength = 1.0; // the chromophores' layer (none: top <= base)
		float folds = 0.0, lanes = 0.0;               // the flow's folds within the bands, the clear lanes between them (0..1)
		double reach = 1500e3;                        // the decks drawn up to this height above their top [m] (fading out over the upper 30 percent)
		/// the gas's absorption optical depth above pressure bar [bar] (colour k)
		double GasAbs (int k, double bar) const { return gasAbs[k] * bar + gasDeep[k] * std::max (bar - gasDeepP, 0.0); }
		/// its absorption per bar there (the coefficient's)
		double GasAbsRate (int k, double bar) const { return gasAbs[k] + (bar > gasDeepP ? gasDeep[k] : 0.0); }
		double vortexV = 120.0, vortexCore = 0.3, towerSpin = 40.0, vortexTwist = 3.0;
		double Top () const;      ///< top of the highest deck (not the haze) [m]
		double CloudTop () const; ///< top of the highest cloud deck (extinction 1e-4/m or more)
		/// optical depth of the decks and haze above altitude alt [m] along a
		/// direction with sine of elevation mu (mean densities: as
		/// ggclouds.comp's ColumnAbove)
		double ColumnAbove (double alt, double mu) const;
		/// the sunlight's share (green) reaching altitude alt [m] (pressure
		/// bar [bar]) along a direction with sine of elevation mu: the
		/// direct beam and the diffuse light (as ggclouds.comp's ColumnLight)
		double Light (double alt, double mu, double bar, double *direct = nullptr) const;
	};
	static bool LoadDecks (const char *planet, Decks &d);
	struct GasGiantFrame {
		OBJHANDLE hPlanet;
		const Decks *decks;
		VkDescriptorSet atmSet;     // the planet's atmosphere
		VECTOR3 camPlanet;          // camera position, planet frame
		double radius;              // 1-bar radius [m]
		double floorRadius;         // the atmosphere's floor [m]
		VkImageView prepass;
		float fx, fy;
		float sky[3];               // skylight above the decks
		double time;                // [s]
		MATRIX3 viewToPlanet;       // camera frame -> planet frame (rotation)
	};
	/// Ray-march a gas giant's decks (outside rendering scopes, after the prepass)
	void RenderGasGiant (const GasGiantFrame &f);
	/// their opacity this frame (fading out towards their reach: the
	/// planet's cloud layer drawn behind them until opaque)
	float DeckOpacity () const { return active ? ggOpacity : 0.0f; }

	/// Lightning in the storm towers near the camera (gas giants): the
	/// flashes lit at the last RenderGasGiant: global position, brightness
	/// (light source colour scale; 0: between strokes)
	struct Flash { VECTOR3 gpos; double brightness; };
	const std::vector<Flash> &Flashes () const { return flashes; }
	/// Composite over the scene (in the scene's rendering scope)
	void Composite ();

	bool enabled = true;
	float brightness = 2.0f; // sun irradiance scale of the cloud lighting
	float extinction = 0.03f; // [1/m] at full density

	/// tileable noise volumes (shape: Perlin-Worley, detail: Worley fBm)
	VkImageView ShapeNoiseView () const { return shapeNoise.view; }
	VkImageView DetailNoiseView () const { return detailNoise.view; }

	/// for the clouds' shadows (atmosphere.glsl's CloudShadow): coverage map
	/// and shape noise, and this frame's parameters (map centre, half size;
	/// noise offset, scale; shell base and top radius, extinction, coverage
	/// scale), valid if rendered this frame
	VkImageView CoverageView () const { return coverage.view; }
	VkSampler ClampSampler () const { return clampSampler; }
	VkSampler RepeatSampler () const { return repeatSampler; }
	bool Active () const { return active; }
	float shadowMap[4] = {}, shadowNoise[4] = {}, shadowShell[4] = {};

	VkPipeline CoveragePipeline () const { return coveragePipe; }
	VkPipelineLayout CoverageLayout () const { return coverageLayout; }

private:
	void CreateOutput (VkImageView prepass);
	vkc::Image CreateImage (VkImageType type, VkFormat fmt, VkExtent3D e, VkImageUsageFlags usage, uint32_t mips = 1);

	vkc::Context *ctx = nullptr;
	TextureManager *texmgr = nullptr;
	vkc::Image shapeNoise, detailNoise, coverage, output;
	VkImageView shapeMipView = VK_NULL_HANDLE, detailMipView = VK_NULL_HANDLE; // (the noise's mip chains)
	VkExtent2D outExtent{};
	uint32_t generation = ~0u;
	VkImageView outPrepass = VK_NULL_HANDLE;
	uint32_t frameNo = 0;
	VkSampler repeatSampler = VK_NULL_HANDLE, clampSampler = VK_NULL_HANDLE, mipSampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout marchSetLayout = VK_NULL_HANDLE, compSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet marchSet = VK_NULL_HANDLE, compSet = VK_NULL_HANDLE;
	VkPipelineLayout marchLayout = VK_NULL_HANDLE, compLayout = VK_NULL_HANDLE, coverageLayout = VK_NULL_HANDLE;
	VkPipeline marchPipe = VK_NULL_HANDLE, compPipe = VK_NULL_HANDLE, coveragePipe = VK_NULL_HANDLE;
	VkPipeline ggPipe = VK_NULL_HANDLE; // gas giant decks (ggclouds.comp)
	// (their temporal accumulation: history of light and transmittance, and
	// the clouds' distance, ping-pong; the camera of the last frame)
	vkc::Image ggHist[2], ggDepth[2];
	int ggParity = 0;
	bool ggHistValid = false;
	int ggStill = 0; // frames the camera has been still (the half-tile march)
	VECTOR3 ggPrevCam{};
	MATRIX3 ggPrevRot{};
	std::vector<Flash> flashes;
	float ggOpacity = 1.0f;
	vkc::Buffer ggBuf;                  // their parameters, one slot per frame in flight
	bool active = false; // rendered this frame
};

#endif // !__CLOUDS_H

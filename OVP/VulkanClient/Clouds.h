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
	vkc::Image CreateImage (VkImageType type, VkFormat fmt, VkExtent3D e, VkImageUsageFlags usage);

	vkc::Context *ctx = nullptr;
	TextureManager *texmgr = nullptr;
	vkc::Image shapeNoise, detailNoise, coverage, output;
	VkExtent2D outExtent{};
	uint32_t generation = ~0u;
	VkImageView outPrepass = VK_NULL_HANDLE;
	uint32_t frameNo = 0;
	VkSampler repeatSampler = VK_NULL_HANDLE, clampSampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout marchSetLayout = VK_NULL_HANDLE, compSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet marchSet = VK_NULL_HANDLE, compSet = VK_NULL_HANDLE;
	VkPipelineLayout marchLayout = VK_NULL_HANDLE, compLayout = VK_NULL_HANDLE, coverageLayout = VK_NULL_HANDLE;
	VkPipeline marchPipe = VK_NULL_HANDLE, compPipe = VK_NULL_HANDLE, coveragePipe = VK_NULL_HANDLE;
	bool active = false; // rendered this frame
};

#endif // !__CLOUDS_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Planetary atmospheres: scattering parameters, sky rendering, and the
// uniform data used for aerial perspective on the planet surface
// =======================================================================

#ifndef __ATMOSPHERE_H
#define __ATMOSPHERE_H

#include "VkContext.h"
#include "OrbiterAPI.h"

class Scene;

/// Shared resources for atmosphere rendering (owned by the scene)
class AtmosphereRenderer {
public:
	bool Init (vkc::Context *ctx, class TextureManager *texmgr);
	/// Write the ground textures of a set
	void WriteGround (VkDescriptorSet set, class VkTexture *west, class VkTexture *east, int binding = 1) const;
	void Shutdown ();
	VkDescriptorSetLayout SetLayout () const { return setLayout; }
	VkDescriptorPool Pool () const { return pool; }
	VkPipeline SkyPipeline () const { return skyPipeline; }
	VkPipelineLayout SkyLayout () const { return skyLayout; }
	/// set with uniform data for "no atmosphere"
	VkDescriptorSet NoAtmosphereSet () const { return noneSet; }
	/// the volumetric clouds' coverage map and shape noise (for their
	/// shadows), written into every set; call before atmospheres are made
	void SetCloudVolume (VkImageView coverage, VkSampler covSampler, VkImageView shape, VkSampler shapeSampler);
	void WriteCloudVolume (VkDescriptorSet set) const;

	/// Compute the transmittance and multiple-scattering tables of an
	/// atmosphere (images created by the caller, storage usage)
	struct LutParams { float betaR[4], betaM[4], ozone[4], radii[4]; };
	void ComputeLuts (const LutParams &p, const vkc::Image &transmittance, const vkc::Image &multiscat);
	/// sampler of the tables (linear, clamped)
	VkSampler LutSampler () const { return lutSampler; }

private:
	vkc::Context *ctx = nullptr;
	class TextureManager *texmgr = nullptr;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkPipelineLayout skyLayout = VK_NULL_HANDLE;
	VkPipeline skyPipeline = VK_NULL_HANDLE;
	vkc::Buffer noneUbo;
	VkDescriptorSet noneSet = VK_NULL_HANDLE;
	VkSampler lutSampler = VK_NULL_HANDLE;
	VkDescriptorImageInfo cloudVol[2] = {};
	VkDescriptorSetLayout lutSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout lutLayout = VK_NULL_HANDLE;
	VkPipeline transmittancePipe = VK_NULL_HANDLE, multiscatPipe = VK_NULL_HANDLE;
};

class Atmosphere {
public:
	Atmosphere (OBJHANDLE hPlanet, Scene *scene);
	~Atmosphere ();

	bool Present () const { return present; }
	/// Write this frame's uniform data (camera and sun relative to the planet)
	void Update ();
	/// Draw the sky (if the camera can see the atmosphere)
	void RenderSky ();
	/// Descriptor set with this frame's uniform data
	VkDescriptorSet Set () const;
	/// Planet surface (hemisphere textures, level 4 tiles) and mean colour, used
	/// for reflections of the planet. Call before the first use of the sets.
	void SetGround (class VkTexture *west, class VkTexture *east, const float colour[3]);
	/// Cloud layer (hemisphere textures) at the given altitude, for cloud shadows
	void SetClouds (class VkTexture *west, class VkTexture *east, double alt);

private:
	OBJHANDLE hPlanet;
	Scene *scene;
	vkc::Context *ctx;
	bool present = false;
	double radius = 0.0, atmRadius = 0.0;
	float betaR[4] = {}, betaM[4] = {};
	float overcastSky[3] = { 0, 0, 0 }; // diffuse radiance under an overcast (Venus, Titan)
	float mieLobe[4] = {}; // aerosols' forward lobe per colour, its share (atmosphere.glsl's AtmMiePhase)
	float mieG = 0.76f, sunIntensity = 6.0f; // (sky radiance scale: physically about pi for a sunlit white surface of 1)
	float airglow = 0.0f; // strength of the airglow layer (Earth)
	float detail[4] = { 0, 0, 0, 0 }; // procedural surface detail (atmosphere.glsl)
	float surfSaturation = 1.0f;     // colour saturation of the surface textures
	float aurora[4] = { 0, 0, 0, 0 }; // geomagnetic pole, aurora strength
	float ozone[4] = {};                  // ozone absorption [1/m] (Earth)
	float groundAlbedo = 0.3f;            // for light reflected into the sky
	vkc::Image transmittanceLut, multiscatLut; // precomputed tables
	bool visible = false;
	// per frame slot, for the main view and the environment probe
	static constexpr uint32_t NSETS = 2 * vkc::FRAMES_IN_FLIGHT;
	vkc::Buffer ubo[NSETS];
	VkDescriptorSet sets[NSETS] = {};
	uint32_t SetIndex () const;
	float groundColour[4] = { 0.5f, 0.5f, 0.5f, 0.0f };
	float cloudParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	bool ripple = false; // wave normals on water
};

/// skylight of an overcast atmosphere (Venus, Titan): the ground's
/// irradiance from the sky relative to the sun's above it; false for others
bool OvercastSkylight (const char *planet, float rgb[3]);

#endif // !__ATMOSPHERE_H

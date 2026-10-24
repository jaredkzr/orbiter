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
	void WriteGround (VkDescriptorSet set, class VkTexture *west, class VkTexture *east) const;
	void Shutdown ();
	VkDescriptorSetLayout SetLayout () const { return setLayout; }
	VkDescriptorPool Pool () const { return pool; }
	VkPipeline SkyPipeline () const { return skyPipeline; }
	VkPipelineLayout SkyLayout () const { return skyLayout; }
	/// set with uniform data for "no atmosphere"
	VkDescriptorSet NoAtmosphereSet () const { return noneSet; }

private:
	vkc::Context *ctx = nullptr;
	class TextureManager *texmgr = nullptr;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkPipelineLayout skyLayout = VK_NULL_HANDLE;
	VkPipeline skyPipeline = VK_NULL_HANDLE;
	vkc::Buffer noneUbo;
	VkDescriptorSet noneSet = VK_NULL_HANDLE;
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

private:
	OBJHANDLE hPlanet;
	Scene *scene;
	vkc::Context *ctx;
	bool present = false;
	double radius = 0.0, atmRadius = 0.0;
	float betaR[4] = {}, betaM[4] = {};
	float mieG = 0.76f, sunIntensity = 16.0f;
	bool visible = false;
	vkc::Buffer ubo[vkc::FRAMES_IN_FLIGHT];
	VkDescriptorSet sets[vkc::FRAMES_IN_FLIGHT] = {};
	float groundColour[4] = { 0.5f, 0.5f, 0.5f, 0.0f };
};

#endif // !__ATMOSPHERE_H

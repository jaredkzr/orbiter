// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The zodiacal light: sunlight scattered by the dust of the inner solar
// system (the smooth cloud of Kelsall et al. 1998, Shaders/zodiacal.glsl),
// seen from the observer's place relative to the Sun, scaled to the
// measured brightness at 1 AU (Leinert et al. 1998). Computed into a small
// cube map (Galactic frame, S10 per band) again as the observer moves; the
// Milky Way's pass draws it (GalaxySky).
// =======================================================================

#ifndef __ZODIACAL_H
#define __ZODIACAL_H

#include "VkContext.h"

class GalaxySky;

class Zodiacal {
public:
	static constexpr uint32_t SIZE = 128; // of a cube face

	void Init (vkc::Context *ctx, const GalaxySky &sky);
	void Shutdown ();
	/// Compute the light again if the observer (heliocentric Galactic
	/// [pc]) has moved enough (outside rendering scopes; at once: Init)
	void Update (const double pos[3], bool now = false);
	VkImageView View () const { return view; }
	/// the model's largest deviation from the measured light at 1 AU (a fraction)
	double Deviation () const { return deviation; }

private:
	void Compute (VkCommandBuffer cmd);

	vkc::Context *ctx = nullptr;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE, arrayView = VK_NULL_HANDLE;
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipelineLayout pl = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	double scale = 0.0;        // S10 per model unit
	float colour[4] = {};
	double eclFromGal[3][3] = {};
	double last[3] = { 1e30, 0.0, 0.0 }; // observer of the light [AU]
	bool enabled = true;
	double deviation = 0.0;
};

#endif // !__ZODIACAL_H

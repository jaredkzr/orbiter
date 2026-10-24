// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Ray traced lighting visibility (hardware ray queries): for each pixel of
// the prepass, the visibility of the sun (soft shadows), ambient occlusion,
// the visibility of the lit planet nearby (planet light) and shadows of
// the strongest local lights. One noisy estimate per frame, accumulated
// over frames and filtered; the scene shaders read the result (frame set)
// in place of the shadow maps and screen-space ambient occlusion.
// =======================================================================

#ifndef __RTLIGHTING_H
#define __RTLIGHTING_H

#include "VkContext.h"

class RTLighting {
public:
	static constexpr VkFormat VIS_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
	static constexpr VkFormat LIGHT_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
	static constexpr int SHADOWED_LIGHTS = 4;

	/// frame: frame set layout; rtScene: ray tracing scene set layout
	bool Init (vkc::Context *ctx, VkDescriptorSetLayout frame, VkDescriptorSetLayout rtScene);
	void Shutdown ();
	bool Enabled () const { return ctx != nullptr; }

	/// (Re)create the images for the current extent. Returns true if they
	/// were recreated (descriptors referring to them must be rewritten; the
	/// GPU is idle then).
	bool EnsureSize (VkImageView prepass, VkImageView velocity, VkImageView material);
	/// Trace and filter (outside rendering scopes, after the ray tracing
	/// scene is built)
	void Compute (VkDescriptorSet frameSet, VkDescriptorSet rtSet, float fx, float fy, const float jitter[2],
		float sunRadius, bool reset);

	/// results (general layout)
	VkImageView VisView () const { return outA.view; }
	VkImageView LightView () const { return outL.view; }
	VkSampler Sampler () const { return nearest; }

	float aoRadius = 4.0f;     // [m]
	float aoMaxDist = 2000.0f; // [m]

private:
	void CreateImages (VkImageView prepass, VkImageView velocity, VkImageView material);
	void DestroyImages ();
	vkc::Image CreateImage (VkFormat fmt);

	vkc::Context *ctx = nullptr;
	VkExtent2D extent{};
	uint32_t generation = ~0u;
	vkc::Image rawA, rawL, accA[2], accL[2], outA, outL;
	int cur = 0;
	bool historyValid = false;
	uint32_t frameNo = 0;
	VkSampler nearest = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet sets[2] = {};
	VkPipelineLayout traceLayout = VK_NULL_HANDLE, layout = VK_NULL_HANDLE;
	VkPipeline tracePipe = VK_NULL_HANDLE, temporalPipe = VK_NULL_HANDLE, filterPipe = VK_NULL_HANDLE;
};

#endif // !__RTLIGHTING_H

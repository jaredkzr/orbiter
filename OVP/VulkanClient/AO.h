// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Screen-space ambient occlusion: a single-sample depth prepass of the
// opaque geometry (view normal and depth), from which a compute pass
// derives the occlusion of ambient light per pixel. The scene shaders
// sample the result (frame set) to attenuate ambient light and
// environment reflections.
// =======================================================================

#ifndef __AO_H
#define __AO_H

#include "VkContext.h"

class AmbientOcclusion {
public:
	static constexpr VkFormat PREPASS_FORMAT = VK_FORMAT_R32G32B32A32_SFLOAT;

	bool Init (vkc::Context *ctx);
	void Shutdown ();

	/// (Re)create the images for the current extent. Returns true if they
	/// were recreated (descriptors referring to View() must be rewritten;
	/// the GPU is idle then).
	bool EnsureSize ();

	/// Bind the prepass target (cleared)
	void BeginPrepass ();
	/// Ambient occlusion from the prepass (fx, fy: projection factors)
	void Compute (float fx, float fy);

	/// occlusion image (1: unoccluded), in shader-read layout
	VkImageView View () const { return aoFinal.view; }
	VkSampler Sampler () const { return sampler; }
	VkExtent2D Extent () const { return extent; }

	bool enabled = true;
	float radius = 4.0f;     // [m]
	float strength = 3.5f;
	float directFrac = 0.6f;  // fraction of the occlusion applied to direct light too
	float maxDist = 2000.0f; // [m]

private:
	void CreateImages ();
	void DestroyImages ();
	vkc::Image CreateImage (VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect);

	vkc::Context *ctx = nullptr;
	VkExtent2D extent{};
	uint32_t generation = ~0u;
	vkc::Image prepassColor, prepassDepth, aoRaw, aoFinal;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet aoSet = VK_NULL_HANDLE, blurSet = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline aoPipe = VK_NULL_HANDLE, blurPipe = VK_NULL_HANDLE;
};

#endif // !__AO_H

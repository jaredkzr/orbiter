// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Post-processing of the HDR scene image: bloom and tone mapping into the
// backbuffer
// =======================================================================

#ifndef __POSTFX_H
#define __POSTFX_H

#include "VkContext.h"
#include <vector>

class PostFX {
public:
	bool Init (vkc::Context *ctx);
	void Shutdown ();

	/// Tone-map the scene image into the backbuffer (within a frame)
	void Apply ();

	float exposure = 1.0f;
	float bloomStrength = 0.35f;
	float bloomThreshold = 1.0f;
	float flareStrength = 0.05f;
	bool autoExposure = true;

	/// adapted exposure and metered scene luminance (log2), as of the last
	/// completed frame
	void ExposureState (float &logExposure, float &avgLogLum) const;

private:
	static constexpr int LEVELS = 6;
	struct Level {
		vkc::Image img;
		VkExtent2D extent{};
		VkDescriptorSet set = VK_NULL_HANDLE;
	};
	void CreateTargets ();
	void DestroyTargets ();
	VkPipeline CreatePipeline (const unsigned char *fs, size_t fsSize, VkFormat fmt, bool additive, VkPipelineLayout layout);
	VkPipeline CreateComputePipeline (const unsigned char *cs, size_t csSize);
	void Meter ();
	VkDescriptorSet AllocSet (VkImageView view);
	void Pass (VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet src, const void *pc, uint32_t pcSize);

	vkc::Context *ctx = nullptr;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;      // one sampler + params
	VkPipelineLayout toneLayout = VK_NULL_HANDLE;  // scene + bloom + params
	VkPipeline downPipe = VK_NULL_HANDLE, upPipe = VK_NULL_HANDLE, tonePipe = VK_NULL_HANDLE;
	VkShaderModule vs = VK_NULL_HANDLE;
	Level levels[LEVELS];
	VkDescriptorSet sceneSet = VK_NULL_HANDLE;

	// auto-exposure
	VkDescriptorSetLayout meterSetLayout = VK_NULL_HANDLE; // scene, histogram, state
	VkDescriptorSetLayout stateSetLayout = VK_NULL_HANDLE; // state (tone mapping)
	VkPipelineLayout meterLayout = VK_NULL_HANDLE;
	VkPipeline histPipe = VK_NULL_HANDLE, adaptPipe = VK_NULL_HANDLE;
	vkc::Buffer histBuf, stateBuf;
	VkDescriptorSet meterSet = VK_NULL_HANDLE, stateSet = VK_NULL_HANDLE;
	double lastMeterTime = -1.0;
	uint32_t generation = ~0u; // swapchain generation the targets were made for
};

#endif // !__POSTFX_H

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

	/// Tone-map an HDR image of the scene (the scene image, or the temporal
	/// anti-aliasing output) into the backbuffer (within a frame)
	void Apply (VkImageView input, VkExtent2D inputSize);

	float exposure = 1.0f;
	float bloomStrength = 0.2f;
	float bloomThreshold = 1.2f; // displayed brightness (after exposure) above which light blooms
	float bloomFalloff = 0.6f;   // contribution of each wider level of the glow
	float flareStrength = 0.05f;
	/// Relativistic view: the scene Doppler shifted per pixel before the
	/// exposure (doppler.frag); off unless set for the frame
	struct Relativity {
		bool on = false;
		float beta[3] = { 0, 0, 0 }; ///< the camera's velocity (view space, units of c)
		float fx = 1.0f, fy = 1.0f;  ///< the projection's focal factors
		float self[4] = { 0, 0, 0, 0 }; ///< the camera's own vessel: centre (view space), radius (0: none)
		VkImageView prepass = VK_NULL_HANDLE; ///< linear view depth in w (the vessel's pixels)
	} rel;
	/// the sun on the screen (texture coordinates of the scene), for the lens flare
	bool sunOnScreen = false;
	float sunUV[2] = { 0.0f, 0.0f };
	bool autoExposure = true;
	/// highest exposure of the automatic exposure: with the camera in
	/// sunlight, sunlit white stays just below full brightness (as a camera
	/// exposed for daylight), so dark scenes (the ocean from orbit) stay dark
	float maxExposure = 4.0f;
	/// lowest exposure, and how far the exposure follows the metered
	/// luminance (log2: 1 fully); by default 0.6 and 0.6, lit white kept
	/// near full brightness and the Sun's disc saturating; near a black
	/// hole's accretion disc, far brighter than the Sun's surface, the eye
	/// adapts further down
	float minExposure = 0.6f, adaptStrength = 0.6f;

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
	// relativistic view: the shifted scene, its pass
	vkc::Image relImg;
	VkDescriptorSet relSet = VK_NULL_HANDLE, prepassSet = VK_NULL_HANDLE;
	VkImageView prepassView = VK_NULL_HANDLE;
	VkPipelineLayout dopplerLayout = VK_NULL_HANDLE;
	VkPipeline dopplerPipe = VK_NULL_HANDLE;
	void Doppler (VkImageView &input, VkExtent2D &inputSize);
	VkDescriptorSet sceneSet = VK_NULL_HANDLE;
	// descriptor sets of input images (scene image, temporal history)
	struct InputSets { VkImageView view; VkDescriptorSet scene, meter; };
	std::vector<InputSets> inputs;
	void SelectInput (VkImageView view);

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

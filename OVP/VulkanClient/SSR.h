// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Screen-space reflections: glossy reflections of the visible scene on
// vessels, bases and water. One ray per reflective pixel (GGX importance
// sampled) is traced through a depth pyramid of the prepass; hits take
// their radiance from the previous frame's image. Neighbouring rays are
// combined (ratio estimator) and accumulated over frames. The scene
// shaders blend the result (frame set) over the environment probe's
// reflections according to its confidence.
// =======================================================================

#ifndef __SSR_H
#define __SSR_H

#include "VkContext.h"
#include <vector>

class ScreenReflections {
public:
	bool Init (vkc::Context *ctx);
	/// Trace with hardware ray queries too (rt.glsl), with these set layouts:
	/// frame, ray tracing scene, bindless textures, atmosphere
	void InitRT (VkDescriptorSetLayout frame, VkDescriptorSetLayout rtScene, VkDescriptorSetLayout rtTex, VkDescriptorSetLayout atm);
	void Shutdown ();

	/// (Re)create the images for the current extent, with the prepass
	/// images as inputs. Returns true if they were recreated (the GPU is
	/// idle then).
	bool EnsureSize (VkImageView prepass, VkImageView material, VkImageView velocity);

	/// Advance to the next history image (before the frame set is bound);
	/// View() then refers to the image this frame's Compute writes, and
	/// Valid() tells whether it will
	void BeginFrame ();
	/// Trace and filter the reflections (outside rendering scopes, after the
	/// prepass). fx, fy: projection factors; jitter: projection jitter (NDC)
	void Compute (float fx, float fy, const float jitter[2], bool reset);
	/// sets of the ray traced variant for the next Compute (frame, scene,
	/// textures, atmosphere)
	void SetRTSets (VkDescriptorSet frame, VkDescriptorSet rtScene, VkDescriptorSet rtTex, VkDescriptorSet atm)
	{ rtSets[0] = frame, rtSets[1] = rtScene, rtSets[2] = rtTex, rtSets[3] = atm; }
	/// engine plumes (plume.glsl): buffer, noise volumes; and the frame slot
	/// of this frame's plumes. Reflected rays add the light of the plumes
	/// they pass.
	void SetPlumes (VkBuffer buf, VkDeviceSize size, VkImageView shapeNoise, VkImageView detailNoise);
	void SetPlumeSlot (uint32_t s) { plumeSlot = s; }
	/// Keep the rendered scene image (shader-read layout) as the source of
	/// the next frame's reflections
	void StoreColor (VkImage scene);

	/// reflections of this frame (general layout)
	VkImageView View () const { return history[cur].view; }
	VkSampler Sampler () const { return nearest; }
	/// the reflections of this frame are valid
	bool Valid () const { return valid; }

	bool enabled = true;
	bool debug = false;      // scene shaders show the reflections alone
	float maxRough = 0.7f;   // reflections fade out up to this roughness
	float thickness = 0.03f; // assumed thickness of surfaces (relative to their depth)

private:
	void CreateImages (VkImageView prepass, VkImageView material, VkImageView velocity);
	void DestroyImages ();
	vkc::Image CreateImage (VkFormat fmt, VkImageUsageFlags usage, uint32_t mips);

	vkc::Context *ctx = nullptr;
	VkExtent2D extent{};
	uint32_t generation = ~0u;
	uint32_t hizLevels = 0, colorLevels = 0;
	void WritePlumes ();
	vkc::Image hiz, hit, col, resolved, history[2], prevColor;
	vkc::Image plumeLight; // plume light along each ray
	VkBuffer plumeBuf = VK_NULL_HANDLE;
	VkDeviceSize plumeBufSize = 0;
	VkImageView plumeNoise[2] = {};
	uint32_t plumeSlot = 0;
	std::vector<VkImageView> hizMipViews;
	int cur = 0;
	bool haveColor = false; // prevColor holds a frame
	bool valid = false, historyValid = false;
	uint32_t frameNo = 0;
	VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE, hizLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet sets[2] = {};     // per history image written
	std::vector<VkDescriptorSet> hizSets;
	VkPipelineLayout layout = VK_NULL_HANDLE, hizPipeLayout = VK_NULL_HANDLE;
	VkPipelineLayout rtLayout = VK_NULL_HANDLE;
	VkPipeline traceRTPipe = VK_NULL_HANDLE;
	VkDescriptorSet rtSets[4] = {};
	VkPipeline hizPipe = VK_NULL_HANDLE, tracePipe = VK_NULL_HANDLE, resolvePipe = VK_NULL_HANDLE, temporalPipe = VK_NULL_HANDLE;
};

#endif // !__SSR_H

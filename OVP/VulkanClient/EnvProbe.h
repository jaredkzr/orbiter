// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Environment probe: a cube map of the surroundings of the focus vessel,
// rendered one face per frame and prefiltered into mip levels of
// increasing roughness, for reflections on vessel surfaces
// =======================================================================

#ifndef __ENVPROBE_H
#define __ENVPROBE_H

#include "VkContext.h"

class EnvProbe {
public:
	static constexpr uint32_t SIZE = 128, LEVELS = 6;

	bool Init (vkc::Context *ctx);
	void Shutdown ();

	/// render target and depth buffer of one face
	vkc::Target FaceTarget () const { return { face.image, face.view, ctx->sceneFormat, { SIZE, SIZE } }; }
	vkc::Target FaceDepth () const { return { depth.image, depth.view, ctx->depthFormat, { SIZE, SIZE } }; }
	/// copy the rendered face into the cube (outside rendering scopes)
	void StoreFace (int f);
	/// recompute the rough levels
	void Prefilter ();

	VkImageView CubeView () const { return cubeView; }
	VkSampler Sampler () const { return sampler; }

private:
	vkc::Context *ctx = nullptr;
	vkc::Image face, depth;
	VkImage cube = VK_NULL_HANDLE;
	VmaAllocation cubeAlloc = VK_NULL_HANDLE;
	VkImageView cubeView = VK_NULL_HANDLE;               // all levels (sampling)
	VkImageView levelCube[LEVELS] = {};                  // single level as cube (filter input)
	VkImageView levelArray[LEVELS] = {};                 // single level as 2D array (filter output)
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet sets[LEVELS] = {};
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	void Barrier (VkCommandBuffer cmd, uint32_t mip, uint32_t nMip, uint32_t layer, uint32_t nLayer,
		VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
		VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
};

#endif // !__ENVPROBE_H

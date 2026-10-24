// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Temporal anti-aliasing: blends the jittered scene image into a history
// reprojected with the prepass motion vectors (ping-pong images)
// =======================================================================

#ifndef __TAA_H
#define __TAA_H

#include "VkContext.h"
#include "VkMath.h"

class TemporalAA {
public:
	bool Init (vkc::Context *ctx);
	void Shutdown ();

	/// Resolve this frame (outside rendering scopes); returns the output
	/// image (shader-read layout). skyMotion: view direction -> clip space
	/// of the previous frame; fx, fy: projection factors; reactive: where
	/// the history is less reliable (plumes, particles: no motion vectors).
	VkImageView Resolve (VkImageView scene, VkImageView prepass, VkImageView velocity, VkImageView reactive,
		const Mat4 &skyMotion, float fx, float fy, bool reset);

	float blend = 0.1f; // weight of the current frame

private:
	void CreateImages (VkImageView scene, VkImageView prepass, VkImageView velocity, VkImageView reactive);
	void DestroyImages ();

	vkc::Context *ctx = nullptr;
	VkExtent2D extent{};
	uint32_t generation = ~0u;
	VkImageView inputs[4] = {};
	vkc::Image history[2];
	int cur = 0;
	bool fresh = true; // history not valid yet
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet sets[2] = {};
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
};

#endif // !__TAA_H

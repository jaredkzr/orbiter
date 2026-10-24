// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "EnvProbe.h"
#include <algorithm>
#include <cmath>

#include "envfilter.comp.h"

void EnvProbe::Barrier (VkCommandBuffer cmd, uint32_t mip, uint32_t nMip, uint32_t layer, uint32_t nLayer,
	VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
	VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
	VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.srcStageMask = srcStage, b.srcAccessMask = srcAccess;
	b.dstStageMask = dstStage, b.dstAccessMask = dstAccess;
	b.oldLayout = from, b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = cube;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, nMip, layer, nLayer };
	VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

bool EnvProbe::Init (vkc::Context *c)
{
	ctx = c;
	auto image2D = [this](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D, ici.format = fmt, ici.extent = { SIZE, SIZE, 1 };
		ici.mipLevels = 1, ici.arrayLayers = 1, ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL, ici.usage = usage;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image, vci.viewType = VK_IMAGE_VIEW_TYPE_2D, vci.format = fmt;
		vci.subresourceRange = { aspect, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		return im;
	};
	face = image2D (ctx->sceneFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	depth = image2D (ctx->depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	ici.imageType = VK_IMAGE_TYPE_2D, ici.format = ctx->sceneFormat, ici.extent = { SIZE, SIZE, 1 };
	ici.mipLevels = LEVELS, ici.arrayLayers = 6, ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &cube, &cubeAlloc, nullptr));
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = cube, vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE, vci.format = ctx->sceneFormat;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 6 };
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &cubeView));
	for (uint32_t m = 0; m < LEVELS; m++) {
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 6 };
		vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &levelCube[m]));
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &levelArray[m]));
	}
	VkImage cb = cube;
	VkImage fi = face.image;
	ctx->ImmediateSubmit ([this, cb, fi](VkCommandBuffer cmd) {
		Barrier (cmd, 0, LEVELS, 0, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkClearColorValue black{{ 0.0f, 0.0f, 0.0f, 1.0f }};
		VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 6 };
		vkCmdClearColorImage (cmd, cb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
		Barrier (cmd, 0, LEVELS, 0, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		vkc::ImageBarrier (cmd, fi, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.maxLod = (float)LEVELS;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &sampler));

	VkDescriptorSetLayoutBinding b[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 2, lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, LEVELS }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, LEVELS } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = LEVELS, pci.poolSizeCount = 2, pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	for (uint32_t m = 1; m < LEVELS; m++) {
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = pool, dai.descriptorSetCount = 1, dai.pSetLayouts = &setLayout;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &sets[m]));
		VkDescriptorImageInfo in{ sampler, levelCube[m-1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo out{ VK_NULL_HANDLE, levelArray[m], VK_IMAGE_LAYOUT_GENERAL };
		VkWriteDescriptorSet wr[2]{};
		for (int k = 0; k < 2; k++) wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, wr[k].dstSet = sets[m], wr[k].dstBinding = k, wr[k].descriptorCount = 1;
		wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[0].pImageInfo = &in;
		wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[1].pImageInfo = &out;
		vkUpdateDescriptorSets (ctx->device, 2, wr, 0, nullptr);
	}
	VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(float) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1, plci.pSetLayouts = &setLayout;
	plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));
	VkShaderModule cs = ctx->CreateShaderModule (spv_envfilter_comp, spv_envfilter_comp_size);
	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
	cpi.layout = layout;
	VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
	vkDestroyShaderModule (ctx->device, cs, nullptr);
	return true;
}

void EnvProbe::Shutdown ()
{
	if (!ctx) return;
	vkDeviceWaitIdle (ctx->device);
	vkDestroyPipeline (ctx->device, pipe, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	vkDestroySampler (ctx->device, sampler, nullptr);
	for (uint32_t m = 0; m < LEVELS; m++) {
		vkDestroyImageView (ctx->device, levelCube[m], nullptr);
		vkDestroyImageView (ctx->device, levelArray[m], nullptr);
	}
	vkDestroyImageView (ctx->device, cubeView, nullptr);
	vmaDestroyImage (ctx->allocator, cube, cubeAlloc);
	ctx->DestroyImage (face);
	ctx->DestroyImage (depth);
	ctx = nullptr;
}

void EnvProbe::StoreFace (int f)
{
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd ();
	vkc::ImageBarrier (cmd, face.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	Barrier (cmd, 0, 1, (uint32_t)f, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkImageCopy r{};
	r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)f, 1 };
	r.extent = { SIZE, SIZE, 1 };
	vkCmdCopyImage (cmd, face.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	Barrier (cmd, 0, 1, (uint32_t)f, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	vkc::ImageBarrier (cmd, face.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

void EnvProbe::Prefilter ()
{
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	for (uint32_t m = 1; m < LEVELS; m++) {
		Barrier (cmd, m, 1, 0, 6, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
		struct { int size; float rough; } p;
		p.size = (int)std::max (1u, SIZE >> m);
		float r1 = (float)m / (LEVELS - 1), r0 = (float)(m - 1) / (LEVELS - 1);
		p.rough = std::sqrt (std::max (r1 * r1 - r0 * r0, 1e-4f));
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[m], 0, nullptr);
		vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
		uint32_t g = ((uint32_t)p.size + 7) / 8;
		vkCmdDispatch (cmd, g, g, 6);
		Barrier (cmd, m, 1, 0, 6, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	}
}

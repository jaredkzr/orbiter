// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "TAA.h"
#include <cstdlib>

#include "taa.comp.h"

namespace {
	struct TAAParams {
		float skyMotion[16];
		float size[2], focal[2];
		float reset, blend, pad[2];
	};
	const VkFormat HISTORY_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
}

bool TemporalAA::Init (vkc::Context *c)
{
	ctx = c;
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &sampler));

	VkDescriptorSetLayoutBinding b[5];
	for (uint32_t i = 0; i < 4; i++) b[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	b[4] = { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 5;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));

	VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TAAParams) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));
	VkShaderModule cs = ctx->CreateShaderModule (spv_taa_comp, spv_taa_comp_size);
	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
	cpi.layout = layout;
	VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
	vkDestroyShaderModule (ctx->device, cs, nullptr);
	if (const char *e = getenv ("ORBITER_VK_TAA_BLEND")) blend = (float)atof (e);
	return true;
}

void TemporalAA::Shutdown ()
{
	if (!ctx) return;
	DestroyImages ();
	vkDestroyPipeline (ctx->device, pipe, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	vkDestroySampler (ctx->device, sampler, nullptr);
	ctx = nullptr;
}

void TemporalAA::CreateImages (VkImageView scene, VkImageView prepass, VkImageView velocity)
{
	extent = ctx->RenderExtent();
	for (auto &h : history) {
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = HISTORY_FORMAT;
		ici.extent = { extent.width, extent.height, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &h.image, &h.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = h.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = HISTORY_FORMAT;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &h.view));
		VkImage img = h.image;
		ctx->ImmediateSubmit ([img](VkCommandBuffer cmd) {
			vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		});
	}
	VkDescriptorSetLayout l[2] = { setLayout, setLayout };
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 2;
	dai.pSetLayouts = l;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, sets));
	for (int i = 0; i < 2; i++) { // set i writes history[i], reads history[1-i]
		VkDescriptorImageInfo ii[5] = {
			{ sampler, scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ sampler, history[1-i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ sampler, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ sampler, prepass, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ VK_NULL_HANDLE, history[i].view, VK_IMAGE_LAYOUT_GENERAL } };
		VkWriteDescriptorSet wr[5]{};
		for (uint32_t k = 0; k < 5; k++) {
			wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			wr[k].dstSet = sets[i];
			wr[k].dstBinding = k;
			wr[k].descriptorCount = 1;
			wr[k].descriptorType = k < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			wr[k].pImageInfo = &ii[k];
		}
		vkUpdateDescriptorSets (ctx->device, 5, wr, 0, nullptr);
	}
	inputs[0] = scene, inputs[1] = prepass, inputs[2] = velocity;
	generation = ctx->SwapchainGeneration();
	fresh = true;
}

void TemporalAA::DestroyImages ()
{
	if (generation == ~0u) return;
	vkDeviceWaitIdle (ctx->device);
	vkResetDescriptorPool (ctx->device, pool, 0);
	for (auto &h : history) ctx->DestroyImage (h);
	generation = ~0u;
}

VkImageView TemporalAA::Resolve (VkImageView scene, VkImageView prepass, VkImageView velocity,
	const Mat4 &skyMotion, float fx, float fy, bool reset)
{
	if (generation != ctx->SwapchainGeneration() || inputs[0] != scene || inputs[1] != prepass || inputs[2] != velocity) {
		DestroyImages ();
		CreateImages (scene, prepass, velocity);
	}
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd();
	cur = 1 - cur;
	vkc::Image &out = history[cur];
	vkc::ImageBarrier (cmd, out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
	TAAParams p{};
	skyMotion.ToFloat (p.skyMotion);
	p.size[0] = (float)extent.width, p.size[1] = (float)extent.height;
	p.focal[0] = fx, p.focal[1] = fy;
	p.reset = (reset || fresh) ? 1.0f : 0.0f;
	p.blend = blend;
	fresh = false;
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[cur], 0, nullptr);
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
	vkCmdDispatch (cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
	vkc::ImageBarrier (cmd, out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	return out.view;
}

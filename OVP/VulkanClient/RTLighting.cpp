// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "RTLighting.h"
#include "OrbiterAPI.h"
#include <cstdlib>
#include <vector>

#include "rtlight_trace.comp.h"
#include "rtlight_temporal.comp.h"
#include "rtlight_filter.comp.h"

namespace {
	struct Params { // must match rtlight.glsl
		float focal[2], size[2], jitter[2];
		float frame, reset, aoRadius, aoMaxDist, sunRadius, nLights;
	};
	constexpr VkAccessFlags2 RW = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;

	void MemoryBarrier (VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
		VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
	{
		VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		b.srcStageMask = srcStage;
		b.srcAccessMask = srcAccess;
		b.dstStageMask = dstStage;
		b.dstAccessMask = dstAccess;
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.memoryBarrierCount = 1;
		dep.pMemoryBarriers = &b;
		vkCmdPipelineBarrier2 (cmd, &dep);
	}
}

bool RTLighting::Init (vkc::Context *c, VkDescriptorSetLayout frame, VkDescriptorSetLayout rtScene, VkDescriptorSetLayout rtTex)
{
	if (const char *e = getenv ("ORBITER_VK_RT_LIGHTING")) if (!atoi (e)) return false;
	ctx = c;
	if (const char *e = getenv ("ORBITER_VK_AO_RADIUS")) aoRadius = (float)atof (e);

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &nearest));

	const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, STO = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	const VkDescriptorType types[11] = { CIS, CIS, STO, STO, CIS, CIS, STO, STO, STO, STO, CIS };
	VkDescriptorSetLayoutBinding b[11];
	for (uint32_t i = 0; i < 11; i++) b[i] = { i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 11;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));
	VkDescriptorPoolSize ps[2] = { { CIS, 10 }, { STO, 12 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));

	VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params) };
	VkDescriptorSetLayout tl[4] = { setLayout, frame, rtScene, rtTex };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 4;
	plci.pSetLayouts = tl;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &traceLayout));
	plci.setLayoutCount = 1;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));
	auto compute = [this](const unsigned char *code, size_t size, VkPipelineLayout l) {
		VkShaderModule cs = ctx->CreateShaderModule (code, size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = l;
		VkPipeline pipe = VK_NULL_HANDLE;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
		return pipe;
	};
	tracePipe = compute (spv_rtlight_trace_comp, spv_rtlight_trace_comp_size, traceLayout);
	temporalPipe = compute (spv_rtlight_temporal_comp, spv_rtlight_temporal_comp_size, layout);
	filterPipe = compute (spv_rtlight_filter_comp, spv_rtlight_filter_comp_size, layout);
	oapiWriteLog ("VulkanClient: ray traced shadows, ambient occlusion and planet light");
	return true;
}

void RTLighting::Shutdown ()
{
	if (!ctx) return;
	DestroyImages ();
	for (VkPipeline p : { tracePipe, temporalPipe, filterPipe }) vkDestroyPipeline (ctx->device, p, nullptr);
	vkDestroyPipelineLayout (ctx->device, traceLayout, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	vkDestroySampler (ctx->device, nearest, nullptr);
	ctx = nullptr;
}

vkc::Image RTLighting::CreateImage (VkFormat fmt)
{
	vkc::Image img;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = { extent.width, extent.height, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &img.image, &img.alloc, nullptr));
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = img.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = fmt;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &img.view));
	return img;
}

void RTLighting::CreateImages (VkImageView prepass, VkImageView velocity, VkImageView material)
{
	extent = ctx->RenderExtent();
	rawA = CreateImage (VIS_FORMAT);
	rawL = CreateImage (LIGHT_FORMAT);
	for (int i = 0; i < 2; i++) accA[i] = CreateImage (VIS_FORMAT), accL[i] = CreateImage (LIGHT_FORMAT);
	outA = CreateImage (VIS_FORMAT);
	outL = CreateImage (LIGHT_FORMAT);
	// all in general layout; the results start fully visible
	std::vector<VkImage> all = { rawA.image, rawL.image, accA[0].image, accA[1].image, accL[0].image, accL[1].image, outA.image, outL.image };
	ctx->ImmediateSubmit ([&all](VkCommandBuffer cmd) {
		VkClearColorValue one{{ 1.0f, 1.0f, 1.0f, 1.0f }};
		VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		for (VkImage im : all) {
			vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			vkCmdClearColorImage (cmd, im, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &r);
			vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, RW);
		}
	});

	VkDescriptorSetLayout l[2] = { setLayout, setLayout };
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 2;
	dai.pSetLayouts = l;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, sets));
	const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GEN = VK_IMAGE_LAYOUT_GENERAL;
	std::vector<VkDescriptorImageInfo> infos;
	infos.reserve (32);
	std::vector<VkWriteDescriptorSet> wr;
	auto add = [&](VkDescriptorSet s, uint32_t binding, bool sampled, VkImageView v, VkImageLayout lay) {
		infos.push_back ({ sampled ? nearest : VK_NULL_HANDLE, v, lay });
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = s;
		w.dstBinding = binding;
		w.descriptorCount = 1;
		w.descriptorType = sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		w.pImageInfo = &infos.back();
		wr.push_back (w);
	};
	for (int k = 0; k < 2; k++) { // set k accumulates into acc[k] from acc[1-k]
		add (sets[k], 0, true, prepass, RO);
		add (sets[k], 1, true, velocity, RO);
		add (sets[k], 2, false, rawA.view, GEN);
		add (sets[k], 3, false, rawL.view, GEN);
		add (sets[k], 4, true, accA[1 - k].view, GEN);
		add (sets[k], 5, true, accL[1 - k].view, GEN);
		add (sets[k], 6, false, accA[k].view, GEN);
		add (sets[k], 7, false, accL[k].view, GEN);
		add (sets[k], 8, false, outA.view, GEN);
		add (sets[k], 9, false, outL.view, GEN);
		add (sets[k], 10, true, material, RO);
	}
	vkUpdateDescriptorSets (ctx->device, (uint32_t)wr.size(), wr.data(), 0, nullptr);
	historyValid = false;
	generation = ctx->SwapchainGeneration();
}

void RTLighting::DestroyImages ()
{
	if (generation == ~0u) return;
	vkDeviceWaitIdle (ctx->device);
	vkResetDescriptorPool (ctx->device, pool, 0);
	for (vkc::Image *im : { &rawA, &rawL, &accA[0], &accA[1], &accL[0], &accL[1], &outA, &outL }) ctx->DestroyImage (*im);
	generation = ~0u;
}

bool RTLighting::EnsureSize (VkImageView prepass, VkImageView velocity, VkImageView material)
{
	if (!ctx || generation == ctx->SwapchainGeneration()) return false;
	DestroyImages ();
	CreateImages (prepass, velocity, material);
	return true;
}

void RTLighting::Compute (VkDescriptorSet frameSet, VkDescriptorSet rtSet, VkDescriptorSet texSet, float fx, float fy, const float jitter[2],
	float sunRadius, bool reset)
{
	if (!ctx) return;
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd();
	cur = 1 - cur;
	Params p{ { fx, fy }, { (float)extent.width, (float)extent.height }, { jitter[0], jitter[1] },
		(float)(frameNo++ & 0xffff), (reset || !historyValid) ? 1.0f : 0.0f, aoRadius, aoMaxDist, sunRadius, (float)SHADOWED_LIGHTS };
	historyValid = true;
	const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	// the previous frame's passes and scene shaders are done with the images
	MemoryBarrier (cmd, CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, RW, CS, RW);
	uint32_t gx = (extent.width + 7) / 8, gy = (extent.height + 7) / 8;
	// (set 0 and the push constants are compatible between both layouts)
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, traceLayout, 0, 1, &sets[cur], 0, nullptr);
	VkDescriptorSet s123[3] = { frameSet, rtSet, texSet };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, traceLayout, 1, 3, s123, 0, nullptr);
	for (VkPipeline pipe : { tracePipe, temporalPipe, filterPipe }) {
		VkPipelineLayout l = pipe == tracePipe ? traceLayout : layout;
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdPushConstants (cmd, l, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
		vkCmdDispatch (cmd, gx, gy, 1);
		MemoryBarrier (cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		ctx->GpuMark (pipe == tracePipe ? "rtl trace" : pipe == temporalPipe ? "rtl temporal" : "rtl filter");
	}
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "SSR.h"
#include <algorithm>
#include <cstdlib>

#include "ssr_hiz.comp.h"
#include "ssr_trace.comp.h"
#include "ssr_trace_rt.comp.h"
#include "ssr_resolve.comp.h"
#include "ssr_temporal.comp.h"

namespace {
	struct SSRParams { // must match ssr.glsl
		float focal[2], size[2], jitter[2];
		float frame, levels, thickness, maxRough, reset, pad;
	};
	constexpr VkFormat HIZ_FORMAT = VK_FORMAT_R32_SFLOAT;
	constexpr VkFormat HIT_FORMAT = VK_FORMAT_R32G32B32A32_SFLOAT;
	constexpr VkFormat COL_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
	constexpr uint32_t MAX_HIZ_LEVELS = 16;
	constexpr uint32_t MAX_COLOR_LEVELS = 7;

	void Barrier (VkCommandBuffer cmd, VkImage image, uint32_t mip, uint32_t nMip,
		VkImageLayout from, VkImageLayout to,
		VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
		VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
	{
		VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		b.srcStageMask = srcStage;
		b.srcAccessMask = srcAccess;
		b.dstStageMask = dstStage;
		b.dstAccessMask = dstAccess;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, nMip, 0, 1 };
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers = &b;
		vkCmdPipelineBarrier2 (cmd, &dep);
	}

	// images in general layout: execution and memory dependency only
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

	constexpr VkAccessFlags2 RW = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
}

bool ScreenReflections::Init (vkc::Context *c)
{
	ctx = c;
	if (const char *e = getenv ("ORBITER_VK_SSR")) enabled = atoi (e) != 0;
	if (const char *e = getenv ("ORBITER_VK_SSR_DEBUG")) debug = atoi (e) != 0;
	if (ctx->sceneSamples != VK_SAMPLE_COUNT_1_BIT) enabled = false; // (the scene image is resolved)

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.maxLod = VK_LOD_CLAMP_NONE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &nearest));
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &linear));

	// trace, resolve and temporal passes
	VkDescriptorSetLayoutBinding b[10];
	const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, STO = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	const VkDescriptorType types[10] = { CIS, CIS, CIS, CIS, CIS, STO, STO, STO, CIS, STO };
	for (uint32_t i = 0; i < 10; i++) b[i] = { i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 10;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));
	// depth pyramid levels
	VkDescriptorSetLayoutBinding hb[3] = {
		{ 0, CIS, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 1, STO, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 2, STO, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
	};
	lci.bindingCount = 3;
	lci.pBindings = hb;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &hizLayout));

	VkDescriptorPoolSize ps[2] = { { CIS, 2 * 6 + MAX_HIZ_LEVELS }, { STO, 2 * 4 + 2 * MAX_HIZ_LEVELS } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2 + MAX_HIZ_LEVELS;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));

	auto pipeLayout = [this](VkDescriptorSetLayout sl, uint32_t pcSize) {
		VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 1;
		plci.pSetLayouts = &sl;
		plci.pushConstantRangeCount = 1;
		plci.pPushConstantRanges = &pcr;
		VkPipelineLayout l = VK_NULL_HANDLE;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &l));
		return l;
	};
	layout = pipeLayout (setLayout, sizeof(SSRParams));
	hizPipeLayout = pipeLayout (hizLayout, sizeof(int32_t));
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
	hizPipe = compute (spv_ssr_hiz_comp, spv_ssr_hiz_comp_size, hizPipeLayout);
	tracePipe = compute (spv_ssr_trace_comp, spv_ssr_trace_comp_size, layout);
	resolvePipe = compute (spv_ssr_resolve_comp, spv_ssr_resolve_comp_size, layout);
	temporalPipe = compute (spv_ssr_temporal_comp, spv_ssr_temporal_comp_size, layout);
	return true;
}

void ScreenReflections::InitRT (VkDescriptorSetLayout frame, VkDescriptorSetLayout rtScene, VkDescriptorSetLayout rtTex, VkDescriptorSetLayout atm)
{
	VkDescriptorSetLayout sl[5] = { setLayout, frame, rtScene, rtTex, atm };
	VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SSRParams) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 5;
	plci.pSetLayouts = sl;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &rtLayout));
	VkShaderModule cs = ctx->CreateShaderModule (spv_ssr_trace_rt_comp, spv_ssr_trace_rt_comp_size);
	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
	cpi.layout = rtLayout;
	VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &traceRTPipe));
	vkDestroyShaderModule (ctx->device, cs, nullptr);
}

void ScreenReflections::Shutdown ()
{
	if (!ctx) return;
	DestroyImages ();
	if (traceRTPipe) vkDestroyPipeline (ctx->device, traceRTPipe, nullptr);
	if (rtLayout) vkDestroyPipelineLayout (ctx->device, rtLayout, nullptr);
	for (VkPipeline p : { hizPipe, tracePipe, resolvePipe, temporalPipe }) vkDestroyPipeline (ctx->device, p, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	vkDestroyPipelineLayout (ctx->device, hizPipeLayout, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, hizLayout, nullptr);
	vkDestroySampler (ctx->device, nearest, nullptr);
	vkDestroySampler (ctx->device, linear, nullptr);
	ctx = nullptr;
}

vkc::Image ScreenReflections::CreateImage (VkFormat fmt, VkImageUsageFlags usage, uint32_t mips)
{
	vkc::Image img;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = { extent.width, extent.height, 1 };
	ici.mipLevels = mips;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &img.image, &img.alloc, nullptr));
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = img.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = fmt;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &img.view));
	return img;
}

void ScreenReflections::CreateImages (VkImageView prepass, VkImageView material, VkImageView velocity)
{
	extent = ctx->RenderExtent();
	uint32_t m = std::max (extent.width, extent.height), levels = 1;
	while (m > 1) m /= 2, levels++;
	hizLevels = std::min (levels, MAX_HIZ_LEVELS);
	colorLevels = std::min (levels, MAX_COLOR_LEVELS);
	const VkImageUsageFlags SU = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	hiz = CreateImage (HIZ_FORMAT, SU, hizLevels);
	hit = CreateImage (HIT_FORMAT, SU, 1);
	col = CreateImage (COL_FORMAT, SU, 1);
	resolved = CreateImage (COL_FORMAT, SU, 1);
	for (auto &h : history) h = CreateImage (COL_FORMAT, SU | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1);
	prevColor = CreateImage (ctx->sceneFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, colorLevels);
	hizMipViews.resize (hizLevels);
	for (uint32_t i = 0; i < hizLevels; i++) {
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = hiz.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = HIZ_FORMAT;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &hizMipViews[i]));
	}

	// storage images live in general layout; the reflections start empty,
	// the previous colour image rests in shader-read layout
	std::vector<VkImage> gen = { hiz.image, hit.image, col.image, resolved.image };
	VkImage h0 = history[0].image, h1 = history[1].image, pc = prevColor.image;
	ctx->ImmediateSubmit ([&gen, h0, h1, pc](VkCommandBuffer cmd) {
		for (VkImage im : gen)
			Barrier (cmd, im, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, RW);
		VkClearColorValue zero{};
		VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		for (VkImage im : { h0, h1 }) {
			Barrier (cmd, im, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			vkCmdClearColorImage (cmd, im, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
			Barrier (cmd, im, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, RW);
		}
		Barrier (cmd, pc, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});

	// descriptor sets
	std::vector<VkDescriptorSetLayout> l (2 + hizLevels, hizLayout);
	l[0] = l[1] = setLayout;
	std::vector<VkDescriptorSet> all (l.size());
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = (uint32_t)l.size();
	dai.pSetLayouts = l.data();
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, all.data()));
	sets[0] = all[0], sets[1] = all[1];
	hizSets.assign (all.begin() + 2, all.end());

	const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GEN = VK_IMAGE_LAYOUT_GENERAL;
	std::vector<VkDescriptorImageInfo> infos;
	std::vector<VkWriteDescriptorSet> wr;
	infos.reserve (64);
	auto add = [&](VkDescriptorSet s, uint32_t binding, VkSampler smp, VkImageView v, VkImageLayout lay) {
		infos.push_back ({ smp, v, lay });
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = s;
		w.dstBinding = binding;
		w.descriptorCount = 1;
		w.descriptorType = smp ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		w.pImageInfo = &infos.back();
		wr.push_back (w);
	};
	for (int k = 0; k < 2; k++) {
		add (sets[k], 0, nearest, prepass, RO);
		add (sets[k], 1, nearest, material, RO);
		add (sets[k], 2, nearest, hiz.view, GEN);
		add (sets[k], 3, nearest, velocity, RO);
		add (sets[k], 4, linear, prevColor.view, RO);
		add (sets[k], 5, VK_NULL_HANDLE, hit.view, GEN);
		add (sets[k], 6, VK_NULL_HANDLE, col.view, GEN);
		add (sets[k], 7, VK_NULL_HANDLE, resolved.view, GEN);
		add (sets[k], 8, linear, history[1 - k].view, GEN);
		add (sets[k], 9, VK_NULL_HANDLE, history[k].view, GEN);
	}
	for (uint32_t i = 0; i < hizLevels; i++) {
		add (hizSets[i], 0, nearest, prepass, RO);
		add (hizSets[i], 1, VK_NULL_HANDLE, hizMipViews[i ? i - 1 : 0], GEN);
		add (hizSets[i], 2, VK_NULL_HANDLE, hizMipViews[i], GEN);
	}
	vkUpdateDescriptorSets (ctx->device, (uint32_t)wr.size(), wr.data(), 0, nullptr);
	haveColor = false;
	generation = ctx->SwapchainGeneration();
}

void ScreenReflections::DestroyImages ()
{
	if (generation == ~0u) return;
	vkDeviceWaitIdle (ctx->device);
	vkResetDescriptorPool (ctx->device, pool, 0);
	for (VkImageView v : hizMipViews) vkDestroyImageView (ctx->device, v, nullptr);
	hizMipViews.clear ();
	for (vkc::Image *im : { &hiz, &hit, &col, &resolved, &history[0], &history[1], &prevColor }) ctx->DestroyImage (*im);
	generation = ~0u;
}

bool ScreenReflections::EnsureSize (VkImageView prepass, VkImageView material, VkImageView velocity)
{
	if (generation == ctx->SwapchainGeneration()) return false;
	DestroyImages ();
	CreateImages (prepass, material, velocity);
	return true;
}

void ScreenReflections::BeginFrame ()
{
	cur = 1 - cur;
	historyValid = valid;
	valid = enabled && haveColor;
}

void ScreenReflections::Compute (float fx, float fy, const float jitter[2], bool reset)
{
	if (!valid) return;
	VkCommandBuffer cmd = ctx->Cmd();
	SSRParams p{ { fx, fy }, { (float)extent.width, (float)extent.height }, { jitter[0], jitter[1] },
		(float)(frameNo++ & 0xffff), (float)hizLevels, thickness, maxRough, (reset || !historyValid) ? 1.0f : 0.0f, 0.0f };
	const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	// the previous frame's passes and scene shaders are done with the images
	MemoryBarrier (cmd, CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, RW, CS, RW);

	// depth pyramid
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hizPipe);
	for (uint32_t i = 0; i < hizLevels; i++) {
		uint32_t w = std::max (1u, extent.width >> i), h = std::max (1u, extent.height >> i);
		int32_t level = (int32_t)i;
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hizPipeLayout, 0, 1, &hizSets[i], 0, nullptr);
		vkCmdPushConstants (cmd, hizPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(level), &level);
		vkCmdDispatch (cmd, (w + 7) / 8, (h + 7) / 8, 1);
		MemoryBarrier (cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_READ_BIT);
	}

	uint32_t gx = (extent.width + 7) / 8, gy = (extent.height + 7) / 8;
	bool rt = traceRTPipe && rtSets[0] && rtSets[1] && rtSets[2] && rtSets[3];
	// (set 0 and the push constants are compatible between both layouts)
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[cur], 0, nullptr);
	if (rt) vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rtLayout, 1, 4, rtSets, 0, nullptr);
	for (VkPipeline pipe : { rt ? traceRTPipe : tracePipe, resolvePipe, temporalPipe }) {
		VkPipelineLayout l = pipe == traceRTPipe ? rtLayout : layout;
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdPushConstants (cmd, l, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
		vkCmdDispatch (cmd, gx, gy, 1);
		MemoryBarrier (cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	}
	for (auto &s : rtSets) s = VK_NULL_HANDLE;
}

void ScreenReflections::StoreColor (VkImage scene)
{
	if (!enabled) return;
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd();
	vkc::ImageBarrier (cmd, scene, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	Barrier (cmd, prevColor.image, 0, colorLevels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkImageCopy cp{};
	cp.srcSubresource = cp.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	cp.extent = { extent.width, extent.height, 1 };
	vkCmdCopyImage (cmd, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prevColor.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
	vkc::ImageBarrier (cmd, scene, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	// mipmaps (glossy reflections of distant hits read coarser levels)
	int32_t w = (int32_t)extent.width, h = (int32_t)extent.height;
	for (uint32_t i = 1; i < colorLevels; i++) {
		Barrier (cmd, prevColor.image, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		int32_t w2 = std::max (1, w / 2), h2 = std::max (1, h / 2);
		VkImageBlit bl{};
		bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 };
		bl.srcOffsets[1] = { w, h, 1 };
		bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
		bl.dstOffsets[1] = { w2, h2, 1 };
		vkCmdBlitImage (cmd, prevColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prevColor.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
		w = w2, h = h2;
	}
	if (colorLevels > 1)
		Barrier (cmd, prevColor.image, 0, colorLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	Barrier (cmd, prevColor.image, colorLevels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	haveColor = true;
}

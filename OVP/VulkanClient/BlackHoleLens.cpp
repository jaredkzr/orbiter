// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The sky seen near a black hole (BlackHoleLens.h)

#include "BlackHoleLens.h"
#include "StarField.h"
#include "../../Src/Orbiter/Kerr.h"
#include <cmath>
#include <cstdio>
#include "OrbiterAPI.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "lens_trace.comp.h"
#include "milkyway.vert.h"
#include "lenssky.frag.h"
#include "lens_stars.comp.h"

namespace {
	struct Params {
		float cam[4];
		float e[4][4];
		float v2k[3][4];
		float k2g[3][4];
		float proj[4];
		float size[4];
		float lim[4];
		float inner[4];
		float disc[4];
		float roots[4];
	};
}

bool BlackHoleLens::Init (vkc::Context *c, GalaxySky *s, StarField *sf)
{
	ctx = c, sky = s, starField = sf && sf->Ready () ? sf : nullptr;
	if (!sky || !sky->Ready ()) return false;
	if (const char *e = getenv ("ORBITER_VK_LENS_GRID")) grid = std::clamp (atoi (e), 1, 16);
	params = ctx->CreateBuffer (sizeof (Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	counts = ctx->CreateBuffer (STAR_NCELL * 4, su, false);
	offsets = ctx->CreateBuffer ((STAR_NCELL + 1) * 4, su, false);
	scratch = ctx->CreateBuffer ((VkDeviceSize)STAR_CAP * 16, su, false);
	records = ctx->CreateBuffer ((VkDeviceSize)STAR_CAP * 16, su, false);
	ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
		for (vkc::Buffer *b : { &counts, &offsets }) vkCmdFillBuffer (cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
	});
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &sampler));
	CreatePipelines ();
	ready = true;
	return true;
}

void BlackHoleLens::Shutdown ()
{
	if (!ctx) return;
	vkDeviceWaitIdle (ctx->device);
	if (mapView) vkDestroyImageView (ctx->device, mapView, nullptr);
	if (map) vmaDestroyImage (ctx->allocator, map, mapAlloc);
	mapView = VK_NULL_HANDLE, map = VK_NULL_HANDLE;
	ctx->DestroyBuffer (params);
	for (vkc::Buffer *b : { &counts, &offsets, &scratch, &records }) ctx->DestroyBuffer (*b);
	if (starPipe) vkDestroyPipeline (ctx->device, starPipe, nullptr);
	if (starPL) vkDestroyPipelineLayout (ctx->device, starPL, nullptr);
	if (starLayout) vkDestroyDescriptorSetLayout (ctx->device, starLayout, nullptr);
	starPipe = VK_NULL_HANDLE, starPL = VK_NULL_HANDLE, starLayout = VK_NULL_HANDLE;
	for (VkPipeline p : { tracePipe, drawPipe }) if (p) vkDestroyPipeline (ctx->device, p, nullptr);
	for (VkPipelineLayout l : { tracePL, drawPL }) if (l) vkDestroyPipelineLayout (ctx->device, l, nullptr);
	if (pool) vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	for (VkDescriptorSetLayout l : { traceLayout, drawLayout }) if (l) vkDestroyDescriptorSetLayout (ctx->device, l, nullptr);
	if (sampler) vkDestroySampler (ctx->device, sampler, nullptr);
	tracePipe = drawPipe = VK_NULL_HANDLE, tracePL = drawPL = VK_NULL_HANDLE;
	pool = VK_NULL_HANDLE, traceLayout = drawLayout = VK_NULL_HANDLE, sampler = VK_NULL_HANDLE;
	ready = traced = false;
	ctx = nullptr;
}

void BlackHoleLens::CreatePipelines ()
{
	{
		VkDescriptorSetLayoutBinding b[2] = {
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = 2, lci.pBindings = b;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &traceLayout));
	}
	{
		VkDescriptorSetLayoutBinding b[3] = {
			{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
			{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr } };
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = 3, lci.pBindings = b;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &drawLayout));
	}
	{
		VkDescriptorSetLayoutBinding b[4];
		for (uint32_t i = 0; i < 4; i++) b[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = 4, lci.pBindings = b;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &starLayout));
	}
	VkDescriptorPoolSize ps[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 3, pci.poolSizeCount = 4, pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool, dai.descriptorSetCount = 1;
	dai.pSetLayouts = &traceLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &traceSet));
	dai.pSetLayouts = &drawLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &drawSet));
	dai.pSetLayouts = &starLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &starSet));
	{
		VkDescriptorBufferInfo bi[4] = { { counts.buffer, 0, VK_WHOLE_SIZE }, { offsets.buffer, 0, VK_WHOLE_SIZE },
			{ scratch.buffer, 0, VK_WHOLE_SIZE }, { records.buffer, 0, VK_WHOLE_SIZE } };
		VkWriteDescriptorSet wr[6]{};
		for (int i = 0; i < 6; i++) {
			wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, wr[i].descriptorCount = 1;
			wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		}
		for (int i = 0; i < 4; i++) wr[i].dstSet = starSet, wr[i].dstBinding = i, wr[i].pBufferInfo = &bi[i];
		wr[4].dstSet = drawSet, wr[4].dstBinding = 1, wr[4].pBufferInfo = &bi[1];
		wr[5].dstSet = drawSet, wr[5].dstBinding = 2, wr[5].pBufferInfo = &bi[3];
		vkUpdateDescriptorSets (ctx->device, 6, wr, 0, nullptr);
	}

	// the stars' bins
	if (starField) {
		VkDescriptorSetLayout sets[2] = { starField->DrawLayout (), starLayout };
		VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 * sizeof(float) };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 2, plci.pSetLayouts = sets;
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &starPL));
		VkShaderModule cs = ctx->CreateShaderModule (spv_lens_stars_comp, spv_lens_stars_comp_size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = starPL;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &starPipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
	}

	// trace
	{
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 1, plci.pSetLayouts = &traceLayout;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &tracePL));
		VkShaderModule cs = ctx->CreateShaderModule (spv_lens_trace_comp, spv_lens_trace_comp_size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = tracePL;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &tracePipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
	}
	// draw: full-screen triangle at infinity, added to the scene (as the Milky Way's)
	{
		VkDescriptorSetLayout sets[2] = { sky->DrawLayout (), drawLayout };
		VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 28 * sizeof(float) };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 2, plci.pSetLayouts = sets;
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &drawPL));
		VkShaderModule vs = ctx->CreateShaderModule (spv_milkyway_vert, spv_milkyway_vert_size);
		VkShaderModule fs = ctx->CreateShaderModule (spv_lenssky_frag, spv_lenssky_frag_size);
		VkPipelineShaderStageCreateInfo stages[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" } };
		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vps.viewportCount = 1, vps.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = ctx->sceneSamples;
		VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		VkPipelineColorBlendAttachmentState cba{};
		cba.colorWriteMask = 0xF;
		cba.blendEnable = VK_TRUE;
		cba.srcColorBlendFactor = cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.colorBlendOp = VK_BLEND_OP_ADD;
		cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.alphaBlendOp = VK_BLEND_OP_ADD;
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1, cb.pAttachments = &cba;
		VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
		VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		rci.colorAttachmentCount = 1;
		rci.pColorAttachmentFormats = &ctx->sceneFormat;
		rci.depthAttachmentFormat = ctx->depthFormat;
		VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gpi.pNext = &rci;
		gpi.stageCount = 2, gpi.pStages = stages;
		gpi.pVertexInputState = &vi, gpi.pInputAssemblyState = &ia, gpi.pViewportState = &vps;
		gpi.pRasterizationState = &rs, gpi.pMultisampleState = &ms, gpi.pDepthStencilState = &ds;
		gpi.pColorBlendState = &cb, gpi.pDynamicState = &dy;
		gpi.layout = drawPL;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &drawPipe));
		vkDestroyShaderModule (ctx->device, vs, nullptr);
		vkDestroyShaderModule (ctx->device, fs, nullptr);
	}
}

void BlackHoleLens::EnsureMap (uint32_t w, uint32_t h)
{
	uint32_t mw = w / grid + 2, mh = h / grid + 2;
	if (map && mw == mapW && mh == mapH) return;
	if (map) {
		vkDeviceWaitIdle (ctx->device); // (the sets are rewritten: rare, the image resized)
		vkDestroyImageView (ctx->device, mapView, nullptr);
		vmaDestroyImage (ctx->allocator, map, mapAlloc);
	}
	mapW = mw, mapH = mh, imgW = w, imgH = h;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D, ici.format = VK_FORMAT_R32G32B32A32_SFLOAT, ici.extent = { mapW, mapH, 1 };
	ici.mipLevels = 1, ici.arrayLayers = 1, ici.samples = VK_SAMPLE_COUNT_1_BIT, ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &map, &mapAlloc, nullptr));
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = map, vci.viewType = VK_IMAGE_VIEW_TYPE_2D, vci.format = ici.format;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &mapView));
	mapLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkDescriptorImageInfo si{ VK_NULL_HANDLE, mapView, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorBufferInfo bi{ params.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorImageInfo di{ sampler, mapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet wr[3]{};
	for (auto &x : wr) x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, x.descriptorCount = 1;
	wr[0].dstSet = traceSet, wr[0].dstBinding = 0, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[0].pImageInfo = &si;
	wr[1].dstSet = traceSet, wr[1].dstBinding = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, wr[1].pBufferInfo = &bi;
	wr[2].dstSet = drawSet, wr[2].dstBinding = 0, wr[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[2].pImageInfo = &di;
	vkUpdateDescriptorSets (ctx->device, 3, wr, 0, nullptr);
}

void BlackHoleLens::Trace (const Frame &f)
{
	traced = false;
	if (!ready) return;
	// (a check of the last frame's trace, once, after the view settled)
	static const int checkAt = getenv ("ORBITER_VK_LENS_CHECK") ? std::max (atoi (getenv ("ORBITER_VK_LENS_CHECK")), 2) : 0;
	if (checkAt && ++checkFrame == checkAt && map) Check ();
	lastFrame = f;
	VkExtent2D ext = ctx->RenderExtent ();
	EnsureMap (ext.width, ext.height);
	cosmic = f.cosmic;
	for (int i = 0; i < 3; i++) betaBg[i] = f.betaBg[i];

	Params p{};
	for (int i = 0; i < 3; i++) p.cam[i] = (float)f.cam[i];
	p.cam[3] = (float)f.a;
	for (int k = 0; k < 4; k++) {
		for (int i = 0; i < 3; i++) p.e[k][i] = (float)f.e[k][1 + i];
		p.e[k][3] = (float)f.e[k][0];
	}
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) p.v2k[i][j] = (float)f.v2k[i][j], p.k2g[i][j] = (float)f.k2g[i][j];
	p.proj[0] = (float)(1.0 / f.fx), p.proj[1] = (float)(1.0 / f.fy), p.proj[2] = (float)grid, p.proj[3] = (float)imgW;
	p.size[0] = (float)imgH, p.size[1] = (float)mapW, p.size[2] = (float)mapH, p.size[3] = (float)f.rh;
	double rc = std::sqrt (f.cam[0] * f.cam[0] + f.cam[1] * f.cam[1] + f.cam[2] * f.cam[2]);
	static const int maxSteps = getenv ("ORBITER_VK_LENS_STEPS") ? atoi (getenv ("ORBITER_VK_LENS_STEPS")) : MAX_STEPS;
	static const double step = getenv ("ORBITER_VK_LENS_STEP") ? atof (getenv ("ORBITER_VK_LENS_STEP")) : STEP;
	// (a disc: rays traced through all of it)
	double rin = std::max (R_IN, f.discTK > 0.0 ? 1.05 * f.discOut : 0.0), rout = std::max (R_OUT, f.discTK > 0.0 ? 1.05 * f.discOut : 0.0);
	p.lim[0] = (float)rin, p.lim[1] = (float)std::max (rout, 1.05 * rc), p.lim[2] = (float)maxSteps, p.lim[3] = (float)step;
	p.inner[0] = (float)std::max (f.rm, 0.02), p.inner[1] = f.inside ? 1.0f : 0.0f, p.inner[2] = (float)f.rph;
	p.inner[3] = (float)(frame++ % 64);
	p.disc[0] = (float)f.discTK, p.disc[1] = (float)f.discIn, p.disc[2] = (float)f.discOut;
	p.disc[3] = (float)std::fmod (f.time, 1e6); // (the turbulence's phases repeat long before)
	{
		const double PI = 3.14159265358979323846;
		double ac = std::acos (std::clamp (f.a, -1.0, 1.0));
		p.roots[0] = (float)std::sqrt (f.discIn), p.roots[1] = (float)(2.0 * std::cos ((ac - PI) / 3.0));
		p.roots[2] = (float)(2.0 * std::cos ((ac + PI) / 3.0)), p.roots[3] = (float)(-2.0 * std::cos (ac / 3.0));
	}

	VkCommandBuffer cmd = ctx->Cmd ();
	// (the parameters: after the last trace read them)
	VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
	bb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, bb.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
	bb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, bb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bb.buffer = params.buffer, bb.size = VK_WHOLE_SIZE;
	VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dep.bufferMemoryBarrierCount = 1, dep.pBufferMemoryBarriers = &bb;
	vkCmdPipelineBarrier2 (cmd, &dep);
	vkCmdUpdateBuffer (cmd, params.buffer, 0, sizeof (p), &p);
	bb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	bb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, bb.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
	vkCmdPipelineBarrier2 (cmd, &dep);
	// (the map: after the last frame's sky read it)
	vkc::ImageBarrier (cmd, map, VK_IMAGE_ASPECT_COLOR_BIT, mapLayout, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tracePL, 0, 1, &traceSet, 0, nullptr);
	vkCmdDispatch (cmd, (mapW + 7) / 8, (mapH + 7) / 8, 1);
	vkc::ImageBarrier (cmd, map, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	mapLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	stars = f.stars && starField && starPipe;
	if (stars) BinStars (cmd);
	traced = true;
}

void BlackHoleLens::Check ()
{
	// the map of the last frame (its commands done: this waits for them)
	VkDeviceSize bytes = (VkDeviceSize)mapW * mapH * 16;
	vkc::Buffer rb = ctx->CreateReadbackBuffer (bytes);
	ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, map, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy r{};
		r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		r.imageExtent = { mapW, mapH, 1 };
		vkCmdCopyImageToBuffer (cmd, map, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &r);
		vkc::ImageBarrier (cmd, map, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	vmaInvalidateAllocation (ctx->allocator, rb.alloc, 0, VK_WHOLE_SIZE);
	const float *m = (const float*)rb.mapped;
	const Frame &f = lastFrame;
	kerr::Hole K (1.0, f.a);
	// corners along the middle row and column: each traced in double
	// precision (Kerr.h) far out (1e6 M)
	int n = 0, agree = 0, dirOk = 0, dirN = 0;
	double worst = 0.0;
	for (int s = 0; s < 64; s++) {
		int ix = s < 32 ? (int)((s + 0.5) / 32.0 * (mapW - 1)) : (int)(mapW / 2);
		int iy = s < 32 ? (int)(mapH / 2) : (int)((s - 31.5) / 32.0 * (mapH - 1));
		const float *g = m + ((size_t)iy * mapW + ix) * 4;
		double px = ix * (double)grid, py = iy * (double)grid;
		double nx = px / imgW * 2.0 - 1.0, ny = py / imgH * 2.0 - 1.0;
		double vd[3] = { nx / f.fx, -ny / f.fy, 1.0 }, l = std::sqrt (vd[0] * vd[0] + vd[1] * vd[1] + 1.0);
		double nk[3];
		for (int i = 0; i < 3; i++) nk[i] = (f.v2k[i][0] * vd[0] + f.v2k[i][1] * vd[1] + f.v2k[i][2] * vd[2]) / l;
		double nl = std::sqrt (nk[0] * nk[0] + nk[1] * nk[1] + nk[2] * nk[2]);
		double qu[4];
		for (int mu = 0; mu < 4; mu++) qu[mu] = -f.e[0][mu] + (nk[0] * f.e[1][mu] + nk[1] * f.e[2][mu] + nk[2] * f.e[3][mu]) / nl;
		kerr::State st;
		for (int i = 0; i < 3; i++) st.x[1 + i] = f.cam[i];
		K.Lower (f.cam, qu, st.p);
		kerr::Integrator opt;
		opt.tol = 1e-10;
		bool captured = false;
		opt.stop = [&] (const kerr::State &q) {
			double r = K.R (q.x + 1);
			if (r < (f.inside ? std::max (f.rm, 0.02) : f.rh * 1.0005)) { captured = true; return true; }
			return r > 1e6;
		};
		double h = 0.0;
		kerr::Result res = kerr::Integrate (K, st, 1e9, opt, h);
		bool cpuEsc = res == kerr::GEO_STOPPED && !captured;
		bool gpuEsc = g[3] > 0.0f, gpuDisc = g[3] < 0.0f;
		if (gpuDisc) continue; // (the disc: not traced here)
		n++;
		if (cpuEsc == gpuEsc) agree++;
		if (cpuEsc && gpuEsc) {
			double v[4];
			K.Raise (st.x + 1, st.p, v);
			double d[3] = { v[1], v[2], v[3] }, dl = std::sqrt (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			double dg[3];
			for (int i = 0; i < 3; i++) dg[i] = (f.k2g[i][0] * d[0] + f.k2g[i][1] * d[1] + f.k2g[i][2] * d[2]) / dl;
			double c = dg[0] * g[0] + dg[1] * g[1] + dg[2] * g[2];
			double ang = std::acos (std::min (1.0, c)) * 57.29578;
			worst = std::max (worst, ang);
			dirN++;
			if (ang < 0.05) dirOk++;
		}
	}
	ctx->DestroyBuffer (rb);
	bool ok = n > 0 && agree >= n - 1 && dirOk == dirN;
	char buf[400];
	snprintf (buf, sizeof (buf), "lens check at r = %.2f M: %d of %d rays captured or escaping as Kerr.h's, directions of %d of %d within 0.05 deg (worst %.4f deg) %s",
		std::sqrt (f.cam[0] * f.cam[0] + f.cam[1] * f.cam[1] + f.cam[2] * f.cam[2]), agree, n, dirOk, dirN, worst, ok ? "ok" : "FAILED");
	oapiWriteLogV ("VulkanClient: %s", buf);
	printf ("%s\n", buf);
	fflush (stdout);
}

void BlackHoleLens::BinStars (VkCommandBuffer cmd)
{
	auto barrier = [&](VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = src, mb.srcAccessMask = sa, mb.dstStageMask = dst, mb.dstAccessMask = da;
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2 (cmd, &dep);
	};
	const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	// (after the last frame's sky read the bins, the star field's selection wrote its list)
	barrier (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		CS, RW);
	struct { float obs[4]; float misc[4]; } p{};
	const galactic::Observer &o = starField->CurObserver ();
	for (int i = 0; i < 3; i++) p.obs[i] = (float)o.pos[i];
	p.obs[3] = std::min (starField->Limit (), sky->ShownLimit ());
	p.misc[0] = (float)starField->YearsNow (), p.misc[2] = (float)STAR_CAP;
	VkDescriptorSet sets[2] = { starField->DrawSet (), starSet };
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, starPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, starPL, 0, 2, sets, 0, nullptr);
	for (int mode = 0; mode < 3; mode++) {
		p.misc[1] = (float)mode;
		vkCmdPushConstants (cmd, starPL, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (p), &p);
		vkCmdDispatch (cmd, mode == 1 ? 1 : (STAR_CAP + 255) / 256, 1, 1);
		barrier (CS, RW, CS | (mode == 2 ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : 0), RW);
	}
}

void BlackHoleLens::Render (VkCommandBuffer cmd, const GalaxySky::Display &d, float sunSurface)
{
	if (!ready || !traced) return;
	struct { float colour[3][4]; float tone[4]; float grid[4]; float bg[4]; float disc[4]; } p{};
	p.disc[0] = d.gain > 0.0f ? sunSurface / d.gain : 0.0f;
	sky->DisplayParams (d, p.colour, p.tone);
	p.grid[0] = (float)grid, p.grid[1] = sky->Fade (), p.grid[2] = cosmic ? 1.0f : 0.0f;
	for (int i = 0; i < 3; i++) p.bg[i] = (float)betaBg[i];
	p.bg[3] = stars ? 1.0f : 0.0f;
	VkDescriptorSet sets[2] = { sky->DrawSet (), drawSet };
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPL, 0, 2, sets, 0, nullptr);
	vkCmdPushConstants (cmd, drawPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (p), &p);
	vkCmdDraw (cmd, 3, 1, 0, 0);
}

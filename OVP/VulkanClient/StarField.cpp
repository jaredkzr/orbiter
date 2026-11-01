// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "StarField.h"
#include "GalaxySky.h"
#include "StarCatalogue.h"
#include "OrbiterAPI.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "star_select.comp.h"
#include "star_gen.comp.h"
#include "starfield.vert.h"
#include "starfield.frag.h"

namespace {
	const char *CATALOGUE = "Config/CSphere/Sky/stars.bin";
	constexpr int NTEMP = 256;
	constexpr double LOGT0 = 3.2, LOGT1 = 4.7;       // temperature index range (as starfield.glsl)
	constexpr double RESELECT_DIST = 0.25;           // observer movement that starts a new selection [pc] (stars within
	                                                 // 2 pc are always selected: their brightness changes faster)
	constexpr double AT_SUN = 0.01;                  // [pc]: the catalogue's extinction from the Sun applies
	constexpr float MARGIN = 0.5f;                   // [mag] selected beyond the limit (the observer moves on)
	constexpr uint32_t STARS_PER_FRAME = 1u << 19;   // of a selection spread over frames
	constexpr float PSF_SIGMA = 0.6f;                // [px]
	// glare (light scattered in the eye or the optics, which makes bright
	// stars look big): two wide halos, sigma [screen heights], fraction
	constexpr float GLARE_SIGMA[2] = { 0.0035f, 0.018f }; // (their shares: starfield.glsl)
	constexpr double RESELECT_YEARS = 200.0;         // time that starts a new selection (the stars' motion: 0.02 pc at 100 km/s)
	constexpr double GEN_BRIGHTEST = 2.5;            // M_V: brighter stars are not generated (seen too far)
	constexpr int GEN_CELLS = 6;                     // cells across the radius a row's stars are seen within, at least
	constexpr double GEN_PER_CELL = 16.0;            // stars per cell, at most ~ (at the Sun's density: the work of a thread)
	constexpr int GEN_MAX_K = 64;                    // cells per axis, at most
	constexpr uint32_t GEN_CAPACITY = 2u << 20;      // generated stars

	struct GpuStar { float pos[3]; uint32_t packed, vel; }; // (starfield.glsl: Star)
	struct ListHeader { uint32_t vertexCount, instanceCount, firstVertex, firstInstance, capacity, pad[3]; };

	struct SelectPush { float obs[4]; uint32_t part, nParts, atSun, hideSun, nStar; float years; uint32_t pad[2]; float beta[4], ext[4]; uint32_t excl[4]; };
	struct GenPush { float obs[4]; uint32_t nRows, nWork, part, nParts; float catLimit, catNear, pad[2]; float beta[4], ext[4]; uint32_t pad2[4]; };
	constexpr double RESELECT_U = 0.03;              // relativistic view: change of the proper velocity (gamma beta, relative
	                                                 // to its size, at least 1) that starts a new selection

	// Relativistic view: the factor on a black body's V band flux (temperature
	// T [K]) seen at Doppler factor D (as relsky.glsl's RelStarBoostV)
	double StarBoostV (double T, double D)
	{
		auto logPlanck = [](double l, double T) {
			double um = l * 1e-3, x = 14387.77 / (um * T);
			return -5.0 * std::log (um) - (x > 20.0 ? x : std::log (std::expm1 (x)));
		};
		double s0 = 0.0, s1 = 0.0;
		for (int k = 0; k < 4; k++) {
			double l = 505.0 + 90.0 * (k + 0.5) * 0.25;
			s0 += std::exp (logPlanck (l, T) - logPlanck (550.0, T));
			s1 += std::exp (logPlanck (l, D * T) - logPlanck (550.0, T));
		}
		return s1 / (s0 * D * D);
	}
	struct GenHeader { uint32_t count, capacity, pad[2]; };
	struct GenRow { float a[4]; int32_t cell0[4]; float b[4]; uint32_t c[4]; float d[4]; };
	constexpr int GEN_ROWS = 1024;                   // (field rows, then cluster rows)
	constexpr uint32_t CLUSTER_STARS_PER_ITEM = 64;  // a work item's stars of a cluster row
	constexpr double CLUSTER_MAX_STARS = 1.2e6;      // generated in clusters, at most (the brightest first)

	// G - V of dwarfs by temperature (Pecaut & Mamajek 2013, 2022 version)
	double GminusV (double T)
	{
		static const double tab[][2] = { { 2420, -3.09 }, { 2680, -2.98 }, { 2930, -1.95 }, { 3210, -1.40 }, { 3470, -1.02 },
			{ 3660, -0.82 }, { 3930, -0.66 }, { 4300, -0.51 }, { 4830, -0.32 }, { 5270, -0.227 }, { 5680, -0.173 },
			{ 6050, -0.145 }, { 6750, -0.09 }, { 7760, -0.03 }, { 9300, 0.01 }, { 14500, 0.01 }, { 24500, -0.02 } };
		const int n = sizeof(tab) / sizeof(tab[0]);
		if (T <= tab[0][0]) return tab[0][1];
		for (int i = 1; i < n; i++)
			if (T < tab[i][0]) return tab[i-1][1] + (tab[i][1] - tab[i-1][1]) * (T - tab[i-1][0]) / (tab[i][0] - tab[i-1][0]);
		return tab[n-1][1];
	}
	struct DrawPush { float viewProj[16]; float obs[4]; float sprite[4]; float glare[4]; float misc[4]; };

	double Dist (const double *a, const double *b)
	{
		return std::sqrt ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
	}

	double Norm (const double *a)
	{
		return std::sqrt (a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
	}
}

std::unique_ptr<galactic::StarCatalogue> StarField::Load ()
{
	auto cat = std::make_unique<galactic::StarCatalogue> ();
	if (!cat->Load (CATALOGUE)) {
		oapiWriteLogError ("VulkanClient: %s: unsupported star catalogue", CATALOGUE);
		return nullptr;
	}
	return cat;
}

bool StarField::Init (vkc::Context *c, const GalaxySky &sky, const galactic::StarCatalogue &cat, const galactic::Observer &obs, bool hideSun)
{
	ctx = c;
	auto t0 = std::chrono::steady_clock::now ();
	nStar = (uint32_t)cat.Count ();
	catLimit = cat.CompleteMag (), catNear = cat.CompleteDist ();
	lfRows = sky.LFRows ();

	// the stars as seen from the Sun at the catalogue's epoch, their
	// velocities (where they are seen from elsewhere at other times:
	// starfield.glsl, StarSeenPos), packed
	catEpoch = cat.Epoch ();
	std::vector<GpuStar> gs (nStar);
	const auto &sec = cat.Sectors ();
	const uint16_t *pos = cat.RawPos (), *logT = cat.RawLogT (), *flags = cat.RawFlags ();
	const uint32_t *sectorOf = cat.SectorOf ();
	const int16_t *mv = cat.RawAbsMag ();
	const double edge = cat.SectorEdge ();
	for (uint32_t i = 0; i < nStar; i++) {
		const auto &s = sec[sectorOf[i]];
		double v[3];
		cat.Velocity (i, v);
		for (int k = 0; k < 3; k++)
			gs[i].pos[k] = (float)((s.index[k] + pos[i * 3 + k] / 65536.0) * edge);
		auto q = [&](int k, int bias, int hi) { return (uint32_t)std::clamp ((int)std::lround (v[k]) + bias, 0, hi); };
		gs[i].vel = q (0, 1024, 2047) | (q (1, 1024, 2047) << 11) | (q (2, 512, 1023) << 22);
		uint32_t m = (uint32_t)std::clamp ((int)std::lround ((mv[i] * 1e-3 + 12.0) * 100.0), 0, 4095);
		uint32_t t = (uint32_t)std::clamp ((int)std::lround ((logT[i] * 1e-4 - LOGT0) / (LOGT1 - LOGT0) * (NTEMP - 1)), 0, NTEMP - 1);
		uint32_t a = (uint32_t)std::clamp ((int)std::lround (cat.ExtinctionFromSun (i) * 100.0), 0, 2047);
		gs[i].packed = m | (t << 12) | (a << 20) | ((flags[i] & galactic::StarCatalogue::SUN) ? 0x80000000u : 0u);
	}
	stars = ctx->CreateDeviceBuffer (gs.data(), gs.size() * sizeof(GpuStar), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	gs.clear (), gs.shrink_to_fit ();

	// band light by temperature
	{
		std::vector<float> cb (4 * (3 + 1 + NTEMP));
		for (int k = 0; k < 3; k++)
			for (int b = 0; b < GalaxySky::NBAND; b++) cb[k * 4 + b] = sky.BandColour ()[k][b];
		for (int b = 0; b < GalaxySky::NBAND; b++) cb[12 + b] = ext[b] = sky.Extinction ()[b];
		for (int t = 0; t < NTEMP; t++)
			sky.BandLight (std::pow (10.0, LOGT0 + (LOGT1 - LOGT0) * t / (NTEMP - 1)), &cb[16 + t * 4]);
		colours = ctx->CreateDeviceBuffer (cb.data(), cb.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	}

	// selection lists
	limit = selLimit = wantLimit = sky.StarLimit ();
	capacity = std::min (nStar, 4u << 20);
	for (int l = 0; l < 2; l++) {
		lists[l] = ctx->CreateBuffer (sizeof(ListHeader) + (VkDeviceSize)capacity * 8,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
		gens[l] = ctx->CreateBuffer (sizeof(GenHeader) + (VkDeviceSize)GEN_CAPACITY * sizeof(GpuStar),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
	}
	genRows = ctx->CreateBuffer (GEN_ROWS * sizeof(GenRow), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	clusters = sky.Objects ().Clusters ();
	CreatePipelines (sky);

	// the first selection at once
	listObs = curObs = obs, listHideSun = hideSun;
	selObs = obs, selHideSun = hideSun;
	vkc::Buffer rb = ctx->CreateReadbackBuffer (sizeof(ListHeader) + sizeof(GenHeader));
	ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
		Select (cmd, 0, 0, 1);
		VkBufferCopy r{ 0, 0, sizeof(ListHeader) };
		vkCmdCopyBuffer (cmd, lists[0].buffer, rb.buffer, 1, &r);
		r = { 0, sizeof(ListHeader), sizeof(GenHeader) };
		vkCmdCopyBuffer (cmd, gens[0].buffer, rb.buffer, 1, &r);
	});
	vmaInvalidateAllocation (ctx->allocator, rb.alloc, 0, VK_WHOLE_SIZE);
	uint32_t nSel = ((const ListHeader*)rb.mapped)->instanceCount;
	uint32_t nGen = ((const GenHeader*)((const char*)rb.mapped + sizeof(ListHeader)))->count;
	ctx->DestroyBuffer (rb);
	front = 0;
	double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
	oapiWriteLogV ("VulkanClient: %u catalogue stars; %u brighter than V = %.1f from (%.1f, %.1f, %.1f) pc, %u of them generated (%.0f ms)",
		nStar, nSel, limit, obs.pos[0], obs.pos[1], obs.pos[2], nGen, ms);
	if (nSel > capacity) oapiWriteLogV ("VulkanClient: star selection truncated to %u", capacity);
	ready = true;
	return true;
}

void StarField::CreatePipelines (const GalaxySky &sky)
{
	const int NL = GalaxySky::DustLevels ();
	// selection and generation: model, dust grids, catalogue stars, list,
	// generated stars, generator rows
	{
		VkDescriptorSetLayoutBinding b[6] = {
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)NL, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = 6, lci.pBindings = b;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &selectLayout));
	}
	// sprites: catalogue stars, list, colours, generated stars
	{
		VkDescriptorSetLayoutBinding b[4] = {
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = 4, lci.pBindings = b;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &drawLayout));
	}
	VkDescriptorPoolSize ps[2] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * 5 + 2 * 4 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)(2 * NL) } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 4, pci.poolSizeCount = 2, pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	for (int l = 0; l < 2; l++) {
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = pool, dai.descriptorSetCount = 1;
		dai.pSetLayouts = &selectLayout;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &selectSet[l]));
		dai.pSetLayouts = &drawLayout;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &drawSet[l]));

		VkDescriptorBufferInfo mi{ sky.ModelBuffer (), 0, VK_WHOLE_SIZE }, si{ stars.buffer, 0, VK_WHOLE_SIZE };
		VkDescriptorBufferInfo li{ lists[l].buffer, 0, VK_WHOLE_SIZE }, ci{ colours.buffer, 0, VK_WHOLE_SIZE };
		VkDescriptorBufferInfo gi{ gens[l].buffer, 0, VK_WHOLE_SIZE }, ri{ genRows.buffer, 0, VK_WHOLE_SIZE };
		std::vector<VkDescriptorImageInfo> di (NL);
		for (int k = 0; k < NL; k++) di[k] = { sky.DustSampler (), sky.DustView (k), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet wr[10]{};
		for (auto &w : wr) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		const VkDescriptorBufferInfo *sel[6] = { &mi, nullptr, &si, &li, &gi, &ri }, *drw[4] = { &si, &li, &ci, &gi };
		int n = 0;
		for (int b = 0; b < 6; b++, n++) {
			wr[n].dstSet = selectSet[l], wr[n].dstBinding = b;
			if (b == 1) wr[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[n].descriptorCount = NL, wr[n].pImageInfo = di.data();
			else wr[n].pBufferInfo = sel[b];
		}
		for (int b = 0; b < 4; b++, n++) wr[n].dstSet = drawSet[l], wr[n].dstBinding = b, wr[n].pBufferInfo = drw[b];
		vkUpdateDescriptorSets (ctx->device, n, wr, 0, nullptr);
	}

	// selection and generation (one layout: their parameters are alike in size)
	{
		static_assert (sizeof(SelectPush) == sizeof(GenPush), "push constants");
		VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SelectPush) };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 1, plci.pSetLayouts = &selectLayout;
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &selectPL));
		const unsigned char *code[2] = { spv_star_select_comp, spv_star_gen_comp };
		size_t size[2] = { spv_star_select_comp_size, spv_star_gen_comp_size };
		VkPipeline *pipe[2] = { &selectPipe, &genPipe };
		for (int k = 0; k < 2; k++) {
			VkShaderModule cs = ctx->CreateShaderModule (code[k], size[k]);
			VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
			cpi.layout = selectPL;
			VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, pipe[k]));
			vkDestroyShaderModule (ctx->device, cs, nullptr);
		}
	}
	// sprites: instanced quads, added to the scene
	{
		VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawPush) };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 1, plci.pSetLayouts = &drawLayout;
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &drawPL));
		VkShaderModule vs = ctx->CreateShaderModule (spv_starfield_vert, spv_starfield_vert_size);
		VkShaderModule fs = ctx->CreateShaderModule (spv_starfield_frag, spv_starfield_frag_size);
		VkPipelineShaderStageCreateInfo st[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" } };
		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
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
		gpi.stageCount = 2, gpi.pStages = st;
		gpi.pVertexInputState = &vi, gpi.pInputAssemblyState = &ia, gpi.pViewportState = &vps;
		gpi.pRasterizationState = &rs, gpi.pMultisampleState = &ms, gpi.pDepthStencilState = &ds;
		gpi.pColorBlendState = &cb, gpi.pDynamicState = &dy;
		gpi.layout = drawPL;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &drawPipe));
		vkDestroyShaderModule (ctx->device, vs, nullptr);
		vkDestroyShaderModule (ctx->device, fs, nullptr);
	}
}

void StarField::Shutdown ()
{
	if (!ctx) return;
	vkDeviceWaitIdle (ctx->device);
	vkDestroyPipeline (ctx->device, selectPipe, nullptr);
	vkDestroyPipeline (ctx->device, genPipe, nullptr);
	vkDestroyPipeline (ctx->device, drawPipe, nullptr);
	vkDestroyPipelineLayout (ctx->device, selectPL, nullptr);
	vkDestroyPipelineLayout (ctx->device, drawPL, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, selectLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, drawLayout, nullptr);
	ctx->DestroyBuffer (stars);
	ctx->DestroyBuffer (colours);
	for (auto &l : lists) ctx->DestroyBuffer (l);
	for (auto &g : gens) ctx->DestroyBuffer (g);
	ctx->DestroyBuffer (genRows);
	ready = false;
	ctx = nullptr;
}

void StarField::Select (VkCommandBuffer cmd, int l, int part, int nParts)
{
	bool begin = part == 0, end = part == nParts - 1;
	auto barrier = [&](VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
		VkBufferMemoryBarrier2 b[2] = {};
		for (int k = 0; k < 2; k++) {
			b[k].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
			b[k].srcStageMask = ss, b[k].srcAccessMask = sa, b[k].dstStageMask = ds, b[k].dstAccessMask = da;
			b[k].srcQueueFamilyIndex = b[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b[k].buffer = k ? gens[l].buffer : lists[l].buffer, b[k].offset = 0, b[k].size = VK_WHOLE_SIZE;
		}
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.bufferMemoryBarrierCount = 2, dep.pBufferMemoryBarriers = b;
		vkCmdPipelineBarrier2 (cmd, &dep);
	};
	const VkPipelineStageFlags2 DRAW = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
	const VkAccessFlags2 DRAW_READ = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
	const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	bool atSun = std::sqrt (selObs.pos[0] * selObs.pos[0] + selObs.pos[1] * selObs.pos[1] + selObs.pos[2] * selObs.pos[2]) < AT_SUN;
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selectPL, 0, 1, &selectSet[l], 0, nullptr);
	if (begin) {
		// (after the draws of earlier frames from this list)
		barrier (DRAW, DRAW_READ, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		ListHeader h{ 4, 0, 0, 0, capacity, { 0, 0, 0 } };
		vkCmdUpdateBuffer (cmd, lists[l].buffer, 0, sizeof(h), &h);
		GenHeader g{ 0, GEN_CAPACITY, { 0, 0 } };
		vkCmdUpdateBuffer (cmd, gens[l].buffer, 0, sizeof(g), &g);
		barrier (VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, RW);
		// the generator's work (none to do at the Sun)
		genWork = 0;
		// (relativistic view: stars too faint for the catalogue may be brightened)
		if (!atSun || Norm (selBeta) > 1e-3) GenRows (genRowCount, genWork);
	}
	else barrier (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, RW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, RW);

	// the stars the catalogue lacks: a part of the generator's cells
	if (genWork) {
		GenPush p{};
		for (int k = 0; k < 3; k++) p.obs[k] = (float)selObs.pos[k];
		p.obs[3] = selLimit + MARGIN;
		p.nRows = genRowCount, p.nWork = genWork;
		p.part = (uint32_t)part, p.nParts = (uint32_t)nParts;
		p.catLimit = catLimit, p.catNear = catNear;
		for (int k = 0; k < 3; k++) p.beta[k] = (float)selBeta[k];
		for (int k = 0; k < 4; k++) p.ext[k] = ext[k];
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, genPipe);
		vkCmdPushConstants (cmd, selectPL, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
		vkCmdDispatch (cmd, (genWork / nParts + 64) / 64, 1, 1);
	}

	SelectPush p{};
	for (int k = 0; k < 3; k++) p.obs[k] = (float)selObs.pos[k];
	p.obs[3] = selLimit + MARGIN;
	p.part = (uint32_t)part, p.nParts = (uint32_t)nParts, p.nStar = nStar;
	uint32_t count = (nStar - (uint32_t)part + (uint32_t)nParts - 1) / (uint32_t)nParts;
	p.atSun = atSun;
	p.hideSun = selHideSun;
	p.years = (float)Years (selObs);
	for (int k = 0; k < MAX_EXCLUDED; k++) p.excl[k] = excluded[k];
	for (int k = 0; k < 3; k++) p.beta[k] = (float)selBeta[k];
	for (int k = 0; k < 4; k++) p.ext[k] = ext[k];
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selectPipe);
	vkCmdPushConstants (cmd, selectPL, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
	vkCmdDispatch (cmd, (count + 255) / 256, 1, 1);

	if (end)
		barrier (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, RW, DRAW | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			DRAW_READ | VK_ACCESS_2_TRANSFER_READ_BIT);
}

bool StarField::GenRows (uint32_t &nRows, uint32_t &nWork)
{
	// per luminosity function row fainter than GEN_BRIGHTEST: a cube of
	// cells around the observer, GEN_CELLS across the radius within which
	// its brightest stars are seen
	GenRow *rows = (GenRow*)genRows.mapped;
	const uint32_t maxRows = (uint32_t)(genRows.size / sizeof(GenRow));
	nRows = nWork = 0;
	// (relativistic view: seen as far as the brightest of them ahead, at the largest Doppler factor)
	double b = Norm (selBeta), dMax = b > 0.0 ? std::sqrt ((1.0 + b) / (1.0 - b)) : 1.0;
	for (size_t i = 0; i < lfRows.size() && nRows < maxRows; i++) {
		const auto &r = lfRows[i];
		if (r.M - 0.5 < GEN_BRIGHTEST) continue;
		double boost = b > 0.0 ? std::max (0.0, 2.5 * std::log10 (std::max (StarBoostV (r.T * 0.9, dMax), StarBoostV (r.T * 1.1, dMax)))) : 0.0;
		double rVis = std::pow (10.0, (selLimit + MARGIN + boost - (r.M - 0.5)) / 5.0 + 1.0);
		double n = std::pow (10.0, r.logn);
		double cell = std::min (rVis / GEN_CELLS, std::cbrt (GEN_PER_CELL / n));
		cell = std::max (cell, 2.0 * rVis / (GEN_MAX_K - 1));
		GenRow &g = rows[nRows];
		int K = 0;
		for (int k = 0; k < 3; k++) {
			int lo = (int)std::floor ((selObs.pos[k] - rVis) / cell), hi = (int)std::floor ((selObs.pos[k] + rVis) / cell);
			g.cell0[k] = lo;
			K = std::max (K, hi - lo + 1);
		}
		g.cell0[3] = K;
		g.a[0] = (float)r.M, g.a[1] = (float)n;
		g.a[2] = (float)std::clamp ((std::log10 (r.T) - LOGT0) / (LOGT1 - LOGT0) * (NTEMP - 1), 0.0, NTEMP - 1.0);
		g.a[3] = (float)r.pop;
		g.b[0] = (float)cell, g.b[1] = (float)rVis, g.b[2] = (float)GminusV (r.T), g.b[3] = 0.0f;
		g.c[0] = nWork, g.c[1] = (uint32_t)i + 1, g.c[2] = g.c[3] = 0;
		nWork += (uint32_t)(K * K * K);
		nRows++;
	}

	// the globular clusters' stars: per cluster, its luminosity function
	// rows (the old population's, scaled to the cluster) whose stars are
	// seen from the observer, the brightest first
	struct Cand { size_t cl, row; double M, n; };
	std::vector<Cand> cand;
	for (size_t c = 0; c < clusters.size(); c++) {
		const auto &cl = clusters[c];
		double dmin = std::max (Dist (cl.pos, selObs.pos) - cl.rt, 1.0);
		for (size_t i = 0; i < lfRows.size(); i++) {
			const auto &r = lfRows[i];
			double boost = b > 0.0 ? std::max (0.0, 2.5 * std::log10 (StarBoostV (r.T, dMax))) : 0.0;
			if (r.pop != 2 || r.M - 0.5 + 5.0 * std::log10 (dmin / 10.0) >= selLimit + MARGIN + boost) continue;
			double n = cl.volume * std::pow (10.0, r.logn);
			if (n >= 0.01) cand.push_back ({ c, i, r.M, n });
		}
	}
	std::sort (cand.begin(), cand.end(), [](const Cand &x, const Cand &y) { return x.M < y.M; });
	double total = 0.0;
	for (const Cand &k : cand) {
		if (nRows >= maxRows || (total += k.n) > CLUSTER_MAX_STARS) break;
		const auto &cl = clusters[k.cl];
		const auto &r = lfRows[k.row];
		GenRow &g = rows[nRows];
		g = GenRow{};
		g.a[0] = (float)r.M, g.a[1] = (float)k.n;
		g.a[2] = (float)std::clamp ((std::log10 (r.T) - LOGT0) / (LOGT1 - LOGT0) * (NTEMP - 1), 0.0, NTEMP - 1.0);
		g.a[3] = -1.0f; // (a cluster's row)
		g.b[0] = (float)cl.rt, g.b[2] = (float)GminusV (r.T);
		g.c[0] = nWork, g.c[1] = cl.seed, g.c[2] = (uint32_t)k.row + 1;
		for (int j = 0; j < 3; j++) g.d[j] = (float)cl.pos[j];
		g.d[3] = (float)cl.a;
		nWork += (uint32_t)((uint32_t)std::ceil (k.n + 1.0) + CLUSTER_STARS_PER_ITEM - 1) / CLUSTER_STARS_PER_ITEM;
		nRows++;
	}
	vmaFlushAllocation (ctx->allocator, genRows.alloc, 0, VK_WHOLE_SIZE);
	return nRows > 0;
}

void StarField::SetLimit (float m)
{
	if (m == wantLimit) return;
	wantLimit = m;
	selectAgain = true;
}

void StarField::SetExcluded (const std::vector<int> &idx)
{
	uint32_t e[MAX_EXCLUDED];
	for (int k = 0; k < MAX_EXCLUDED; k++) e[k] = k < (int)idx.size () ? (uint32_t)idx[k] : ~0u;
	if (memcmp (e, excluded, sizeof (e))) {
		memcpy (excluded, e, sizeof (e));
		selectAgain = true;
	}
}

void StarField::Update (const galactic::Observer &obs, bool hideSun, const double beta[3])
{
	if (!ready) return;
	curObs = obs;
	double bt[3] = { 0.0, 0.0, 0.0 };
	if (beta) for (int k = 0; k < 3; k++) bt[k] = beta[k];
	if (!selecting) {
		// (a selection under way for an old limit is finished first)
		// relativistic view: again when the proper velocity has changed
		// (brightnesses with it)
		auto u = [](const double *b, double *out) {
			double g = 1.0 / std::sqrt (std::max (1.0 - b[0] * b[0] - b[1] * b[1] - b[2] * b[2], 1e-12));
			for (int k = 0; k < 3; k++) out[k] = g * b[k];
		};
		double u0[3], u1[3];
		u (listBeta, u0), u (bt, u1);
		bool moved = Dist (u0, u1) > RESELECT_U * std::max (1.0, Norm (u0));
		bool aged = std::fabs (Years (obs) - Years (listObs)) > RESELECT_YEARS;
		if (Dist (obs.pos, listObs.pos) < RESELECT_DIST && hideSun == listHideSun && !selectAgain && !moved && !aged) return;
		selecting = true, selPart = 0, selObs = obs, selHideSun = hideSun, selectAgain = false, selLimit = wantLimit;
		for (int k = 0; k < 3; k++) selBeta[k] = bt[k];
	}
	int back = 1 - front;
	int nParts = (int)((nStar + STARS_PER_FRAME - 1) / STARS_PER_FRAME);
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd ();
	Select (cmd, back, selPart, nParts);
	if (++selPart >= nParts) {
		front = back, listObs = selObs, listHideSun = selHideSun, limit = selLimit;
		for (int k = 0; k < 3; k++) listBeta[k] = selBeta[k];
		selecting = false;
		if (getenv ("ORBITER_VK_SKY_LOG"))
			oapiWriteLogV ("VulkanClient: stars selected from (%.2f, %.2f, %.2f) pc", selObs.pos[0], selObs.pos[1], selObs.pos[2]);
	}
}

void StarField::Render (VkCommandBuffer cmd, const float galViewProj[16], float fx, float fy, float gain, float maxLimit, const float relBetaGal[3])
{
	if (!ready) return;
	VkExtent2D ext = ctx->RenderExtent ();
	DrawPush p;
	memcpy (p.viewProj, galViewProj, sizeof(p.viewProj));
	for (int k = 0; k < 3; k++) p.obs[k] = (float)curObs.pos[k];
	p.obs[3] = std::min (limit, maxLimit);
	p.sprite[0] = 2.0f / ext.width, p.sprite[1] = 2.0f / ext.height;
	p.sprite[2] = PSF_SIGMA;
	// a pixel at the centre: (2 / (fx W)) (2 / (fy H)) rad^2
	double pixDeg2 = 4.0 / (fx * ext.width * fy * ext.height) * std::pow (180.0 / 3.14159265358979323846, 2.0);
	p.sprite[3] = (float)(gain / pixDeg2);
	for (int k = 0; k < 2; k++) p.glare[k] = GLARE_SIGMA[k] * ext.height;
	p.glare[2] = (float)Years (curObs); // (the stars' motion)
	p.misc[0] = gain;
	for (int k = 0; k < 3; k++) p.misc[1 + k] = relBetaGal ? relBetaGal[k] : 0.0f; // (relativistic view: aberrated directions)
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPL, 0, 1, &drawSet[front], 0, nullptr);
	vkCmdPushConstants (cmd, drawPL, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
	vkCmdDrawIndirect (cmd, lists[front].buffer, 0, 1, 0);
}

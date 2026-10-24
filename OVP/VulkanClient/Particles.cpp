// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Particles.h"
#include "Scene.h"
#include "VulkanClient.h"
#include "VkTexture.h"
#include "VesselAPI.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "particle_emit.comp.h"
#include "particle_update.comp.h"
#include "particle_update_rt.comp.h"
#include "particle_light.comp.h"
#include "particle_light_rt.comp.h"
#include "particle_shadow_splat.comp.h"
#include "particle_shadow_sum.comp.h"
#include "gpuparticle.vert.h"
#include "gpuparticle.frag.h"

namespace {
	constexpr size_t PARTICLE_SIZE = 64; // (Particle in particles.glsl)
	constexpr double NOISE_SCALE = 0.08; // turbulence noise coordinates per metre (eddies of about 12 m)
	// smoke shadows: the grid (particle_common.glsl) around the camera
	constexpr uint32_t SHADOW_NX = 192, SHADOW_NZ = 64;
	constexpr double SHADOW_HALF_WIDTH = 1200.0, SHADOW_HALF_DEPTH = 1500.0; // [m]
	constexpr double SMOKE_EXTINCTION = 0.2; // optical depth (towards the sun) per unit of particle opacity
}

// ======================================================================
// ParticleSystem

struct ParticleSystem::GpuEmission { // particles.glsl
	float p0[4], p1[4], vel[4], dir[4], shape[4];
	uint32_t slab[4], misc[4];
};

struct ParticleSystem::UpdateParams { // particle_common.glsl
	float camShift[4], bodyPos[4], bodyVel[4], omega[4], noiseOrg[4];
	float lightX[4], lightY[4], lightZ[4], ground[4];
};

bool ParticleSystem::Init (vkc::Context *c, TextureManager *tm, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout atmLayout,
	VkDescriptorSetLayout texLayout, VkImageView shapeNoise, VkImageView detailNoise)
{
	ctx = c, texmgr = tm;
	{
		// smoke puffs: clusters of soft blobs (cauliflower-like billows),
		// broken up by noise, slightly darker inside
		const int N = 256, Q = N / 2;
		std::vector<uint8_t> px (N * N * 4);
		std::mt19937 rng (4242);
		std::uniform_real_distribution<double> uni (0.0, 1.0);
		auto hash = [](int x, int y, int o) {
			uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663) ^ (uint32_t)(o * 83492791);
			h ^= h >> 13; h *= 0x5bd1e995; h ^= h >> 15;
			return (h & 0xffff) / 65535.0;
		};
		auto vnoise = [&](double x, double y, int o) { // value noise, tiling not needed
			int ix = (int)std::floor (x), iy = (int)std::floor (y);
			double fx = x - ix, fy = y - iy;
			fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
			double a = hash (ix, iy, o), b = hash (ix + 1, iy, o), c2 = hash (ix, iy + 1, o), d = hash (ix + 1, iy + 1, o);
			return (a + (b - a) * fx) + ((c2 + (d - c2) * fx) - (a + (b - a) * fx)) * fy;
		};
		for (int q = 0; q < 4; q++) {
			struct Blob { double x, y, r; };
			std::vector<Blob> blobs{ { 0.0, 0.0, 0.42 } };
			for (int k = 0; k < 7; k++) {
				double ang = uni (rng) * 6.2831853, d = 0.18 + 0.15 * uni (rng);
				blobs.push_back ({ d * cos (ang), d * sin (ang), 0.16 + 0.12 * uni (rng) });
			}
			for (int y = 0; y < Q; y++)
				for (int x = 0; x < Q; x++) {
					double u = (x + 0.5) / Q * 2.0 - 1.0, v = (y + 0.5) / Q * 2.0 - 1.0;
					double dens = 0.0;
					for (auto &b : blobs) {
						double dx = u - b.x, dy = v - b.y;
						dens += std::exp (-(dx * dx + dy * dy) / (b.r * b.r) * 2.0);
					}
					double n = 0.0, amp = 0.5, f = 4.0;
					for (int o = 0; o < 4; o++, amp *= 0.5, f *= 2.0) n += amp * vnoise (u * f + q * 17.0, v * f, o);
					double a = std::clamp ((dens * (0.55 + 0.9 * n) - 0.25) * 1.4, 0.0, 1.0);
					a *= std::clamp ((1.0 - std::sqrt (u * u + v * v)) * 4.0, 0.0, 1.0); // (zero at the edge)
					double shade = std::clamp (0.7 + 0.35 * n + 0.1 * dens, 0.0, 1.0);
					uint8_t *p = &px[((y + (q >> 1) * Q) * N + x + (q & 1) * Q) * 4];
					p[0] = p[1] = p[2] = (uint8_t)std::lround (shade * 255.0);
					p[3] = (uint8_t)std::lround (a * 255.0);
				}
		}
		smokeTex = texmgr->CreateRGBA (N, N, px.data());
	}
	shapeView = shapeNoise, detailView = detailNoise;
	if (const char *e = getenv ("ORBITER_VK_PARTICLES")) enabled = atoi (e) != 0;
	if (const char *e = getenv ("ORBITER_VK_PARTICLE_DENSITY")) density = std::clamp ((float)atof (e), 1.0f, 16.0f);

	pool = ctx->CreateBuffer (CAPACITY * PARTICLE_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	for (auto &b : emitBuf) b = ctx->CreateBuffer (MAX_EMISSIONS * sizeof(GpuEmission), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	VkBuffer pb = pool.buffer;
	ctx->ImmediateSubmit ([pb](VkCommandBuffer cmd) {
		vkCmdFillBuffer (cmd, pb, 0, VK_WHOLE_SIZE, 0); // (lifetime 0: no particles)
	});
	freeList.push_back ({ 0, CAPACITY });

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &noiseSampler));
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &clampSampler));

	// smoke shadow grid: smoke per cell, optical depth towards the sun
	auto image3D = [this](VkFormat fmt, VkImageUsageFlags usage) {
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_3D;
		ici.format = fmt;
		ici.extent = { SHADOW_NX, SHADOW_NX, SHADOW_NZ };
		ici.mipLevels = 1, ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image, vci.viewType = VK_IMAGE_VIEW_TYPE_3D, vci.format = fmt;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		return im;
	};
	shadowAccum = image3D (VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	shadowDepth = image3D (VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	VkImage gi[2] = { shadowAccum.image, shadowDepth.image };
	ctx->ImmediateSubmit ([gi](VkCommandBuffer cmd) {
		VkClearColorValue zero{};
		VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		for (VkImage im : gi) {
			vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			vkCmdClearColorImage (cmd, im, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
		}
	});

	// pool, emissions, noise volumes, prepass, shadow grid
	const VkShaderStageFlags CS = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutBinding b[8] = {
		{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, CS | VK_SHADER_STAGE_VERTEX_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, CS, nullptr },
		{ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, CS, nullptr },
		{ 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, CS, nullptr },
		{ 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
		{ 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, CS, nullptr },
		{ 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, CS, nullptr },
		{ 7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, CS, nullptr } };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 8, lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));
	VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * vkc::FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * vkc::FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * vkc::FRAMES_IN_FLIGHT } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = vkc::FRAMES_IN_FLIGHT, pci.poolSizeCount = 3, pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &dpool));
	VkDescriptorSetLayout ls[vkc::FRAMES_IN_FLIGHT];
	for (auto &l : ls) l = setLayout;
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = dpool, dai.descriptorSetCount = vkc::FRAMES_IN_FLIGHT, dai.pSetLayouts = ls;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, sets));
	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++) {
		VkDescriptorBufferInfo bi[2] = { { pool.buffer, 0, VK_WHOLE_SIZE }, { emitBuf[i].buffer, 0, VK_WHOLE_SIZE } };
		VkDescriptorImageInfo ii[2] = { { noiseSampler, shapeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ noiseSampler, detailView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
		VkDescriptorImageInfo gii[3] = { { VK_NULL_HANDLE, shadowAccum.view, VK_IMAGE_LAYOUT_GENERAL },
			{ VK_NULL_HANDLE, shadowDepth.view, VK_IMAGE_LAYOUT_GENERAL }, { noiseSampler, shadowDepth.view, VK_IMAGE_LAYOUT_GENERAL } };
		VkWriteDescriptorSet wr[7]{};
		for (uint32_t k = 0; k < 7; k++) {
			wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			wr[k].dstSet = sets[i], wr[k].descriptorCount = 1;
			wr[k].dstBinding = k < 4 ? k : k + 1;
			if (k < 2) wr[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wr[k].pBufferInfo = &bi[k];
			else if (k < 4) wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[k].pImageInfo = &ii[k - 2];
			else {
				wr[k].descriptorType = k < 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				wr[k].pImageInfo = &gii[k - 4];
			}
		}
		vkUpdateDescriptorSets (ctx->device, 7, wr, 0, nullptr);
	}

	auto layoutOf = [this](std::initializer_list<VkDescriptorSetLayout> l, VkShaderStageFlags stages, uint32_t pcSize) {
		std::vector<VkDescriptorSetLayout> v (l);
		VkPushConstantRange pcr{ stages, 0, pcSize };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = (uint32_t)v.size(), plci.pSetLayouts = v.data();
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VkPipelineLayout pl;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &pl));
		return pl;
	};
	auto compute = [this](const unsigned char *code, size_t size, VkPipelineLayout l) {
		VkShaderModule cs = ctx->CreateShaderModule (code, size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = l;
		VkPipeline p;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &p));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
		return p;
	};
	emitLayout = layoutOf ({ setLayout }, CS, sizeof(uint32_t));
	updateLayout = layoutOf ({ setLayout, frameLayout, atmLayout }, CS, sizeof(UpdateParams));
	emitPipe = compute (spv_particle_emit_comp, spv_particle_emit_comp_size, emitLayout);
	updatePipe = compute (spv_particle_update_comp, spv_particle_update_comp_size, updateLayout);
	lightPipe = compute (spv_particle_light_comp, spv_particle_light_comp_size, updateLayout);
	splatPipe = compute (spv_particle_shadow_splat_comp, spv_particle_shadow_splat_comp_size, updateLayout);
	sumPipe = compute (spv_particle_shadow_sum_comp, spv_particle_shadow_sum_comp_size, updateLayout);

	// drawing: quads (triangle strips) from the pool, premultiplied or additive,
	// with the reactive mask
	drawLayout = layoutOf ({ frameLayout, setLayout, texLayout }, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 64);
	VkShaderModule vs = ctx->CreateShaderModule (spv_gpuparticle_vert, spv_gpuparticle_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_gpuparticle_frag, spv_gpuparticle_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" } };
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1, vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = ctx->sceneSamples;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed z
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
	VkFormat formats[2] = { ctx->sceneFormat, VK_FORMAT_R8_UNORM }; // (Scene::REACTIVE_FORMAT)
	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.colorAttachmentCount = 2, rci.pColorAttachmentFormats = formats;
	rci.depthAttachmentFormat = ctx->depthFormat;
	for (int additive = 0; additive < 2; additive++) {
		VkPipelineColorBlendAttachmentState cb[2]{};
		cb[0].colorWriteMask = 0xF;
		cb[0].blendEnable = VK_TRUE;
		cb[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[0].dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		cb[0].colorBlendOp = VK_BLEND_OP_ADD;
		cb[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		cb[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[0].alphaBlendOp = VK_BLEND_OP_ADD;
		cb[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
		cb[1].blendEnable = VK_TRUE;
		cb[1].srcColorBlendFactor = cb[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[1].colorBlendOp = VK_BLEND_OP_MAX;
		cb[1].srcAlphaBlendFactor = cb[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cb[1].alphaBlendOp = VK_BLEND_OP_MAX;
		VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cbs.attachmentCount = 2, cbs.pAttachments = cb;
		VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gpi.pNext = &rci;
		gpi.stageCount = 2, gpi.pStages = stages;
		gpi.pVertexInputState = &vi;
		gpi.pInputAssemblyState = &ia;
		gpi.pViewportState = &vps;
		gpi.pRasterizationState = &rs;
		gpi.pMultisampleState = &ms;
		gpi.pDepthStencilState = &ds;
		gpi.pColorBlendState = &cbs;
		gpi.pDynamicState = &dy;
		gpi.layout = drawLayout;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, additive ? &drawAdd : &drawBlend));
	}
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);
	return true;
}

void ParticleSystem::InitRT (VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rtScene, VkDescriptorSetLayout rtTex, VkDescriptorSetLayout atmLayout)
{
	VkDescriptorSetLayout l[5] = { setLayout, frameLayout, rtScene, rtTex, atmLayout };
	VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpdateParams) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 5, plci.pSetLayouts = l;
	plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &updateRTLayout));
	auto compute = [this](const unsigned char *code, size_t size, VkPipeline &pipe) {
		VkShaderModule cs = ctx->CreateShaderModule (code, size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = updateRTLayout;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
	};
	compute (spv_particle_update_rt_comp, spv_particle_update_rt_comp_size, updateRTPipe);
	compute (spv_particle_light_rt_comp, spv_particle_light_rt_comp_size, lightRTPipe);
}

void ParticleSystem::Shutdown ()
{
	if (!ctx) return;
	for (VkPipeline p : { emitPipe, updatePipe, updateRTPipe, lightPipe, lightRTPipe, splatPipe, sumPipe, drawBlend, drawAdd })
		if (p) vkDestroyPipeline (ctx->device, p, nullptr);
	for (VkPipelineLayout l : { emitLayout, updateLayout, updateRTLayout, drawLayout })
		if (l) vkDestroyPipelineLayout (ctx->device, l, nullptr);
	vkDestroyDescriptorPool (ctx->device, dpool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	vkDestroySampler (ctx->device, noiseSampler, nullptr);
	vkDestroySampler (ctx->device, clampSampler, nullptr);
	ctx->DestroyBuffer (pool);
	for (auto &b : emitBuf) ctx->DestroyBuffer (b);
	ctx->DestroyImage (shadowAccum);
	ctx->DestroyImage (shadowDepth);
	if (smokeTex) texmgr->Release (smokeTex);
	ctx = nullptr;
}

bool ParticleSystem::Allocate (uint32_t want, uint32_t &first, uint32_t &capacity)
{
	uint32_t n = std::max (BLOCK, (want + BLOCK - 1) / BLOCK * BLOCK);
	for (size_t i = 0; i < freeList.size(); i++) {
		auto &f = freeList[i];
		if (f.second < n) continue;
		first = f.first, capacity = n;
		f.first += n, f.second -= n;
		if (!f.second) freeList.erase (freeList.begin() + i);
		toClear.push_back ({ first, capacity });
		return true;
	}
	return false;
}

void ParticleSystem::Free (uint32_t first, uint32_t capacity)
{
	// (particles left in the ring are cleared when it is allocated again)
	freeList.push_back ({ first, capacity });
	std::sort (freeList.begin(), freeList.end());
	std::vector<std::pair<uint32_t, uint32_t>> merged;
	for (auto &f : freeList) {
		if (!merged.empty() && merged.back().first + merged.back().second == f.first) merged.back().second += f.second;
		else merged.push_back (f);
	}
	freeList.swap (merged);
}

void ParticleSystem::Queue (const Emission &e)
{
	if (e.count && queued.size() < MAX_EMISSIONS) queued.push_back (e);
}

void ParticleSystem::SetPrepass (VkImageView prepass)
{
	if (prepass == prepassView) return;
	if (prepassView) vkDeviceWaitIdle (ctx->device); // (the sets may be in use)
	prepassView = prepass;
	VkDescriptorImageInfo ii{ clampSampler, prepass, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	for (VkDescriptorSet s : sets) {
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = s, wr.dstBinding = 4, wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr.pImageInfo = &ii;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	}
}

void ParticleSystem::Simulate (const VECTOR3 &camPos, const VECTOR3 &sunDir, VkDescriptorSet frameSet, VkDescriptorSet atmSet,
	const VkDescriptorSet *rt, double simt)
{
	VkCommandBuffer cmd = ctx->Cmd ();
	uint32_t fi = ctx->FrameIndex ();
	VkDescriptorSet set = sets[fi];
	auto barrier = [cmd](VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA) {
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = src, mb.srcAccessMask = srcA, mb.dstStageMask = dst, mb.dstAccessMask = dstA;
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2 (cmd, &dep);
	};
	const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
	// the previous frame's drawing is done with the pool
	barrier (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | CS, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
	// rings allocated since: no particles
	for (auto &c : toClear)
		vkCmdFillBuffer (cmd, pool.buffer, (VkDeviceSize)c.first * PARTICLE_SIZE, (VkDeviceSize)c.second * PARTICLE_SIZE, 0);
	if (!toClear.empty()) barrier (VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
	toClear.clear ();

	// move and light the existing particles
	UpdateParams up{};
	VECTOR3 shift = haveCam ? lastCam - camPos : _V(0, 0, 0);
	lastCam = camPos, haveCam = true;
	up.camShift[0] = (float)shift.x, up.camShift[1] = (float)shift.y, up.camShift[2] = (float)shift.z;
	up.camShift[3] = (float)pendingDt;
	if (OBJHANDLE hp = oapiCameraProxyGbody ()) { // the air of the planet nearby
		VECTOR3 bp, bv;
		oapiGetGlobalPos (hp, &bp);
		oapiGetGlobalVel (hp, &bv);
		bp -= camPos;
		up.bodyPos[0] = (float)bp.x, up.bodyPos[1] = (float)bp.y, up.bodyPos[2] = (float)bp.z, up.bodyPos[3] = (float)oapiGetSize (hp);
		up.bodyVel[0] = (float)bv.x, up.bodyVel[1] = (float)bv.y, up.bodyVel[2] = (float)bv.z;
		// the ground under the camera (collisions without ray tracing)
		VECTOR3 loc;
		oapiGlobalToLocal (hp, &camPos, &loc);
		double rad = length (loc);
		if (rad > 0.0)
			up.ground[0] = (float)(oapiGetSize (hp) + oapiSurfaceElevation (hp, atan2 (loc.z, loc.x), asin (std::clamp (loc.y / rad, -1.0, 1.0))));
		up.ground[1] = (float)(GGRAV * oapiGetMass (hp) / (oapiGetSize (hp) * oapiGetSize (hp)));
		double T = oapiGetPlanetPeriod (hp);
		if (T != 0.0) {
			MATRIX3 R;
			oapiGetRotationMatrix (hp, &R);
			VECTOR3 w = mul (R, _V(0, 1, 0)) * (PI2 / T);
			up.omega[0] = (float)w.x, up.omega[1] = (float)w.y, up.omega[2] = (float)w.z;
		}
	}
	up.bodyVel[3] = (float)fmod (simt, 1e4);
	for (int k = 0; k < 3; k++)
		drawBody[k] = up.bodyPos[k], drawBody[4 + k] = up.bodyVel[k], drawBody[8 + k] = up.omega[k];
	drawBody[3] = up.bodyPos[3] > 0.0f ? 1.0f : 0.0f;
	up.omega[3] = (float)CAPACITY;
	for (int k = 0; k < 3; k++) {
		double q = (&camPos.x)[k] * NOISE_SCALE;
		up.noiseOrg[k] = (float)(q - floor (q));
	}
	up.noiseOrg[3] = (float)NOISE_SCALE;
	// smoke shadow grid: axes across the sunlight and towards the sun
	VECTOR3 lz = unit (sunDir);
	VECTOR3 lx = unit (crossp (lz, fabs (lz.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0))), ly = crossp (lz, lx);
	for (int k = 0; k < 3; k++)
		up.lightX[k] = (float)(&lx.x)[k], up.lightY[k] = (float)(&ly.x)[k], up.lightZ[k] = (float)(&lz.x)[k];
	up.lightX[3] = (float)SHADOW_HALF_WIDTH, up.lightY[3] = (float)SHADOW_HALF_DEPTH, up.lightZ[3] = (float)SMOKE_EXTINCTION;
	pendingDt = 0.0;
	bool useRT = rt && updateRTPipe;
	VkPipelineLayout ul = useRT ? updateRTLayout : updateLayout;
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, useRT ? updateRTPipe : updatePipe);
	if (useRT) {
		VkDescriptorSet s[5] = { set, frameSet, rt[0], rt[1], atmSet };
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ul, 0, 5, s, 0, nullptr);
	} else {
		VkDescriptorSet s[3] = { set, frameSet, atmSet };
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ul, 0, 3, s, 0, nullptr);
	}
	auto bindSets = [&](VkPipelineLayout l, bool withRT) {
		if (withRT) {
			VkDescriptorSet s[5] = { set, frameSet, rt[0], rt[1], atmSet };
			vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, l, 0, 5, s, 0, nullptr);
		} else {
			VkDescriptorSet s[3] = { set, frameSet, atmSet };
			vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, l, 0, 3, s, 0, nullptr);
		}
		vkCmdPushConstants (cmd, l, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(up), &up);
	};
	vkCmdPushConstants (cmd, ul, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(up), &up);
	vkCmdDispatch (cmd, CAPACITY / 64, 1, 1);

	// new particles (at the end of the step: not moved again)
	if (!queued.empty()) {
		barrier (CS, RW, CS, RW);
		GpuEmission *ge = (GpuEmission*)emitBuf[fi].mapped;
		uint32_t maxCount = 0, n = 0;
		static uint32_t seed = 1;
		for (const Emission &e : queued) {
			GpuEmission &g = ge[n++];
			VECTOR3 p0 = e.p0 - camPos, p1 = e.p1 - camPos;
			float f0[4] = { (float)p0.x, (float)p0.y, (float)p0.z, (float)e.v0 };
			float f1[4] = { (float)p1.x, (float)p1.y, (float)p1.z, (float)e.spread };
			float fv[4] = { (float)e.vel.x, (float)e.vel.y, (float)e.vel.z, (float)e.lifetime };
			float fd[4] = { (float)e.dir.x, (float)e.dir.y, (float)e.dir.z, (float)e.dt };
			float fs[4] = { (float)e.size, (float)e.growth, (float)e.alpha, (float)e.slowdown };
			memcpy (g.p0, f0, 16), memcpy (g.p1, f1, 16), memcpy (g.vel, fv, 16), memcpy (g.dir, fd, 16), memcpy (g.shape, fs, 16);
			uint32_t count = std::min (e.count, e.capacity);
			g.slab[0] = e.first, g.slab[1] = e.capacity, g.slab[2] = e.head, g.slab[3] = count;
			g.misc[0] = seed++, g.misc[1] = (e.emissive ? 1u : 0u) | (e.reentry ? 2u : 0u) | (e.tinted ? 4u : 0u) | (e.planar ? 8u : 0u)
				| (e.ballistic ? 16u : 0u) | (e.cold ? 32u : 0u);
			uint32_t rgb = 0;
			for (int k = 0; k < 3; k++) rgb |= (uint32_t)std::lround (std::clamp (e.albedo[k], 0.0f, 1.0f) * 255.0f) << (8 * k);
			g.misc[2] = rgb, g.misc[3] = 0;
			maxCount = std::max (maxCount, count);
		}
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, emitPipe);
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, emitLayout, 0, 1, &set, 0, nullptr);
		vkCmdPushConstants (cmd, emitLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(n), &n);
		vkCmdDispatch (cmd, (maxCount + 63) / 64, n, 1);
		queued.clear ();
	}

	// smoke shadows: smoke per cell of the grid, summed towards the sun
	VkClearColorValue zero{};
	VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	barrier (CS, RW, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	vkCmdClearColorImage (cmd, shadowAccum.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
	barrier (VK_PIPELINE_STAGE_2_TRANSFER_BIT | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW, CS, RW);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, splatPipe);
	bindSets (updateLayout, false);
	vkCmdDispatch (cmd, CAPACITY / 64, 1, 1);
	barrier (CS, RW, CS, RW);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sumPipe);
	vkCmdDispatch (cmd, (SHADOW_NX + 7) / 8, (SHADOW_NX + 7) / 8, 1);
	barrier (CS, RW, CS, RW);

	// sunlight on the particles
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, useRT ? lightRTPipe : lightPipe);
	bindSets (ul, useRT);
	vkCmdDispatch (cmd, CAPACITY / 64, 1, 1);
	barrier (CS, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

void ParticleSystem::Draw (uint32_t first, uint32_t capacity, VkTexture *tex, bool emissive, VkDescriptorSet frameSet)
{
	if (!enabled || !prepassView || !tex) return;
	VkCommandBuffer cmd = ctx->Cmd ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, emissive ? drawAdd : drawBlend);
	VkDescriptorSet s[3] = { frameSet, sets[ctx->FrameIndex ()], tex->set };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 3, s, 0, nullptr);
	struct { uint32_t first, flags; float alpha, pad; float body[12]; } pc{ first, emissive ? 1u : 0u, 1.0f, 0.0f, {} };
	memcpy (pc.body, drawBody, sizeof(drawBody));
	vkCmdPushConstants (cmd, drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw (cmd, 4, capacity, 0, 0);
}

// ======================================================================
// VkParticleStream

VkParticleStream::VkParticleStream (oapi::GraphicsClient *gc, TextureManager *tm, PARTICLESTREAMSPEC *pss, Kind k)
	: oapi::ParticleStream (gc, pss), kind(k), texmgr(tm), client((VulkanClient*)gc), rng((unsigned)(uintptr_t)this)
{
	if (pss) spec = *pss;
	else {
		spec = PARTICLESTREAMSPEC{};
		spec.srcsize = 1.0, spec.srcrate = 10.0, spec.lifetime = 2.0;
		spec.ltype = PARTICLESTREAMSPEC::DIFFUSE;
		spec.levelmap = PARTICLESTREAMSPEC::LVL_LIN;
		spec.atmsmap = PARTICLESTREAMSPEC::ATM_FLAT;
	}
	if (spec.tex) {
		tex = (VkTexture*)spec.tex;
		texmgr->AddRef (tex);
	} else {
		tex = texmgr->Load (kind == Kind::Reentry ? "Reentry.dds" : "Contrail1.dds");
	}
}

VkParticleStream::~VkParticleStream ()
{
	if (allocated && sys == client->Particles ()) sys->Free (first, capacity);
	texmgr->Release (tex);
}

bool VkParticleStream::EnsureRing ()
{
	ParticleSystem *ps = client->Particles ();
	if (!ps) return false;
	if (allocated && ps == sys) return true;
	sys = ps;
	head = 0;
	// ring size: the most particles alive at once (the emission rate rises
	// at speed in an atmosphere, up to 60 per second)
	double rate = std::max (spec.srcrate, kind == Kind::Reentry ? 0.0 : 60.0) * sys->density;
	double want = rate * std::max (spec.lifetime, 0.01) * 1.5 + 64.0;
	allocated = sys->Allocate ((uint32_t)std::min (want, 65536.0), first, capacity);
	if (!allocated) oapiWriteLogV ("VulkanClient: particle pool full");
	return allocated;
}

bool VkParticleStream::Expired () const
{
	// (particles live up to 1.5 lifetimes)
	return !hRef && simTime - lastEmit > 1.5 * spec.lifetime + 0.1;
}

double VkParticleStream::LevelAlpha (double lvl) const
{
	switch (spec.levelmap) {
	case PARTICLESTREAMSPEC::LVL_FLAT: return 1.0;
	case PARTICLESTREAMSPEC::LVL_LIN:  return std::clamp (lvl, 0.0, 1.0);
	case PARTICLESTREAMSPEC::LVL_SQRT: return std::sqrt (std::clamp (lvl, 0.0, 1.0));
	case PARTICLESTREAMSPEC::LVL_PLIN:
	case PARTICLESTREAMSPEC::LVL_PSQRT: {
		if (lvl < spec.lmin) return 0.0;
		double a = (spec.lmax > spec.lmin) ? std::min (1.0, (lvl - spec.lmin) / (spec.lmax - spec.lmin)) : 1.0;
		return spec.levelmap == PARTICLESTREAMSPEC::LVL_PSQRT ? std::sqrt (a) : a;
	}
	}
	return 1.0;
}

double VkParticleStream::AtmAlpha (double rho) const
{
	switch (spec.atmsmap) {
	case PARTICLESTREAMSPEC::ATM_FLAT: return 1.0;
	case PARTICLESTREAMSPEC::ATM_PLIN:
		if (spec.amax <= spec.amin) return rho >= spec.amin ? 1.0 : 0.0;
		return std::clamp ((rho - spec.amin) / (spec.amax - spec.amin), 0.0, 1.0);
	case PARTICLESTREAMSPEC::ATM_PLOG:
		if (rho <= spec.amin || spec.amin <= 0.0) return 0.0;
		if (spec.amax <= spec.amin) return 1.0;
		return std::clamp (std::log (rho / spec.amin) / std::log (spec.amax / spec.amin), 0.0, 1.0);
	}
	return 1.0;
}

bool VkParticleStream::SourceState (VECTOR3 &gpos, VECTOR3 &gdir, VECTOR3 &gvel, double &lvl)
{
	if (!hRef) return false;
	VESSEL *v = oapiGetVesselInterface (hRef);
	if (!v) return false;
	v->GetGlobalVel (gvel);
	hAtmRef = v->GetAtmRef ();
	double rho = hAtmRef ? v->GetAtmDensity () : 0.0;

	if (kind == Kind::Reentry) {
		// plasma at the leading side of the vessel, flowing with the air
		VECTOR3 air;
		if (!hAtmRef || !v->GetAirspeedVector (FRAME_GLOBAL, air)) return false;
		double va = length (air);
		if (va < 1.0) return false;
		double q = 0.5 * rho * va * va * va; // heat flux indicator [W/m^2]
		reentryLevel = q > 0.0 ? std::clamp ((std::log10 (q) - 6.0) / 1.5, 0.0, 1.0) : 0.0;
		lvl = reentryLevel;
		gdir = air / -va;
		v->Local2Global (_V(0, 0, 0), gpos);
		gpos += air * (v->GetSize() * 0.5 / va);
		gvel -= air; // emitted at rest relative to the atmosphere
	} else {
		lvl = level ? *level : 1.0;
		v->Local2Global (*pos, gpos);
		v->GlobalRot (*dir, gdir);
	}
	lvl = LevelAlpha (lvl) * AtmAlpha (rho);
	return true;
}

void VkParticleStream::Update (double dt)
{
	if (dt <= 0.0) return;
	simTime += dt;
	VECTOR3 src, dir, vel;
	double alpha;
	if (!EnsureRing () || !SourceState (src, dir, vel, alpha) || alpha < 0.01) {
		havePrev = false;
		emitAccum = 0.0;
		return;
	}
	if (!havePrev) prevSrc = src - vel * dt;
	havePrev = true;
	// When the source moves through the air quickly, more particles are
	// emitted, so that the trail stays continuous (spacing of about twice
	// the particle size) instead of breaking into isolated puffs; their
	// opacity is reduced to keep the trail's density.
	double rate = spec.srcrate, alphaScale = 1.0;
	if (kind != Kind::Reentry && hAtmRef && spec.srcsize > 0.0) {
		VECTOR3 air;
		if (oapiGetVesselInterface (hRef)->GetAirspeedVector (FRAME_GLOBAL, air)) {
			double needed = length (air) / (2.0 * spec.srcsize);
			if (needed > rate) {
				alphaScale = std::max (0.35, std::sqrt (rate / needed));
				rate = std::min (needed, 60.0);
			}
		}
	}
	// more, smaller particles: about the same covered area and density
	double D = sys->density, sizeScale = std::pow (D, -1.0 / 3.0);
	emitAccum += rate * D * dt;
	uint32_t n = (uint32_t)emitAccum;
	emitAccum -= n;
	if (!n) { prevSrc = src; return; }
	ParticleSystem::Emission e{};
	e.p0 = prevSrc, e.p1 = src;
	e.vel = vel, e.dir = dir;
	// (the stream direction is the thrust direction: the exhaust leaves the
	// other way; srcspread randomises the velocity by up to +-v0*srcspread/2
	// per axis)
	e.v0 = spec.v0, e.spread = spec.srcspread * spec.v0;
	e.lifetime = spec.lifetime, e.dt = dt;
	e.size = spec.srcsize * sizeScale, e.growth = spec.growthrate * sizeScale;
	e.alpha = alpha * alphaScale * std::min (1.0, 1.3 / (D * sizeScale * sizeScale));
	// exhaust gas takes on the air's velocity quickly (as the plume bends)
	e.slowdown = !hAtmRef ? 0.0 : kind == Kind::Exhaust
		? std::max (spec.atmslowdown, ParticleSystem::ExhaustDrag (oapiGetVesselInterface (hRef)->GetAtmDensity ()))
		: spec.atmslowdown;
	e.first = first, e.capacity = capacity, e.head = head, e.count = std::min (n, capacity);
	e.emissive = spec.ltype == PARTICLESTREAMSPEC::EMISSIVE;
	e.reentry = kind == Kind::Reentry;
	sys->Queue (e);
	head = (head + e.count) % capacity;
	lastEmit = simTime;
	prevSrc = src;
}

void VkParticleStream::Render (Scene *scene)
{
	if (!allocated || sys != client->Particles () || simTime - lastEmit > 1.5 * spec.lifetime + 0.1) return;
	sys->Draw (first, capacity, tex, spec.ltype == PARTICLESTREAMSPEC::EMISSIVE, scene->FrameSet ());
}

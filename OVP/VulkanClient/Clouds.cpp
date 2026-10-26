// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Clouds.h"
#include "PlanetSurface.h"
#include "VkTexture.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

#include "cloudnoise.comp.h"
#include "cloudmap.vert.h"
#include "cloudmap.frag.h"
#include "clouds.comp.h"
#include "ggclouds.comp.h"
#include <cstring>
#include <string>
#include <sstream>
#include <fstream>
#include "cloudcomp.frag.h"
#include "post.vert.h"

namespace {
	constexpr uint32_t SHAPE_SIZE = 128, DETAIL_SIZE = 32, MAP_SIZE = 2048;
	const VkFormat OUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

	struct MarchParams {
		float shell[4], mapCentre[4], mapEast[4], mapNorth[4], ambient[4], proj[4], noise[4];
	};

	// ggclouds.comp's parameters (std430: arrays of vec4)
	constexpr int GG_LAYERS = 4, GG_STORMS = 16, GG_TOWERS = 64, GG_FLASHES = 8, GG_BOLTV = 128;
	struct GGParams {
		float layer[GG_LAYERS][4], albedo[GG_LAYERS][4], lmisc[GG_LAYERS][4];
		float haze[4], hazeH[4];
		float jets[64];
		float storm[GG_STORMS][4], stormP[GG_STORMS][4];
		float misc[4], misc2[4];
		float tower[GG_TOWERS][4], towerP[GG_TOWERS][4];
		float flash[GG_FLASHES][4]; // position (planet frame), brightness
		float flashInfo[4];         // count, diffusion length [m]
		float prevRot[3][4];        // last frame's camera frame -> planet frame (columns)
		float prevCam[4];           // its position (planet frame); w: history valid
		float pres[16];             // log2 of the pressure [bar] at 16 radii (presInfo)
		float presInfo[4];          // the first of them [m], their spacing [m]
		float gasK[4], rayK[4];     // the gas's absorption and Rayleigh scattering per bar (rgb)
		float vortex[4];            // storms' swirl speed [m/s], core radius (share), tower spin speed [m/s], spiral twist [rad]
		float bolt[GG_BOLTV][4];    // lightning channels: vertices (planet frame), brightness (negative: a channel's first vertex)
		float boltInfo[4];          // vertex count
	};

	uint32_t Pcg (uint32_t v)
	{
		uint32_t state = v * 747796405u + 2891336453u;
		uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}
	double Unit16 (uint32_t h) { return (h & 0xffffu) / 65535.0; }
	struct GGPush { float proj[4], ambient[4], slot[4]; };
	// a layout transition of one mip level of a colour image
	void LevelBarrier (VkCommandBuffer cmd, VkImage im, uint32_t level, VkImageLayout from, VkImageLayout to,
		VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
	{
		VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		b.srcStageMask = srcStage, b.srcAccessMask = srcAccess, b.dstStageMask = dstStage, b.dstAccessMask = dstAccess;
		b.oldLayout = from, b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = im;
		b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1 };
		VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
		vkCmdPipelineBarrier2 (cmd, &di);
	}
}

vkc::Image VolumetricClouds::CreateImage (VkImageType type, VkFormat fmt, VkExtent3D e, VkImageUsageFlags usage, uint32_t mips)
{
	vkc::Image im;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = type;
	ici.format = fmt;
	ici.extent = e;
	ici.mipLevels = mips;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = im.image;
	vci.viewType = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
	vci.format = fmt;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
	return im;
}

bool VolumetricClouds::Init (vkc::Context *c, TextureManager *tm, VkDescriptorSetLayout atmLayout)
{
	ctx = c, texmgr = tm;
	if (const char *e = getenv ("ORBITER_VK_CLOUDS")) enabled = atoi (e) != 0;

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &repeatSampler));
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &clampSampler));
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sci.maxLod = VK_LOD_CLAMP_NONE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &mipSampler));

	// noise volumes, generated once
	// (with mip chains, for the gas giants' march: the noise filtered to
	// the pixels' footprint far away; the views of the other users take
	// only the full resolution)
	const uint32_t sizes[2] = { SHAPE_SIZE, DETAIL_SIZE };
	vkc::Image *noise[2] = { &shapeNoise, &detailNoise };
	VkImageView *mipViews[2] = { &shapeMipView, &detailMipView };
	uint32_t mips[2];
	for (int i = 0; i < 2; i++) {
		mips[i] = 1;
		while ((sizes[i] >> mips[i]) > 0) mips[i]++;
		*noise[i] = CreateImage (VK_IMAGE_TYPE_3D, VK_FORMAT_R8_UNORM, { sizes[i], sizes[i], sizes[i] },
			VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, mips[i]);
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = noise[i]->image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
		vci.format = VK_FORMAT_R8_UNORM;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips[i], 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, mipViews[i]));
	}
	{
		VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = 1;
		lci.pBindings = &b;
		VkDescriptorSetLayout l;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &l));
		VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(int) };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 1, plci.pSetLayouts = &l;
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VkPipelineLayout pl;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &pl));
		VkShaderModule cs = ctx->CreateShaderModule (spv_cloudnoise_comp, spv_cloudnoise_comp_size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = pl;
		VkPipeline pipe;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
		VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };
		VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pci.maxSets = 2, pci.poolSizeCount = 1, pci.pPoolSizes = &ps;
		VkDescriptorPool tmp;
		VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &tmp));
		VkDescriptorSetLayout ls[2] = { l, l };
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = tmp, dai.descriptorSetCount = 2, dai.pSetLayouts = ls;
		VkDescriptorSet s[2];
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, s));
		VkDescriptorImageInfo ii[2] = { { VK_NULL_HANDLE, shapeNoise.view, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, detailNoise.view, VK_IMAGE_LAYOUT_GENERAL } };
		VkWriteDescriptorSet wr[2]{};
		for (int i = 0; i < 2; i++) {
			wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			wr[i].dstSet = s[i], wr[i].descriptorCount = 1;
			wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[i].pImageInfo = &ii[i];
		}
		vkUpdateDescriptorSets (ctx->device, 2, wr, 0, nullptr);
		VkImage imgs[2] = { shapeNoise.image, detailNoise.image };
		ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
			vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
			for (int i = 0; i < 2; i++) {
				LevelBarrier (cmd, imgs[i], 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
					VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
				int p[2] = { (int)(i ? DETAIL_SIZE : SHAPE_SIZE), i };
				vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s[i], 0, nullptr);
				vkCmdPushConstants (cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), p);
				uint32_t g = (uint32_t)(p[0] + 3) / 4;
				vkCmdDispatch (cmd, g, g, g);
				// the mip chain: each level halved from the one above (the
				// volume tileable: 2:1 averages of whole texel pairs)
				LevelBarrier (cmd, imgs[i], 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
				for (uint32_t m = 1; m < mips[i]; m++) {
					LevelBarrier (cmd, imgs[i], m, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
					int32_t a = (int32_t)(sizes[i] >> (m - 1)), b = (int32_t)(sizes[i] >> m);
					VkImageBlit bl{};
					bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1 };
					bl.srcOffsets[1] = { a, a, a };
					bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 };
					bl.dstOffsets[1] = { b, b, b };
					vkCmdBlitImage (cmd, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
					LevelBarrier (cmd, imgs[i], m, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
				}
				for (uint32_t m = 0; m < mips[i]; m++)
					LevelBarrier (cmd, imgs[i], m, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			}
		});
		vkDestroyDescriptorPool (ctx->device, tmp, nullptr);
		vkDestroyPipeline (ctx->device, pipe, nullptr);
		vkDestroyShaderModule (ctx->device, cs, nullptr);
		vkDestroyPipelineLayout (ctx->device, pl, nullptr);
		vkDestroyDescriptorSetLayout (ctx->device, l, nullptr);
	}

	// coverage map (rests in shader-read layout)
	coverage = CreateImage (VK_IMAGE_TYPE_2D, VK_FORMAT_R8_UNORM, { MAP_SIZE, MAP_SIZE, 1 }, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	VkImage cov = coverage.image;
	ctx->ImmediateSubmit ([cov](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, cov, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});

	auto graphicsPipe = [this](const unsigned char *vsc, size_t vss, const unsigned char *fsc, size_t fss, VkPipelineLayout layout,
		VkFormat fmt, bool tileVertices, bool scene) {
		VkShaderModule vs = ctx->CreateShaderModule (vsc, vss), fs = ctx->CreateShaderModule (fsc, fss);
		VkPipelineShaderStageCreateInfo stages[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" } };
		VkVertexInputBindingDescription bind{ 0, sizeof(NTVERTEX), VK_VERTEX_INPUT_RATE_VERTEX };
		VkVertexInputAttributeDescription attr[3] = {
			{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(NTVERTEX, x) },
			{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(NTVERTEX, nx) },
			{ 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NTVERTEX, tu) } };
		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		if (tileVertices) {
			vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &bind;
			vi.vertexAttributeDescriptionCount = 3, vi.pVertexAttributeDescriptions = attr;
		}
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vps.viewportCount = 1, vps.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = scene ? ctx->sceneSamples : VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		VkPipelineColorBlendAttachmentState cba{};
		cba.colorWriteMask = 0xF;
		if (scene) { // src + dst * src.a
			cba.blendEnable = VK_TRUE;
			cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE, cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			cba.colorBlendOp = VK_BLEND_OP_ADD;
			cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			cba.alphaBlendOp = VK_BLEND_OP_ADD;
		}
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1, cb.pAttachments = &cba;
		VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
		VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		rci.colorAttachmentCount = 1, rci.pColorAttachmentFormats = &fmt;
		if (scene) rci.depthAttachmentFormat = ctx->depthFormat;
		VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gpi.pNext = &rci;
		gpi.stageCount = 2, gpi.pStages = stages;
		gpi.pVertexInputState = &vi, gpi.pInputAssemblyState = &ia, gpi.pViewportState = &vps;
		gpi.pRasterizationState = &rs, gpi.pMultisampleState = &ms, gpi.pDepthStencilState = &ds;
		gpi.pColorBlendState = &cb, gpi.pDynamicState = &dy, gpi.layout = layout;
		VkPipeline pipe;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));
		vkDestroyShaderModule (ctx->device, vs, nullptr);
		vkDestroyShaderModule (ctx->device, fs, nullptr);
		return pipe;
	};

	// coverage pass: tile textures (texture manager sets), tile -> map transform
	VkDescriptorSetLayout texLayout = texmgr->Layout();
	VkPushConstantRange cpcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, 20 * sizeof(float) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1, plci.pSetLayouts = &texLayout;
	plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &cpcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &coverageLayout));
	coveragePipe = graphicsPipe (spv_cloudmap_vert, spv_cloudmap_vert_size, spv_cloudmap_frag, spv_cloudmap_frag_size,
		coverageLayout, VK_FORMAT_R8_UNORM, true, false);

	// ray-march: coverage, noise, prepass, output; the planet's atmosphere
	VkDescriptorSetLayoutBinding mb[10];
	for (uint32_t i = 0; i < 4; i++) mb[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	mb[4] = { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	mb[5] = { 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // (gas giant decks)
	// (gas giant decks' history: light and distance, read and written, two each)
	mb[6] = { 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	mb[7] = { 7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	mb[8] = { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	mb[9] = { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 10, lci.pBindings = mb;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &marchSetLayout));
	VkDescriptorSetLayout ml[2] = { marchSetLayout, atmLayout };
	VkPushConstantRange mpcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MarchParams) };
	plci.setLayoutCount = 2, plci.pSetLayouts = ml;
	plci.pPushConstantRanges = &mpcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &marchLayout));
	VkShaderModule cs = ctx->CreateShaderModule (spv_clouds_comp, spv_clouds_comp_size);
	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
	cpi.layout = marchLayout;
	VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &marchPipe));
	vkDestroyShaderModule (ctx->device, cs, nullptr);
	cs = ctx->CreateShaderModule (spv_ggclouds_comp, spv_ggclouds_comp_size);
	cpi.stage.module = cs;
	VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &ggPipe));
	vkDestroyShaderModule (ctx->device, cs, nullptr);
	ggBuf = ctx->CreateBuffer (vkc::FRAMES_IN_FLIGHT * sizeof(GGParams), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	memset (ggBuf.mapped, 0, vkc::FRAMES_IN_FLIGHT * sizeof(GGParams));

	// composite
	VkDescriptorSetLayoutBinding cbnd{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	lci.bindingCount = 1, lci.pBindings = &cbnd;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &compSetLayout));
	plci.setLayoutCount = 1, plci.pSetLayouts = &compSetLayout;
	plci.pushConstantRangeCount = 0;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &compLayout));
	compPipe = graphicsPipe (spv_post_vert, spv_post_vert_size, spv_cloudcomp_frag, spv_cloudcomp_frag_size,
		compLayout, ctx->sceneFormat, false, true);

	VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2, pci.poolSizeCount = 3, pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	return true;
}

void VolumetricClouds::Shutdown ()
{
	if (!ctx) return;
	vkDeviceWaitIdle (ctx->device);
	VkPipeline pipes[] = { marchPipe, compPipe, coveragePipe, ggPipe };
	ctx->DestroyBuffer (ggBuf);
	for (int k = 0; k < 2; k++) ctx->DestroyImage (ggHist[k]), ctx->DestroyImage (ggDepth[k]);
	for (auto p : pipes) vkDestroyPipeline (ctx->device, p, nullptr);
	VkPipelineLayout pls[] = { marchLayout, compLayout, coverageLayout };
	for (auto p : pls) vkDestroyPipelineLayout (ctx->device, p, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, marchSetLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, compSetLayout, nullptr);
	vkDestroySampler (ctx->device, repeatSampler, nullptr);
	vkDestroySampler (ctx->device, clampSampler, nullptr);
	vkDestroySampler (ctx->device, mipSampler, nullptr);
	vkDestroyImageView (ctx->device, shapeMipView, nullptr);
	vkDestroyImageView (ctx->device, detailMipView, nullptr);
	ctx->DestroyImage (shapeNoise);
	ctx->DestroyImage (detailNoise);
	ctx->DestroyImage (coverage);
	ctx->DestroyImage (output);
	ctx = nullptr;
}

void VolumetricClouds::CreateOutput (VkImageView prepass)
{
	vkDeviceWaitIdle (ctx->device);
	ctx->DestroyImage (output);
	for (int k = 0; k < 2; k++) ctx->DestroyImage (ggHist[k]), ctx->DestroyImage (ggDepth[k]);
	vkResetDescriptorPool (ctx->device, pool, 0);
	VkExtent2D r = ctx->RenderExtent ();
	outExtent = { (r.width + 1) / 2, (r.height + 1) / 2 };
	output = CreateImage (VK_IMAGE_TYPE_2D, OUT_FORMAT, { outExtent.width, outExtent.height, 1 }, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	VkImage img = output.image;
	ctx->ImmediateSubmit ([img](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	// (the gas giant decks' history: in the general layout, read and written)
	for (int k = 0; k < 2; k++) {
		ggHist[k] = CreateImage (VK_IMAGE_TYPE_2D, OUT_FORMAT, { outExtent.width, outExtent.height, 1 }, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		ggDepth[k] = CreateImage (VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, { outExtent.width, outExtent.height, 1 }, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		VkImage hi = ggHist[k].image, di = ggDepth[k].image;
		ctx->ImmediateSubmit ([hi, di](VkCommandBuffer cmd) {
			for (VkImage im : { hi, di })
				vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
					VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
		});
	}
	ggHistValid = false;
	VkDescriptorSetLayout l[2] = { marchSetLayout, compSetLayout };
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool, dai.descriptorSetCount = 2, dai.pSetLayouts = l;
	VkDescriptorSet s[2];
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, s));
	marchSet = s[0], compSet = s[1];
	VkDescriptorImageInfo ii[6] = {
		{ clampSampler, coverage.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ mipSampler, shapeMipView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ mipSampler, detailMipView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ clampSampler, prepass, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL },
		{ clampSampler, output.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	VkWriteDescriptorSet wr[6]{};
	for (uint32_t i = 0; i < 6; i++) {
		wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[i].dstSet = i < 5 ? marchSet : compSet;
		wr[i].dstBinding = i < 5 ? i : 0;
		wr[i].descriptorCount = 1;
		wr[i].descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[i].pImageInfo = &ii[i];
	}
	vkUpdateDescriptorSets (ctx->device, 6, wr, 0, nullptr);
	VkDescriptorBufferInfo gbi{ ggBuf.buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet gw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	gw.dstSet = marchSet, gw.dstBinding = 5, gw.descriptorCount = 1;
	gw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, gw.pBufferInfo = &gbi;
	vkUpdateDescriptorSets (ctx->device, 1, &gw, 0, nullptr);
	{
		VkDescriptorImageInfo hs[2], ds[2], hw[2], dw[2];
		for (int k = 0; k < 2; k++) {
			hs[k] = { clampSampler, ggHist[k].view, VK_IMAGE_LAYOUT_GENERAL };
			ds[k] = { clampSampler, ggDepth[k].view, VK_IMAGE_LAYOUT_GENERAL };
			hw[k] = { VK_NULL_HANDLE, ggHist[k].view, VK_IMAGE_LAYOUT_GENERAL };
			dw[k] = { VK_NULL_HANDLE, ggDepth[k].view, VK_IMAGE_LAYOUT_GENERAL };
		}
		VkWriteDescriptorSet w4[4]{};
		const VkDescriptorImageInfo *inf[4] = { hs, ds, hw, dw };
		for (int k = 0; k < 4; k++) {
			w4[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w4[k].dstSet = marchSet, w4[k].dstBinding = 6 + k, w4[k].descriptorCount = 2;
			w4[k].descriptorType = k < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			w4[k].pImageInfo = inf[k];
		}
		vkUpdateDescriptorSets (ctx->device, 4, w4, 0, nullptr);
	}
	outPrepass = prepass;
	generation = ctx->SwapchainGeneration ();
}

void VolumetricClouds::Render (const Frame &f)
{
	active = false;
	if (!enabled || !f.clouds) return;
	if (generation != ctx->SwapchainGeneration () || outPrepass != f.prepass) CreateOutput (f.prepass);

	// shell around the 2D layer's altitude
	double R = f.planetRadius;
	double base = R + std::max (f.cloudAlt - 1500.0, 300.0), top = R + f.cloudAlt + 1500.0;

	// coverage map: square of +-RANGE around the point below the camera,
	// in the local horizontal frame (planet frame axes: y towards the pole)
	VECTOR3 up = unit (f.camPlanet);
	VECTOR3 ref = fabs (up.y) < 0.999 ? _V(0, 1, 0) : _V(1, 0, 0);
	VECTOR3 east = unit (crossp (ref, up)), north = crossp (up, east);
	VECTOR3 C = up * (R + f.cloudAlt);
	double H = RANGE;
	Mat4 toMap = Mat4::Identity ();
	toMap.m[0] = east.x / H, toMap.m[4] = east.y / H, toMap.m[8] = east.z / H, toMap.m[12] = -dotp (C, east) / H;
	toMap.m[1] = north.x / H, toMap.m[5] = north.y / H, toMap.m[9] = north.z / H, toMap.m[13] = -dotp (C, north) / H;
	// depth: height above the map plane, clipped beyond +-50 km (the far side
	// of the planet)
	const double DZ = 50e3;
	toMap.m[2] = up.x / (2.0 * DZ), toMap.m[6] = up.y / (2.0 * DZ), toMap.m[10] = up.z / (2.0 * DZ), toMap.m[14] = 0.5 - dotp (C, up) / (2.0 * DZ);
	ctx->BindTarget ({ coverage.image, coverage.view, VK_FORMAT_R8_UNORM, { MAP_SIZE, MAP_SIZE } });
	VkCommandBuffer cmd = ctx->Cmd ();
	VkClearAttachment ca{ VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{ 0.0f, 0.0f, 0.0f, 0.0f }}} };
	VkClearRect cr{ { { 0, 0 }, { MAP_SIZE, MAP_SIZE } }, 0, 1 };
	vkCmdClearAttachments (cmd, 1, &ca, 1, &cr);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, coveragePipe);
	f.clouds->RenderCoverage (toMap, coverageLayout, C, H * 1.5);
	ctx->EndTarget ();

	// ray-march
	cmd = ctx->Cmd ();
	vkc::ImageBarrier (cmd, output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
	MarchParams p{};
	p.shell[0] = (float)base, p.shell[1] = (float)top, p.shell[2] = (float)RANGE, p.shell[3] = brightness * 3.14159265f;
	p.mapCentre[0] = (float)C.x, p.mapCentre[1] = (float)C.y, p.mapCentre[2] = (float)C.z, p.mapCentre[3] = (float)H;
	p.mapEast[0] = (float)east.x, p.mapEast[1] = (float)east.y, p.mapEast[2] = (float)east.z, p.mapEast[3] = 1.0f;
	p.mapNorth[0] = (float)north.x, p.mapNorth[1] = (float)north.y, p.mapNorth[2] = (float)north.z, p.mapNorth[3] = extinction;
	p.ambient[0] = f.ambient[0], p.ambient[1] = f.ambient[1], p.ambient[2] = f.ambient[2], p.ambient[3] = (float)(frameNo++ % 64);
	p.proj[0] = f.fx, p.proj[1] = f.fy, p.proj[2] = (float)outExtent.width, p.proj[3] = (float)outExtent.height;
	p.noise[0] = f.windOffset[0], p.noise[1] = f.windOffset[1], p.noise[2] = f.windOffset[2], p.noise[3] = 1.0f / 6000.0f;
	VkDescriptorSet sets[2] = { marchSet, f.atmSet };
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, marchPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, marchLayout, 0, 2, sets, 0, nullptr);
	vkCmdPushConstants (cmd, marchLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
	vkCmdDispatch (cmd, (outExtent.width + 7) / 8, (outExtent.height + 7) / 8, 1);
	vkc::ImageBarrier (cmd, output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	memcpy (shadowMap, p.mapCentre, sizeof(shadowMap));
	memcpy (shadowNoise, p.noise, sizeof(shadowNoise));
	shadowShell[0] = p.shell[0], shadowShell[1] = p.shell[1], shadowShell[2] = extinction, shadowShell[3] = p.mapEast[3];
	active = true;
}

void VolumetricClouds::Composite ()
{
	if (!active) return;
	VkCommandBuffer cmd = ctx->Cmd ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compLayout, 0, 1, &compSet, 0, nullptr);
	vkCmdDraw (cmd, 3, 1, 0, 0);
}

// ======================================================================
// Gas giants

double VolumetricClouds::Decks::Top () const
{
	double t = 0.0;
	for (const Deck &d : decks) t = std::max (t, d.top);
	return std::max (t, towerTop);
}

double VolumetricClouds::Decks::CloudTop () const
{
	double t = -1e30;
	for (const Deck &d : decks) if (d.ext >= 1e-4) t = std::max (t, d.top);
	return t > -1e30 ? t : 0.0;
}

double VolumetricClouds::Decks::ColumnAbove (double alt, double mu) const
{
	double tau = 0.0;
	for (const Deck &d : decks)
		tau += d.ext * 0.35 * d.coverage * std::clamp (d.top - std::max (alt, d.base), 0.0, d.top - d.base);
	tau += hazeExt * hazeH * exp (-alt / hazeH);
	return tau / std::max (mu, 0.08);
}

double VolumetricClouds::Decks::Light (double alt, double mu, double bar, double *direct) const
{
	const double GC = 0.85;
	double ta = gasAbs[1] * bar, ts = gasRay[1] * bar, te = ta + ts;
	for (const Deck &d : decks) {
		double tau = d.ext * 0.35 * d.coverage * std::clamp (d.top - std::max (alt, d.base), 0.0, d.top - d.base);
		te += tau, ta += tau * (1.0 - d.albedo[1]), ts += tau * d.albedo[1] * (1.0 - GC);
	}
	double th = hazeExt * hazeH * exp (-alt / hazeH);
	te += th, ta += th * (1.0 - haze[1]), ts += th * haze[1] * 0.4;
	double m = std::max (mu, 0.08);
	double dir = exp (-te / m);
	double tot = exp (-sqrt (3.0 * ta * (ta + ts))) / (1.0 + 0.75 * ts / m);
	if (direct) *direct = dir;
	return std::max (dir, tot);
}

bool VolumetricClouds::LoadDecks (const char *planet, Decks &dk)
{
	std::ifstream ifs (std::string ("Config/VK/GasGiant/") + planet + ".cfg");
	if (!ifs) return false;
	dk = Decks();
	std::string line;
	while (std::getline (ifs, line)) {
		size_t c = line.find (';');
		if (c != std::string::npos) line.erase (c);
		std::istringstream ss (line);
		std::string key;
		if (!(ss >> key)) continue;
		if (key == "LAYER") {
			Deck d{};
			int towers = 0;
			if (ss >> d.base >> d.top >> d.ext >> d.scale >> d.albedo[0] >> d.albedo[1] >> d.albedo[2] >> d.coverage >> d.contrast >> towers) {
				d.towers = towers != 0;
				if (!(ss >> d.holes)) d.holes = 0.0f;
				if (dk.decks.size() < (size_t)GG_LAYERS) dk.decks.push_back (d);
			}
		} else if (key == "HAZE") ss >> dk.haze[0] >> dk.haze[1] >> dk.haze[2] >> dk.hazeExt >> dk.hazeH;
		else if (key == "TOWERTOP") ss >> dk.towerTop;
		else if (key == "STRETCH") ss >> dk.stretch;
		else if (key == "TINT") ss >> dk.tint;
		else if (key == "GAS") ss >> dk.gasAbs[0] >> dk.gasAbs[1] >> dk.gasAbs[2] >> dk.gasRay[0] >> dk.gasRay[1] >> dk.gasRay[2];
		else if (key == "VORTEX") ss >> dk.vortexV >> dk.vortexCore >> dk.towerSpin >> dk.vortexTwist;
	}
	return !dk.decks.empty();
}

void VolumetricClouds::RenderGasGiant (const GasGiantFrame &f)
{
	if (!active) ggHistValid = false; // (not drawn last frame: no history)
	active = false;
	if (!enabled || !f.decks || f.decks->decks.empty()) return;
	if (generation != ctx->SwapchainGeneration () || outPrepass != f.prepass) CreateOutput (f.prepass);
	const Decks &dk = *f.decks;
	const double R = f.radius;
	const double RANGE = 2000e3; // [m] the march's reach (the decks fade into the planet map beyond)

	uint32_t fi = ctx->FrameIndex ();
	GGParams &g = ((GGParams*)ggBuf.mapped)[fi];
	memset (&g, 0, sizeof(g));
	// the jets at the clouds, and their share at each deck's height
	double cl = length (f.camPlanet);
	double clng = atan2 (f.camPlanet.z, f.camPlanet.x), clat = asin (std::clamp (f.camPlanet.y / cl, -1.0, 1.0));
	double ujMax = 0.0, latMax = 0.0;
	for (int i = 0; i < 64; i++) {
		double lat = (-90.0 + 180.0 * i / 63.0) * RAD;
		g.jets[i] = (float)oapiGetWindVector (f.hPlanet, clng, lat, 0.0, 0).x;
		if (fabs (g.jets[i]) > ujMax) ujMax = fabs (g.jets[i]), latMax = lat;
	}
	for (size_t i = 0; i < dk.decks.size(); i++) {
		const Deck &d = dk.decks[i];
		g.layer[i][0] = (float)(R + d.base), g.layer[i][1] = (float)(R + d.top);
		g.layer[i][2] = (float)d.ext, g.layer[i][3] = (float)(1.0 / d.scale);
		for (int k = 0; k < 3; k++) g.albedo[i][k] = d.albedo[k];
		g.albedo[i][3] = d.coverage;
		g.lmisc[i][0] = d.contrast, g.lmisc[i][1] = d.towers ? 1.0f : 0.0f, g.lmisc[i][3] = d.holes;
		double um = oapiGetWindVector (f.hPlanet, clng, latMax, 0.5 * (d.base + d.top), 0).x;
		g.lmisc[i][2] = ujMax > 0.0 ? (float)(um / oapiGetWindVector (f.hPlanet, clng, latMax, 0.0, 0).x) : 1.0f;
	}
	for (int k = 0; k < 3; k++) g.haze[k] = dk.haze[k];
	g.haze[3] = (float)dk.hazeExt;
	g.hazeH[0] = (float)dk.hazeH, g.hazeH[1] = (float)dk.decks.size();
	g.hazeH[2] = (float)fmod (f.time, 1e5), g.hazeH[3] = (float)RANGE;
	// the storm cells within range (their towers)
	STORMCELL cells[GG_STORMS];
	int ns = oapiGetStormCells (f.hPlanet, clng, clat, RANGE, cells, GG_STORMS);
	for (int i = 0; i < ns; i++) {
		double cla = cos (cells[i].lat);
		g.storm[i][0] = (float)(cla * cos (cells[i].lng)), g.storm[i][1] = (float)sin (cells[i].lat), g.storm[i][2] = (float)(cla * sin (cells[i].lng));
		g.storm[i][3] = (float)cells[i].radius;
		g.stormP[i][0] = (float)cells[i].strength;
		g.stormP[i][1] = (float)(cells[i].id & 0xffffffu); // (exact in a float)
	}
	g.misc[0] = (float)ns, g.misc[1] = 1.0f, g.misc[2] = (float)R, g.misc[3] = (float)(R + dk.towerTop);
	// the storms' towers: six per cell about its centre (stable: hashed
	// from the cell's identity), rising from the lowest deck's base
	int nt = 0;
	double baseR = R + dk.decks.back().base, topR = R + dk.towerTop;
	for (int i = 0; i < ns && nt < GG_TOWERS; i++) {
		VECTOR3 c = _V(g.storm[i][0], g.storm[i][1], g.storm[i][2]);
		VECTOR3 e = unit (_V(-c.z, 0.0, c.x)), n = crossp (c, e);
		double Rc = cells[i].radius, s = cells[i].strength;
		for (int k = 0; k < 6 && nt < GG_TOWERS; k++) {
			uint32_t h0 = Pcg (cells[i].id * 8u + (uint32_t)k), h1 = Pcg (h0);
			double th = PI2 * Unit16 (h0), dist = 0.45 * Rc * sqrt (Unit16 (h0 >> 16));
			VECTOR3 tdir = unit (c + (e * cos (th) + n * sin (th)) * (dist / R));
			g.tower[nt][0] = (float)tdir.x, g.tower[nt][1] = (float)tdir.y, g.tower[nt][2] = (float)tdir.z;
			g.tower[nt][3] = (float)(Rc * (0.05 + 0.06 * Unit16 (h1)) * (0.6 + 0.4 * s));
			g.towerP[nt][0] = (float)(baseR + (topR - baseR) * (0.55 + 0.45 * Unit16 (h1 >> 16)) * (0.7 + 0.3 * s));
			g.towerP[nt][1] = (float)s;
			g.towerP[nt][2] = (float)(7.1 * k + 3.3 * (cells[i].id % 97));
			g.towerP[nt][3] = 0.0f;
			nt++;
		}
		// (the cell's tallest tower carries its anvil: one per storm)
		int first = nt - 6 < 0 ? 0 : nt - 6, best = first;
		for (int k = first; k < nt; k++) if (g.towerP[k][0] > g.towerP[best][0]) best = k;
		if (nt > first) g.towerP[best][3] = 1.0f;
	}
	g.misc2[3] = (float)nt;

	// Lightning: each tower flashes at random (more often the stronger its
	// storm), decided per 0.1 s of simulated time from hashes (the same
	// flashes at any frame rate); a flash lasts 0.2-0.5 s, two to four
	// strokes, somewhere in the tower's column between its base (the water
	// deck) and 20 km below the 1-bar level
	flashes.clear ();
	{
		const double SLOT = 0.1, MAXDUR = 0.6;
		double t = f.time;
		VECTOR3 pp;
		MATRIX3 prot;
		oapiGetGlobalPos (f.hPlanet, &pp);
		oapiGetRotationMatrix (f.hPlanet, &prot);
		struct Lit { VECTOR3 pl; double b; uint32_t seed; VECTOR3 up, out, n, edge; };
		std::vector<Lit> lit;
		for (int i = 0; i < nt; i++) {
			double s = g.towerP[i][1];
			double rate = 0.5 * s * s; // [1/s] (Juno: flashes every second or so in active storms)
			VECTOR3 c = _V(g.tower[i][0], g.tower[i][1], g.tower[i][2]);
			if (length (c * R - f.camPlanet) > 600e3) continue;
			VECTOR3 e = unit (_V(-c.z, 0.0, c.x)), n = crossp (c, e);
			uint32_t id = Pcg ((uint32_t)(g.towerP[i][2] * 10.0f) + 0x9e37u); // (the tower's identity: stable)
			for (int64_t k = (int64_t)floor ((t - MAXDUR) / SLOT); k <= (int64_t)floor (t / SLOT); k++) {
				uint32_t h0 = Pcg (id ^ Pcg ((uint32_t)k)), h1 = Pcg (h0), h2 = Pcg (h1), h3 = Pcg (h2);
				if (Unit16 (h0) >= rate * SLOT) continue;
				double t0 = (k + Unit16 (h0 >> 16)) * SLOT, dur = 0.2 + 0.3 * Unit16 (h1);
				double age = t - t0;
				if (age < 0.0 || age > dur) continue;
				// (strokes: sharp rises, quick decays)
				int ns = 2 + (int)(Unit16 (h1 >> 16) * 2.99);
				double b = 0.0;
				for (int j = 0; j < ns; j++) {
					double ts = dur * j / ns + 0.03 * Unit16 (Pcg (h3 + j));
					if (age >= ts) b = std::max (b, exp (-(age - ts) / 0.04));
				}
				double alt = (dk.decks.back().base + 10e3) + (std::min (-20e3, dk.towerTop) - dk.decks.back().base - 10e3) * Unit16 (h2);
				double lean = 0.35 * (alt - dk.decks.back().base);
				double rt = g.tower[i][3], a = PI2 * Unit16 (h2 >> 16), rr = 0.5 * rt * Unit16 (h3);
				VECTOR3 pl = c * (R + alt) + e * (lean + rr * cos (a)) + n * (rr * sin (a));
				VECTOR3 out = unit (e * cos (a) + n * sin (a));
				lit.push_back ({ pl, b * (0.5 + 0.5 * Unit16 (h3 >> 16)), Pcg (h3 ^ 0x5bd1e995u), c, out, unit (n * cos (a) - e * sin (a)),
					c * (R + alt) + e * lean + out * (0.9 * rt) }); // (the tower's flank, out from the flash)
			}
		}
		// (a test flash ahead of the camera, lit throughout: tuning the
		// channels' look; ORBITER_VK_GG_TESTFLASH = its distance [m])
		if (const char *tf = getenv ("ORBITER_VK_GG_TESTFLASH")) {
			VECTOR3 fwd = _V(f.viewToPlanet.m13, f.viewToPlanet.m23, f.viewToPlanet.m33), up = unit (f.camPlanet);
			VECTOR3 side = unit (crossp (up, fwd));
			VECTOR3 pl = f.camPlanet + fwd * atof (tf);
			lit.push_back ({ pl, 1.0, 0x1234567u, up, side, unit (crossp (side, up)), pl });
		}
		std::sort (lit.begin(), lit.end(), [&](const Lit &a, const Lit &b) {
			return length (a.pl - f.camPlanet) < length (b.pl - f.camPlanet); });
		int nf = std::min ((int)lit.size(), GG_FLASHES);
		for (int i = 0; i < nf; i++) {
			g.flash[i][0] = (float)lit[i].pl.x, g.flash[i][1] = (float)lit[i].pl.y, g.flash[i][2] = (float)lit[i].pl.z;
			g.flash[i][3] = (float)lit[i].b;
			flashes.push_back ({ pp + mul (prot, lit[i].pl), lit[i].b });
		}
		g.flashInfo[0] = (float)nf, g.flashInfo[1] = 5e3f;

		// the channels of the flashes near the camera: a stroke 10-25 km
		// long from the tower's flank by the flash into the clear air beside
		// it (outwards from its axis, tilted up or down), jagged
		// (midpoint displacement), with two branches (the same channel
		// through the flash's strokes)
		int nv = 0;
		auto rnd = [](uint32_t &h) { h = Pcg (h); return Unit16 (h) * 2.0 - 1.0; };
		const int NSEG = 32, NBR = 3, BRSEG = 9;
		for (int i = 0; i < nf && nv + NSEG + 1 + NBR * (BRSEG + 1) <= GG_BOLTV; i++) {
			const Lit &l = lit[i];
			if (l.b < 0.02 || length (l.pl - f.camPlanet) > 400e3) continue;
			uint32_t h = l.seed;
			double len = 10e3 + 15e3 * (0.5 + 0.5 * rnd (h));
			VECTOR3 axis = unit (l.out + l.up * (0.4 * rnd (h)) + l.n * (0.3 * rnd (h)));
			// (rough at all scales: the displacement falling off slower than
			// the segments shorten)
			VECTOR3 p[NSEG + 1];
			p[0] = l.edge, p[NSEG] = l.edge + axis * len;
			for (int step = NSEG; step > 1; step /= 2)
				for (int a = 0; a < NSEG; a += step) {
					double amp = 0.22 * len * pow ((double)step / NSEG, 0.7);
					p[a + step / 2] = (p[a] + p[a + step]) * 0.5 + l.up * (amp * rnd (h)) + l.n * (amp * rnd (h)) + axis * (0.3 * amp * rnd (h));
				}
			auto put = [&](const VECTOR3 &v, double b) {
				g.bolt[nv][0] = (float)v.x, g.bolt[nv][1] = (float)v.y, g.bolt[nv][2] = (float)v.z, g.bolt[nv][3] = (float)b; nv++; };
			for (int k = 0; k <= NSEG; k++) put (p[k], k == 0 ? -l.b : l.b);
			for (int br = 0; br < NBR; br++) {
				int a = 4 + (int)((0.5 + 0.5 * rnd (h)) * (NSEG - 8));
				VECTOR3 q = p[a], dir = unit (axis + l.up * rnd (h) + l.n * rnd (h));
				double seg = len * (0.25 + 0.15 * rnd (h)) / BRSEG;
				put (q, -0.4 * l.b);
				for (int k = 0; k < BRSEG; k++) {
					dir = unit (dir + (l.up * rnd (h) + l.n * rnd (h) + axis * rnd (h)) * 0.7);
					q += dir * seg;
					put (q, 0.4 * l.b * (1.0 - (double)k / (BRSEG + 1)));
				}
			}
		}
		g.boltInfo[0] = (float)nv;
	}
	g.misc2[0] = (float)dk.stretch, g.misc2[1] = (float)dk.tint, g.misc2[2] = (float)f.floorRadius;
	// the gas: pressure from the floor to above the decks, absorption and
	// scattering per bar
	{
		double r0 = f.floorRadius, r1 = R + std::max (dk.Top (), dk.towerTop) + 20e3, dr = (r1 - r0) / 15.0;
		for (int i = 0; i < 16; i++) {
			ATMPARAM prm;
			oapiGetPlanetAtmParams (f.hPlanet, r0 + dr * i, &prm);
			g.pres[i] = (float)std::log2 (std::max (prm.p, 1e-3) / 1e5);
		}
		g.presInfo[0] = (float)r0, g.presInfo[1] = (float)dr;
		for (int k = 0; k < 3; k++) g.gasK[k] = dk.gasAbs[k], g.rayK[k] = dk.gasRay[k];
		g.vortex[0] = (float)dk.vortexV, g.vortex[1] = (float)dk.vortexCore, g.vortex[2] = (float)dk.towerSpin, g.vortex[3] = (float)dk.vortexTwist;
	}

	// the last frame's camera (temporal accumulation; not after a jump)
	if (ggHistValid && length (f.camPlanet - ggPrevCam) > 50e3) ggHistValid = false;
	const MATRIX3 &pr = ggHistValid ? ggPrevRot : f.viewToPlanet;
	const double pm[9] = { pr.m11, pr.m21, pr.m31, pr.m12, pr.m22, pr.m32, pr.m13, pr.m23, pr.m33 }; // (columns)
	for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) g.prevRot[c][k] = (float)pm[c * 3 + k];
	VECTOR3 pc0 = ggHistValid ? ggPrevCam : f.camPlanet;
	g.prevCam[0] = (float)pc0.x, g.prevCam[1] = (float)pc0.y, g.prevCam[2] = (float)pc0.z, g.prevCam[3] = ggHistValid ? 1.0f : 0.0f;
	// (half the tiles marched per frame only with the camera nearly still:
	// moving fast, the reprojected tiles would not match the marched ones)
	// (still for several frames: a camera moved only every other frame
	// is not still)
	bool still = false;
	if (ggHistValid) {
		VECTOR3 fz = _V(f.viewToPlanet.m13, f.viewToPlanet.m23, f.viewToPlanet.m33), pz = _V(pr.m13, pr.m23, pr.m33);
		// (nor with lightning: the light changes from frame to frame)
		still = length (f.camPlanet - pc0) < 200.0 && dotp (fz, pz) > cos (0.05 * RAD) && g.flashInfo[0] == 0.0f
			&& oapiGetTimeAcceleration () < 2.0; // (nor with the clouds moving fast)
	}
	ggStill = still ? ggStill + 1 : 0;
	if (ggStill >= 4) g.prevCam[3] = 2.0f;

	VkCommandBuffer cmd = ctx->Cmd ();
	vkc::ImageBarrier (cmd, output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
	{
		// (the history written last frame, read now)
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2 (cmd, &dep);
	}
	GGPush p{};
	p.proj[0] = f.fx, p.proj[1] = f.fy, p.proj[2] = (float)outExtent.width, p.proj[3] = (float)outExtent.height;
	p.ambient[0] = f.sky[0], p.ambient[1] = f.sky[1], p.ambient[2] = f.sky[2], p.ambient[3] = (float)(frameNo++ % 64);
	p.slot[0] = (float)fi, p.slot[1] = (float)ggParity;
	VkDescriptorSet sets[2] = { marchSet, f.atmSet };
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ggPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, marchLayout, 0, 2, sets, 0, nullptr);
	vkCmdPushConstants (cmd, marchLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
	vkCmdDispatch (cmd, (outExtent.width + 7) / 8, (outExtent.height + 7) / 8, 1);
	vkc::ImageBarrier (cmd, output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	shadowMap[3] = 0.0f; // (no coverage map: the planet's 2D cloud shadows)
	ggPrevCam = f.camPlanet, ggPrevRot = f.viewToPlanet;
	ggHistValid = true;
	ggParity ^= 1;
	active = true;
}

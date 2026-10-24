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
#include "cloudcomp.frag.h"
#include "post.vert.h"

namespace {
	constexpr uint32_t SHAPE_SIZE = 128, DETAIL_SIZE = 32, MAP_SIZE = 2048;
	const VkFormat OUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

	struct MarchParams {
		float shell[4], mapCentre[4], mapEast[4], mapNorth[4], ambient[4], proj[4], noise[4];
	};
}

vkc::Image VolumetricClouds::CreateImage (VkImageType type, VkFormat fmt, VkExtent3D e, VkImageUsageFlags usage)
{
	vkc::Image im;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = type;
	ici.format = fmt;
	ici.extent = e;
	ici.mipLevels = 1;
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

	// noise volumes, generated once
	shapeNoise = CreateImage (VK_IMAGE_TYPE_3D, VK_FORMAT_R8_UNORM, { SHAPE_SIZE, SHAPE_SIZE, SHAPE_SIZE }, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	detailNoise = CreateImage (VK_IMAGE_TYPE_3D, VK_FORMAT_R8_UNORM, { DETAIL_SIZE, DETAIL_SIZE, DETAIL_SIZE }, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
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
				vkc::ImageBarrier (cmd, imgs[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
					VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
				int p[2] = { (int)(i ? DETAIL_SIZE : SHAPE_SIZE), i };
				vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s[i], 0, nullptr);
				vkCmdPushConstants (cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), p);
				uint32_t g = (uint32_t)(p[0] + 3) / 4;
				vkCmdDispatch (cmd, g, g, g);
				vkc::ImageBarrier (cmd, imgs[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
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
	VkDescriptorSetLayoutBinding mb[5];
	for (uint32_t i = 0; i < 4; i++) mb[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	mb[4] = { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 5, lci.pBindings = mb;
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

	// composite
	VkDescriptorSetLayoutBinding cbnd{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	lci.bindingCount = 1, lci.pBindings = &cbnd;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &compSetLayout));
	plci.setLayoutCount = 1, plci.pSetLayouts = &compSetLayout;
	plci.pushConstantRangeCount = 0;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &compLayout));
	compPipe = graphicsPipe (spv_post_vert, spv_post_vert_size, spv_cloudcomp_frag, spv_cloudcomp_frag_size,
		compLayout, ctx->sceneFormat, false, true);

	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2, pci.poolSizeCount = 2, pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	return true;
}

void VolumetricClouds::Shutdown ()
{
	if (!ctx) return;
	vkDeviceWaitIdle (ctx->device);
	VkPipeline pipes[] = { marchPipe, compPipe, coveragePipe };
	for (auto p : pipes) vkDestroyPipeline (ctx->device, p, nullptr);
	VkPipelineLayout pls[] = { marchLayout, compLayout, coverageLayout };
	for (auto p : pls) vkDestroyPipelineLayout (ctx->device, p, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, marchSetLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, compSetLayout, nullptr);
	vkDestroySampler (ctx->device, repeatSampler, nullptr);
	vkDestroySampler (ctx->device, clampSampler, nullptr);
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
	vkResetDescriptorPool (ctx->device, pool, 0);
	VkExtent2D r = ctx->RenderExtent ();
	outExtent = { (r.width + 1) / 2, (r.height + 1) / 2 };
	output = CreateImage (VK_IMAGE_TYPE_2D, OUT_FORMAT, { outExtent.width, outExtent.height, 1 }, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	VkImage img = output.image;
	ctx->ImmediateSubmit ([img](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	VkDescriptorSetLayout l[2] = { marchSetLayout, compSetLayout };
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool, dai.descriptorSetCount = 2, dai.pSetLayouts = l;
	VkDescriptorSet s[2];
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, s));
	marchSet = s[0], compSet = s[1];
	VkDescriptorImageInfo ii[6] = {
		{ clampSampler, coverage.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ repeatSampler, shapeNoise.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ repeatSampler, detailNoise.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
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

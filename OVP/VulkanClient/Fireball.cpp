// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Fireball.h"
#include "GfxSettings.h"
#include "OrbiterAPI.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "fireball_grid.comp.h"
#include "fireball_sun.comp.h"
#include "fireball.vert.h"
#include "fireball.frag.h"
#include "fireball_comp.frag.h"
#include "post.vert.h"

struct FireballRenderer::Params { // fireball.glsl
	float toBall[16]; // view -> ball space [m]
	float centre[4];  // view space, r
	float boxMin[4];  // ball space [m], slab
	float boxMax[4];  // step [m]
	float shape[4];   // r, R, elongation, lift
	float axis[4];    // ball space, ground (ball z)
	float time[4];    // age, u, t_f, seed
	float heat[4];    // hottest [K], burn-out base, flash, density
	float flow[4];    // ring's turn [rad], ring radius / r, churn, extinction scale
	float tune[4];    // (ORBITER_VK_FIREBALL_TUNE)
	float mode[4];    // the free cloud (1), its scale [m], its peak extinction [1/m], its flash (0..1)
	float asym[4];    // its lean (ball space xy, per unit height), -, -
	float adv[4];     // the noise carried outward, two phases cross-faded (phase 4c): A's scale and weight, B's scale and weight
	float adv2[4];    // the ring's turn since each phase began [rad] (A, B); each phase's number (its seed) (A, B)
};

namespace {
	// a smoothstep of x over 0..1 (clamped)
	double Smooth01 (double x) { x = std::clamp (x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }
	constexpr VkFormat HALF_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
	constexpr VkFormat OCC_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

	// debugging factors of the model and the march (ORBITER_VK_FIREBALL_TUNE=
	// raggedness,warp,erosion,emission[,extinction[,churn[,flow]]]: 1 = as tuned)
	void Tune (float t[4], float &sigma, float &churn, float &flow)
	{
		static float v[7] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		static bool read = false;
		if (!read) {
			read = true;
			if (const char *e = getenv ("ORBITER_VK_FIREBALL_TUNE"))
				sscanf (e, "%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
		}
		for (int i = 0; i < 4; i++) t[i] = v[i];
		sigma = v[4], churn = v[5], flow = v[6];
	}
}

void FireballRenderer::ModelTemps (double u, double &Th, double &Ts)
{
	// its temperature: the hottest gas (its core) cooling as it burns
	// (2350 K as it ignites, 2150 at full size, 1750 by 0.9 t_f, 1500
	// burned out), its surface's (the mixing layer, what is mostly seen:
	// 1890 K to 1700 K, a bright saturated orange under the march's
	// knee - at 1690 K it was a dark orange and the ball's area was all
	// its yellow-white cores - FbPlanck)
	static const double ku[] = { 0.0, 0.1, 0.25, 0.6, 0.9, 1.2, 1.8 };
	static const double kT[] = { 2350.0, 2250.0, 2150.0, 2000.0, 1750.0, 1500.0, 1200.0 }; // (its core)
	static const double kS[] = { 1890.0, 1860.0, 1830.0, 1780.0, 1700.0, 1560.0, 1230.0 }; // (its surface: the mixing layer)
	Th = kT[6], Ts = kS[6];
	if (u < 0.0) u = 0.0;
	for (int k = 0; k < 6; k++)
		if (u < ku[k + 1]) {
			double f = (u - ku[k]) / (ku[k + 1] - ku[k]);
			Th = kT[k] + (kT[k + 1] - kT[k]) * f, Ts = kS[k] + (kS[k + 1] - kS[k]) * f;
			break;
		}
}

double FireballRenderer::RingTurn (double u, double tf, double R, double g)
{
	// (the integral of Lift: u - a sqrt(pi)/2 erf(u/a))
	u = std::max (u, 0.0);
	double I = u - LIFT_U * 0.886226925 * std::erf (u / LIFT_U);
	return 1.4 * sqrt (g / std::max (R, 1.0)) * tf * I;
}

bool FireballRenderer::Init (vkc::Context *c, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout atmLayout,
	VkImageView shapeNoise, VkImageView detailNoise)
{
	ctx = c;
	shapeView = shapeNoise, detailView = detailNoise;
	ApplySettings ();
	buf = ctx->CreateBuffer (sizeof(Params) * MAX_MARCHED * vkc::FRAMES_IN_FLIGHT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	memset (buf.mapped, 0, sizeof(Params) * MAX_MARCHED * vkc::FRAMES_IN_FLIGHT);

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &linearClamp));
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &repeatSampler));
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &nearestClamp));

	const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CI = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	const VkShaderStageFlags CS = VK_SHADER_STAGE_COMPUTE_BIT, FS = VK_SHADER_STAGE_FRAGMENT_BIT;
	auto makeLayout = [this](const std::vector<VkDescriptorSetLayoutBinding> &b) {
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = (uint32_t)b.size (), lci.pBindings = b.data ();
		VkDescriptorSetLayout l;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &l));
		return l;
	};
	// compute: the grids (written A, B, C; A read), the noise, the buffer
	compSetLayout = makeLayout ({ { 0, SI, 1, CS, nullptr }, { 1, SI, 1, CS, nullptr }, { 2, SI, 1, CS, nullptr },
		{ 3, CI, 1, CS, nullptr }, { 4, CI, 1, CS, nullptr }, { 5, CI, 1, CS, nullptr }, { 6, SB, 1, CS, nullptr } });
	// march: the buffer, the grids, the noise, the prepass
	marchSetLayout = makeLayout ({ { 0, SB, 1, FS, nullptr }, { 1, CI, 1, FS, nullptr }, { 2, CI, 1, FS, nullptr },
		{ 3, CI, 1, FS, nullptr }, { 4, CI, 1, FS, nullptr }, { 5, CI, 1, FS, nullptr }, { 6, CI, 1, FS, nullptr } });
	// composite: the half-size images, the prepass, the smoke in front
	compoSetLayout = makeLayout ({ { 0, CI, 1, FS, nullptr }, { 1, CI, 1, FS, nullptr }, { 2, CI, 1, FS, nullptr },
		{ 3, CI, 1, FS, nullptr }, { 4, CI, 1, FS, nullptr } });
	VkDescriptorPoolSize ps[3] = { { SI, 3 }, { CI, 4 + 6 + 5 * vkc::FRAMES_IN_FLIGHT }, { SB, 2 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2 + vkc::FRAMES_IN_FLIGHT, pci.poolSizeCount = 3, pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	VkDescriptorSetLayout ls[2 + vkc::FRAMES_IN_FLIGHT] = { compSetLayout, marchSetLayout };
	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++) ls[2 + i] = compoSetLayout;
	VkDescriptorSet sets[2 + vkc::FRAMES_IN_FLIGHT];
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool, dai.descriptorSetCount = 2 + vkc::FRAMES_IN_FLIGHT, dai.pSetLayouts = ls;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, sets));
	compSet = sets[0], marchSet = sets[1];
	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++) compoSet[i] = sets[2 + i];

	// compute pipelines
	{
		VkDescriptorSetLayout cl[2] = { frameLayout, compSetLayout };
		VkPushConstantRange pcr{ CS, 0, 2 * sizeof(uint32_t) };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 2, plci.pSetLayouts = cl;
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &compLayout));
		auto make = [this](const unsigned char *code, size_t size) {
			VkShaderModule m = ctx->CreateShaderModule (code, size);
			VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main" };
			cpi.layout = compLayout;
			VkPipeline p;
			VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &p));
			vkDestroyShaderModule (ctx->device, m, nullptr);
			return p;
		};
		gridPipe = make (spv_fireball_grid_comp, spv_fireball_grid_comp_size);
		sunPipe = make (spv_fireball_sun_comp, spv_fireball_sun_comp_size);
	}

	// the march: the box (view space), into the half-size images
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	cba.blendEnable = VK_TRUE; // premultiplied: light plus what is behind times the transmittance (alpha: their product)
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	cba.colorBlendOp = VK_BLEND_OP_ADD;
	cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	cba.alphaBlendOp = VK_BLEND_OP_ADD;
	VkPipelineColorBlendAttachmentState cbm{}; // (reactive mask: the strongest)
	cbm.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
	cbm.blendEnable = VK_TRUE;
	cbm.srcColorBlendFactor = cbm.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cbm.colorBlendOp = VK_BLEND_OP_MAX;
	cbm.srcAlphaBlendFactor = cbm.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cbm.alphaBlendOp = VK_BLEND_OP_MAX;
	VkPipelineColorBlendAttachmentState cbo{}; // (occlusion: the nearest's inverse depths, the transmittances' product)
	cbo.colorWriteMask = 0xF;
	cbo.blendEnable = VK_TRUE;
	cbo.srcColorBlendFactor = cbo.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cbo.colorBlendOp = VK_BLEND_OP_MAX;
	cbo.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	cbo.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	cbo.alphaBlendOp = VK_BLEND_OP_ADD;
	{
		VkDescriptorSetLayout ml[3] = { frameLayout, marchSetLayout, atmLayout };
		VkPushConstantRange pcr{ FS, 0, 3 * sizeof(uint32_t) };
		VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 3, plci.pSetLayouts = ml;
		plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &marchLayout));
		VkShaderModule vs = ctx->CreateShaderModule (spv_fireball_vert, spv_fireball_vert_size);
		VkShaderModule fs = ctx->CreateShaderModule (spv_fireball_frag, spv_fireball_frag_size);
		VkPipelineShaderStageCreateInfo stages[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" } };
		VkVertexInputBindingDescription bind{ 0, 3 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
		VkVertexInputAttributeDescription attr{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &bind;
		vi.vertexAttributeDescriptionCount = 1, vi.pVertexAttributeDescriptions = &attr;
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vps.viewportCount = 1, vps.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE; // (the shader keeps the far side of the box)
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; // (none: the march stops at the prepass depth)
		VkPipelineColorBlendAttachmentState cbs[3] = { cba, cbm, cbo };
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 3, cb.pAttachments = cbs;
		VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
		VkFormat formats[3] = { HALF_FORMAT, VK_FORMAT_R8_UNORM, OCC_FORMAT };
		VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		rci.colorAttachmentCount = 3, rci.pColorAttachmentFormats = formats;
		rci.depthAttachmentFormat = ctx->depthFormat;
		VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gpi.pNext = &rci;
		gpi.stageCount = 2, gpi.pStages = stages;
		gpi.pVertexInputState = &vi;
		gpi.pInputAssemblyState = &ia;
		gpi.pViewportState = &vps;
		gpi.pRasterizationState = &rs;
		gpi.pMultisampleState = &ms;
		gpi.pDepthStencilState = &ds;
		gpi.pColorBlendState = &cb;
		gpi.pDynamicState = &dy;
		gpi.layout = marchLayout;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &marchPipe));
		vkDestroyShaderModule (ctx->device, vs, nullptr);
		vkDestroyShaderModule (ctx->device, fs, nullptr);

		// the composite: over the scene (with the reactive mask): the scene
		// times the transmittance, plus the light
		VkDescriptorSetLayout cl[2] = { frameLayout, compoSetLayout };
		VkPushConstantRange cpcr{ FS, 0, sizeof(uint32_t) };
		VkPipelineLayoutCreateInfo cplci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		cplci.setLayoutCount = 2, cplci.pSetLayouts = cl;
		cplci.pushConstantRangeCount = 1, cplci.pPushConstantRanges = &cpcr;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &cplci, nullptr, &compoLayout));
		VkShaderModule cvs = ctx->CreateShaderModule (spv_post_vert, spv_post_vert_size);
		VkShaderModule cfs = ctx->CreateShaderModule (spv_fireball_comp_frag, spv_fireball_comp_frag_size);
		VkPipelineShaderStageCreateInfo cst[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, cvs, "main" },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, cfs, "main" } };
		VkPipelineVertexInputStateCreateInfo cvi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineColorBlendAttachmentState ccba = cba;
		ccba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		ccba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		ccba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		VkPipelineColorBlendAttachmentState ccbs[2] = { ccba, cbm };
		VkPipelineColorBlendStateCreateInfo ccb = cb;
		ccb.attachmentCount = 2, ccb.pAttachments = ccbs;
		VkPipelineMultisampleStateCreateInfo cms = ms;
		cms.rasterizationSamples = ctx->sceneSamples;
		VkFormat cformats[2] = { ctx->sceneFormat, VK_FORMAT_R8_UNORM }; // (Scene::REACTIVE_FORMAT)
		VkPipelineRenderingCreateInfo crci = rci;
		crci.colorAttachmentCount = 2, crci.pColorAttachmentFormats = cformats;
		VkGraphicsPipelineCreateInfo cgpi = gpi;
		cgpi.pNext = &crci;
		cgpi.pStages = cst;
		cgpi.pVertexInputState = &cvi;
		cgpi.pColorBlendState = &ccb;
		cgpi.pMultisampleState = &cms;
		cgpi.layout = compoLayout;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &cgpi, nullptr, &compoPipe));
		vkDestroyShaderModule (ctx->device, cvs, nullptr);
		vkDestroyShaderModule (ctx->device, cfs, nullptr);
	}

	// (no smoke volume: nothing in front)
	{
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_3D, ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		ici.extent = { 1, 1, 1 }, ici.mipLevels = 1, ici.arrayLayers = 1, ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL, ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &dummyFog.image, &dummyFog.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = dummyFog.image, vci.viewType = VK_IMAGE_VIEW_TYPE_3D, vci.format = ici.format;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &dummyFog.view));
		// (and no fireball marched: the sprites' occlusion image while the
		// half-size images are not allocated - nothing begins, transmittance 1)
		ici.imageType = VK_IMAGE_TYPE_2D;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &dummyOcc.image, &dummyOcc.alloc, nullptr));
		vci.image = dummyOcc.image, vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &dummyOcc.view));
		VkImage im = dummyFog.image, im2 = dummyOcc.image;
		ctx->ImmediateSubmit ([im, im2](VkCommandBuffer cmd) {
			VkClearColorValue cv{};
			cv.float32[3] = 1.0f;
			VkImageSubresourceRange rr{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			vkCmdClearColorImage (cmd, im, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rr);
			vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			vkc::ImageBarrier (cmd, im2, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			vkCmdClearColorImage (cmd, im2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rr);
			vkc::ImageBarrier (cmd, im2, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		});
	}
	// (the half-size images: made when the first fireball is marched,
	// released with the grids two minutes after the last - AllocHalf)
	return true;
}

void FireballRenderer::ApplySettings ()
{
	// (the graphics settings' Fireballs, live; the environment's
	// ORBITER_VK_FIREBALL overrides it there; ORBITER_VK_FIREBALL_FULLRES=1
	// as before phase 6: the full resolution)
	const vkc::GfxSettings &g = vkc::Gfx ();
	enabled = g.fireballs != vkc::GfxSettings::FIREBALL_SPRITES;
	static const bool envFull = getenv ("ORBITER_VK_FIREBALL_FULLRES") && atoi (getenv ("ORBITER_VK_FIREBALL_FULLRES"));
	fullRes = envFull || g.fireballs == vkc::GfxSettings::FIREBALL_FULL;
}

void FireballRenderer::Shutdown ()
{
	if (!ctx) return;
	vkDeviceWaitIdle (ctx->device);
	FreeGrids ();
	for (VkPipeline p : { gridPipe, sunPipe, marchPipe, compoPipe }) if (p) vkDestroyPipeline (ctx->device, p, nullptr);
	for (VkPipelineLayout l : { compLayout, marchLayout, compoLayout }) if (l) vkDestroyPipelineLayout (ctx->device, l, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	for (VkDescriptorSetLayout l : { compSetLayout, marchSetLayout, compoSetLayout }) if (l) vkDestroyDescriptorSetLayout (ctx->device, l, nullptr);
	for (VkSampler s : { linearClamp, repeatSampler, nearestClamp }) if (s) vkDestroySampler (ctx->device, s, nullptr);
	for (vkc::Image *im : { &halfColor, &halfMask, &halfOcc, &halfDepth, &dummyFog, &dummyOcc }) ctx->DestroyImage (*im);
	ctx->DestroyBuffer (buf);
	ctx = nullptr;
}

void FireballRenderer::AllocHalf ()
{
	// (within the frame: no wait - the images are new; FreeHalf defers)
	VkExtent2D e = ctx->RenderExtent ();
	halfFull = fullRes;
	halfExtent = halfFull ? e : VkExtent2D{ (e.width + 1) / 2, (e.height + 1) / 2 };
	auto create = [this](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = fmt;
		ici.extent = { halfExtent.width, halfExtent.height, 1 };
		ici.mipLevels = 1, ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image, vci.viewType = VK_IMAGE_VIEW_TYPE_2D, vci.format = fmt;
		vci.subresourceRange = { aspect, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		return im;
	};
	const VkImageUsageFlags CU = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	halfColor = create (HALF_FORMAT, CU, VK_IMAGE_ASPECT_COLOR_BIT);
	halfMask = create (VK_FORMAT_R8_UNORM, CU, VK_IMAGE_ASPECT_COLOR_BIT);
	halfOcc = create (OCC_FORMAT, CU, VK_IMAGE_ASPECT_COLOR_BIT);
	halfDepth = create (ctx->depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
	// (shader-read between uses: the sprites may sample the occlusion before the first march)
	VkCommandBuffer cmd = ctx->Cmd ();
	for (VkImage im : { halfColor.image, halfMask.image, halfOcc.image })
		vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	halfAlloc = true;
	halfGen++;
	halfSwapGen = ctx->SwapchainGeneration ();
	for (auto &g : compoGen) g = ~0u;
	if (onOcclusion) onOcclusion (OcclusionView ());
}

void FireballRenderer::FreeHalf ()
{
	if (!halfAlloc) return;
	vkc::Image a = halfColor, b = halfMask, c = halfOcc, d = halfDepth;
	vkc::Context *cx = ctx;
	ctx->DeferDestroy ([cx, a, b, c, d]() mutable { cx->DestroyImage (a); cx->DestroyImage (b); cx->DestroyImage (c); cx->DestroyImage (d); });
	halfColor = halfMask = halfOcc = halfDepth = vkc::Image{};
	halfAlloc = false;
	drawn = false;
	if (onOcclusion) onOcclusion (OcclusionView ());
}

void FireballRenderer::AllocGrids ()
{
	auto create = [this](VkFormat fmt, uint32_t div) {
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_3D;
		ici.format = fmt;
		ici.extent = { GX / div, GY / div, GZ / div * MAX_MARCHED };
		ici.mipLevels = 1, ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image, vci.viewType = VK_IMAGE_VIEW_TYPE_3D, vci.format = fmt;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		return im;
	};
	gridA = create (VK_FORMAT_R16G16B16A16_SFLOAT, 1);
	gridB = create (VK_FORMAT_R16G16B16A16_SFLOAT, 2); // (the shadows and the fire's light looking in: soft, half the resolution)
	gridC = create (VK_FORMAT_R16G16B16A16_SFLOAT, 1);
	// (general layout throughout: written by compute, read by the march)
	VkImage ims[3] = { gridA.image, gridB.image, gridC.image };
	VkCommandBuffer cmd = ctx->Cmd ();
	for (VkImage im : ims)
		vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
	gridAlloc = true;
	// the sets of the compute passes and the march
	const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkDescriptorImageInfo ci[6] = { { VK_NULL_HANDLE, gridA.view, G }, { VK_NULL_HANDLE, gridB.view, G }, { VK_NULL_HANDLE, gridC.view, G },
		{ linearClamp, gridA.view, G }, { repeatSampler, shapeView, R }, { repeatSampler, detailView, R } };
	VkDescriptorBufferInfo bi{ buf.buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet w[7]{};
	for (uint32_t i = 0; i < 7; i++) {
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = compSet, w[i].dstBinding = i, w[i].descriptorCount = 1;
		if (i < 3) w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[i].pImageInfo = &ci[i];
		else if (i < 6) w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[i].pImageInfo = &ci[i];
		else w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[i].pBufferInfo = &bi;
	}
	vkUpdateDescriptorSets (ctx->device, 7, w, 0, nullptr);
	WriteMarchSet ();
}

void FireballRenderer::WriteMarchSet ()
{
	if (!gridAlloc || !prepassView) return;
	const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkDescriptorImageInfo mi[6] = { { linearClamp, gridA.view, G }, { linearClamp, gridB.view, G }, { linearClamp, gridC.view, G },
		{ repeatSampler, shapeView, R }, { repeatSampler, detailView, R }, { nearestClamp, prepassView, R } };
	VkDescriptorBufferInfo bi{ buf.buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet w[7]{};
	for (uint32_t i = 0; i < 7; i++) {
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = marchSet, w[i].dstBinding = i, w[i].descriptorCount = 1;
		if (i == 0) w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[i].pBufferInfo = &bi;
		else w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[i].pImageInfo = &mi[i - 1];
	}
	vkUpdateDescriptorSets (ctx->device, 7, w, 0, nullptr);
}

void FireballRenderer::FreeGrids ()
{
	if (!gridAlloc) return;
	vkc::Image a = gridA, b = gridB, c = gridC;
	vkc::Context *cx = ctx;
	ctx->DeferDestroy ([cx, a, b, c]() mutable { cx->DestroyImage (a); cx->DestroyImage (b); cx->DestroyImage (c); });
	gridA = gridB = gridC = vkc::Image{};
	gridAlloc = false;
}

void FireballRenderer::SetPrepass (VkImageView prepass)
{
	ApplySettings ();
	// the half-size images: with the swapchain and the resolution chosen;
	// made when a fireball first asks to be marched (WantsVolume, in the
	// visuals' update before this), released with the grids (Compute)
	if (halfAlloc && (halfSwapGen != ctx->SwapchainGeneration () || halfFull != fullRes)) FreeHalf ();
	if (wanted && !halfAlloc && enabled) AllocHalf ();
	if (prepass != prepassView) {
		// (the march set is bound by no frame in flight while it is written:
		// a new prepass comes with a new swapchain, after an idle wait)
		if (prepassView) vkDeviceWaitIdle (ctx->device);
		prepassView = prepass;
		WriteMarchSet ();
		for (auto &g : compoGen) g = ~0u;
	}
}

void FireballRenderer::BeginFrame (double pxr)
{
	list.clear ();
	pxPerRad = pxr;
	drawn = computed = false;
	share = 0.0;
}

bool FireballRenderer::WantsVolume (uint32_t owner, double R, double dist) const
{
	if (!enabled) return false;
	// (at a time warp beyond 100x the sprites: its whole life is a frame or
	// two - PLAN.md 2.6; the model itself is right at any warp)
	if (oapiGetTimeAcceleration () > 100.0) return false;
	auto has = [owner](const std::vector<uint32_t> &v) { return std::find (v.begin (), v.end (), owner) != v.end (); };
	bool last = has (marchedLast);
	// (large enough on screen: one marched stays so down to KEEP_PIXELS,
	// one not needs MIN_PIXELS - no flicker at the threshold; the switch
	// itself is cross-faded by its owner: Ball::fade)
	double px = R / std::max (dist, 1.0) * pxPerRad;
	if (px < (last ? KEEP_PIXELS : MIN_PIXELS)) return false;
	// (marched in the last frame; or room among those and the ones told so since)
	if (last || has (granted)) { wanted = true; return true; }
	size_t n = granted.size ();
	for (uint32_t o : marchedLast) n += std::find (granted.begin (), granted.end (), o) == granted.end () ? 1 : 0;
	if (n >= MAX_MARCHED) return false;
	granted.push_back (owner);
	wanted = true;
	return true;
}

void FireballRenderer::Add (const Ball &b)
{
	if (!enabled || b.r <= 0.0) return;
	double dist = length (b.centre);
	Entry en{};
	en.b = b, en.dist = dist;
	en.px = b.R / std::max (dist, 1.0) * pxPerRad;
	if (en.px < DRAW_PIXELS) return; // (one fading out to sprites is drawn below KEEP_PIXELS)
	wanted = true;
	list.push_back (en);
}

void FireballRenderer::Compute (VkDescriptorSet frameSet)
{
	// the largest on screen (at most MAX_MARCHED: the others' sprites)
	std::sort (list.begin (), list.end (), [](const Entry &a, const Entry &b) { return a.px > b.px; });
	if (list.size () > MAX_MARCHED) list.resize (MAX_MARCHED);
	marchedLast.clear ();
	for (const Entry &en : list) marchedLast.push_back (en.b.owner);
	granted.clear ();
	double now = oapiGetSysTime ();
	if (list.empty ()) {
		// (the grids and the half-size images released two minutes after
		// the last fireball: about 70 MB and 20 MB at 1440p)
		wanted = false;
		if (gridAlloc && now - gridIdleSince > 120.0) FreeGrids (), FreeHalf ();
		return;
	}
	gridIdleSince = now;
	if (!gridAlloc) AllocGrids ();
	if (!prepassView || !halfAlloc) return;

	uint32_t slot = ctx->FrameIndex ();
	float tune[4], tuneSigma, tuneChurn, tuneFlow;
	Tune (tune, tuneSigma, tuneChurn, tuneFlow);
	Params *pm = (Params*)buf.mapped + slot * MAX_MARCHED;
	for (size_t i = 0; i < list.size (); i++) {
		Entry &en = list[i];
		const Ball &b = en.b;
		// ball space: x along its path (horizontal), y across, z up
		VECTOR3 ez = unit (b.up);
		VECTOR3 ex = b.axis - ez * dotp (b.axis, ez);
		if (length (ex) < 1e-3) ex = crossp (ez, fabs (ez.x) < 0.9 ? _V(1, 0, 0) : _V(0, 1, 0));
		ex = unit (ex);
		VECTOR3 ey = crossp (ez, ex);
		VECTOR3 ab = _V(dotp (b.axis, ex), dotp (b.axis, ey), dotp (b.axis, ez));
		if (length (ab) > 1e-6) ab = unit (ab); else ab = _V(1, 0, 0);
		// its box: the ellipsoid drawn out along ab (semi-axes r e along it,
		// r across, widened across as it lifts), its billows out to 1.9 of
		// 0.9 r (fireball_grid.comp: the shape's reach with its displacement
		// and its lobes), and the lean of its top: the box is wider on the
		// side it leans to (phase 4c: a box of 1.85 r cut the lobed, leaning
		// ball flat - the grid fades the shape out at its faces besides)
		double r = b.r, e = std::max (b.elong, 1.0), L = std::clamp (b.lift, 0.0, 1.0);
		double lean[2] = { dotp (b.lean, ex), dotp (b.lean, ey) };
		double half[3], bmin[3], bmax[3];
		for (int k = 0; k < 3; k++) {
			double c = (&ab.x)[k];
			half[k] = 0.9 * r * 1.9 * sqrt (e * e * c * c + (1.0 - c * c)) / (k < 2 ? 1.0 - 0.12 * L : 1.0);
			if (k == 2) half[k] += 0.9 * r * 0.1; // (the lobes lifting unevenly)
			bmin[k] = -half[k], bmax[k] = half[k];
			// (its top at about 2.0 of 0.9 r, carried on by lean x (z + 0.6))
			if (k < 2) {
				double s = 0.9 * r * 2.6 * lean[k];
				if (s > 0.0) bmax[k] += s; else bmin[k] += s;
			}
		}
		// (the free cloud: a cube holding its jets, 2.6 of its scale)
		if (b.vacuum)
			for (int k = 0; k < 3; k++) bmax[k] = 2.6 * b.scale, bmin[k] = -bmax[k];
		// (nothing below the ground)
		if (b.ground > -1e8 && !b.vacuum) bmin[2] = std::min (std::max (bmin[2], b.ground - 0.05 * r), bmax[2] - 0.2 * r);
		if (b.ground > -1e8 && b.vacuum) bmin[2] = std::min (std::max (bmin[2], b.ground - 0.1 * b.scale), bmax[2] - 0.5 * b.scale); // (the free cloud on the ground: its sheet just above it)
		Params &p = pm[i];
		memset (&p, 0, sizeof(p));
		const VECTOR3 *ax[3] = { &ex, &ey, &ez };
		for (int k = 0; k < 3; k++) {
			p.toBall[0 + k] = (float)ax[k]->x;
			p.toBall[4 + k] = (float)ax[k]->y;
			p.toBall[8 + k] = (float)ax[k]->z;
			p.toBall[12 + k] = (float)-dotp (*ax[k], b.centre);
			p.boxMin[k] = (float)bmin[k], p.boxMax[k] = (float)bmax[k];
		}
		p.toBall[15] = 1.0f;
		p.centre[0] = (float)b.centre.x, p.centre[1] = (float)b.centre.y, p.centre[2] = (float)b.centre.z;
		p.boxMin[3] = (float)i;
		// the march's step: by its size on screen (finer when large; PLAN.md
		// 2.6: the levels by the screen radius), and its finest noise octave
		// (the flame's texture close up) only where it can be seen
		double cell = std::min ({ (bmax[0] - bmin[0]) / GX, (bmax[1] - bmin[1]) / GY, (bmax[2] - bmin[2]) / GZ });
		double px = r / std::max (en.dist, 1.0) * pxPerRad;
		double step = px > 700.0 ? 0.5 : px > 400.0 ? 0.6 : px > 150.0 ? 0.75 : px > 60.0 ? 1.0 : 1.4;
		// (the soot ball fading into the smoke volume's cap, 1.3-1.8 t_f: soft
		// and thinning, its box the widest - half as coarse again by the end:
		// the close view's widest frames were its costliest)
		double uu = b.t / std::max (b.tf, 1e-3);
		step *= 1.0 + 0.5 * std::clamp ((uu - 1.3) / 0.5, 0.0, 1.0);
		// (the free cloud: smooth streamers, no billows to resolve - two cells
		// a step; at the fireball's half a cell, a cloud filling the view at
		// 1440p took 3.7 ms to march)
		if (b.vacuum) step = 2.0;
		p.boxMax[3] = (float)(cell * step);
		p.asym[2] = (float)std::clamp ((px - 50.0) / 100.0, 0.0, 1.0);
		double tf = std::max (b.tf, 1e-3), u = b.t / tf;
		p.shape[0] = (float)r, p.shape[1] = (float)b.R, p.shape[2] = (float)e, p.shape[3] = (float)b.lift;
		p.axis[0] = (float)ab.x, p.axis[1] = (float)ab.y, p.axis[2] = (float)ab.z, p.axis[3] = (float)b.ground;
		p.time[0] = (float)b.t, p.time[1] = (float)u, p.time[2] = (float)tf, p.time[3] = (float)(b.seed % 997);
		// its temperature (ModelTemps): the hottest gas (its core) and its
		// surface (the mixing layer); the share burned out growing (the
		// outer layer from 0.3 t_f; most of it by t_f, all by 1.3 t_f); the
		// ignition flash: white-hot for its first two frames, handed over
		// to the orange ball by 0.1 s (not the lemon yellow of a hot
		// blackbody under the march's knee: fireball.frag mixes its light
		// to white by this); its density fading from 1.3 t_f as its soot
		// passes to the smoke volume (gone by 1.8 t_f)
		double Th, Ts;
		ModelTemps (u, Th, Ts);
		p.heat[0] = (float)Th;
		p.centre[3] = (float)Ts;
		p.heat[1] = (float)(-0.55 + 1.15 * u);
		p.heat[2] = (float)std::clamp ((0.1 - b.t) / 0.05, 0.0, 1.0);
		p.heat[3] = (float)(Smooth01 ((1.8 - u) / 0.5) * std::clamp (b.fade, 0.0, 1.0)); // (the soot ball, rising, fading into the smoke volume's cap; its cross-fade with its sprites)
		p.flow[0] = (float)b.roll;
		p.flow[1] = 0.55f;
		p.flow[2] = (float)(0.2 * b.t * tuneChurn); // (boiling: the warp field drifting - 0.08 left the billows frozen in place)
		// its lean (phase 4b)
		p.asym[0] = (float)lean[0], p.asym[1] = (float)lean[1];
		// its billows carried outward with the hot gas (phase 4c): the noise
		// sampled at the coordinates scaled down about the centre, evenly, so
		// that features emerge from inside and push out keeping their shape
		// (phase 4b drew the coordinates in by a fixed length: the noise was
		// stretched along the limb into brushed streaks). Each scaling runs
		// for a period of 0.7 t_f, then a fresh phase takes over: two phases
		// half a period apart, cross-faded (a flow map), each with its own
		// seed and the ring's turn since it began - the stretch of neither
		// ever accumulates
		{
			const double period = 0.7 * tf, rate = 0.5 * tuneFlow; // (the surface's features move out by about 0.45 r per t_f)
			for (int k = 0; k < 2; k++) {
				double ts = floor ((b.t + 0.5 * k * period) / period) * period - 0.5 * k * period;
				double tau = b.t - ts;
				double wgt = 1.0 - fabs (2.0 * tau / period - 1.0);
				p.adv[2 * k] = (float)(1.0 / (1.0 + rate * tau / tf));
				p.adv[2 * k + 1] = (float)wgt;
				p.adv2[k] = (float)(b.roll - RingTurn (std::max (ts, 0.0) / tf, tf, b.R, b.g));
				p.adv2[2 + k] = (float)floor (2.0 * ts / period + 0.5); // (its number: k 0 even, k 1 odd)
			}
		}
		p.flow[3] = tuneSigma;
		for (int k = 0; k < 4; k++) p.tune[k] = tune[k];
		if (b.vacuum) {
			// the free cloud (VACUUM_PLAN.md 3.3): white-hot as it flashes
			// (its first two frames), its core cooling as it expands
			// (adiabatically: 2800 K to a dull red by 0.1 s, gone by 0.2), its
			// droplets and ice lit by the sun; thinning by itself (mode.z, 1 /
			// s^3), faded out over its last column depths 0.15 to 0.06 - not
			// cut when its marching ends
			double Tc = 2800.0 * pow (1.0 + b.t / 0.08, -1.2);
			p.heat[0] = (float)Tc;
			p.centre[3] = p.heat[0];
			p.heat[1] = 0.0f, p.heat[2] = 0.0f;
			double tauC = b.sigma * b.scale * 1.7724539; // (through its centre: sigma s sqrt(pi))
			p.heat[3] = (float)(Smooth01 ((tauC - 0.06) / 0.09) * std::clamp (b.fade, 0.0, 1.0));
			p.flow[0] = 0.0f;
			p.mode[0] = 1.0f, p.mode[1] = (float)b.scale, p.mode[2] = (float)b.sigma;
			p.mode[3] = (float)std::clamp (1.0 - b.t / 0.07, 0.0, 1.0);
		}
		// its box (view space), the proxy drawn
		static const int faces[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
		VECTOR3 corner[8];
		for (int c = 0; c < 8; c++) {
			double x = (c & 1) ? bmax[0] : bmin[0], y = (c & 2) ? bmax[1] : bmin[1], z = (c & 4) ? bmax[2] : bmin[2];
			corner[c] = b.centre + ex * x + ey * y + ez * z;
		}
		int nv = 0;
		for (auto &f : faces) {
			int idx[6] = { f[0], f[1], f[2], f[0], f[2], f[3] };
			for (int c : idx)
				en.box[nv++] = (float)corner[c].x, en.box[nv++] = (float)corner[c].y, en.box[nv++] = (float)corner[c].z;
		}
		// (its share of the view: exposure metering)
		VkExtent2D re = ctx->RenderExtent ();
		share += PI * px * px / std::max (1.0, (double)re.width * re.height);
	}
	share = std::min (share, 1.0);

	VkCommandBuffer cmd = ctx->Cmd ();
	auto barrier = [cmd](VkPipelineStageFlags2 s0, VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = s0, mb.srcAccessMask = a0, mb.dstStageMask = s1, mb.dstAccessMask = a1;
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2 (cmd, &dep);
	};
	const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	const VkAccessFlags2 RD = VK_ACCESS_2_SHADER_READ_BIT, WR = VK_ACCESS_2_SHADER_WRITE_BIT;
	// (the last frame's march read them)
	barrier (FS | CS, RD | WR, CS, RD | WR);
	uint32_t pc[2] = { slot * MAX_MARCHED, (uint32_t)list.size () };
	VkDescriptorSet s[2] = { frameSet, compSet };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compLayout, 0, 2, s, 0, nullptr);
	vkCmdPushConstants (cmd, compLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gridPipe);
	vkCmdDispatch (cmd, GX / 4, GY / 4, GZ / 4 * (uint32_t)list.size ());
	barrier (CS, WR, CS, RD);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sunPipe);
	vkCmdDispatch (cmd, GX / 8, GY / 8, GZ / 8 * (uint32_t)list.size ());
	barrier (CS, WR, FS, RD);
	ctx->GpuMark ("fb grid");
	computed = true;
}

void FireballRenderer::Render (VkDescriptorSet frameSet, VkDescriptorSet atmSet)
{
	drawn = false;
	if (!computed || list.empty () || !halfColor.image) return;
	vkc::Target t[3] = { { halfColor.image, halfColor.view, HALF_FORMAT, halfExtent },
		{ halfMask.image, halfMask.view, VK_FORMAT_R8_UNORM, halfExtent },
		{ halfOcc.image, halfOcc.view, OCC_FORMAT, halfExtent } };
	vkc::Target d{ halfDepth.image, halfDepth.view, ctx->depthFormat, halfExtent };
	// (light 0, transmittance 1; no occlusion: inverse depths 0, transmittance 1)
	const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	ctx->BindTargetsWithDepth (t, 3, d, clear);
	VkCommandBuffer cmd = ctx->Cmd ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, marchPipe);
	VkDescriptorSet s[3] = { frameSet, marchSet, atmSet };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, marchLayout, 0, 3, s, 0, nullptr);
	// (far to near)
	std::vector<size_t> order (list.size ());
	for (size_t i = 0; i < order.size (); i++) order[i] = i;
	std::sort (order.begin (), order.end (), [this](size_t a, size_t b) { return list[a].dist > list[b].dist; });
	// (the march's jitter: a new sequence each simulation step, the same
	// while paused - a frozen fireball, not one crawling with noise)
	static uint32_t frameNo = 0;
	static double lastT = -1e30;
	double simt = oapiGetSimTime ();
	if (simt != lastT) frameNo++, lastT = simt;
	for (size_t i : order) {
		vkc::Context::Transient vb = ctx->AllocTransient (sizeof(list[i].box));
		memcpy (vb.ptr, list[i].box, sizeof(list[i].box));
		vkCmdBindVertexBuffers (cmd, 0, 1, &vb.buffer, &vb.offset);
		uint32_t pc[3] = { ctx->FrameIndex () * MAX_MARCHED + (uint32_t)i, frameNo, halfExtent.width < ctx->RenderExtent ().width ? 2u : 1u };
		vkCmdPushConstants (cmd, marchLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
		vkCmdDraw (cmd, 36, 1, 0, 0);
	}
	ctx->EndTarget ();
	ctx->GpuMark ("fb march");
	drawn = true;
}

void FireballRenderer::Draw (VkDescriptorSet frameSet, VkImageView fog, VkSampler fogSampler)
{
	if (!drawn) return;
	uint32_t fi = ctx->FrameIndex ();
	VkImageView fv = fog ? fog : dummyFog.view;
	VkSampler fs = fog ? fogSampler : linearClamp;
	if (compoFog[fi] != fv || compoPrepass[fi] != prepassView || compoGen[fi] != halfGen) {
		// (this slot's set: not in use by a frame in flight)
		VkDescriptorImageInfo ii[5] = { { nearestClamp, halfColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ nearestClamp, halfMask.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ nearestClamp, halfOcc.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ nearestClamp, prepassView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ fs, fv, VK_IMAGE_LAYOUT_GENERAL } };
		VkWriteDescriptorSet w[5]{};
		for (uint32_t i = 0; i < 5; i++) {
			w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w[i].dstSet = compoSet[fi], w[i].dstBinding = i, w[i].descriptorCount = 1;
			w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[i].pImageInfo = &ii[i];
		}
		vkUpdateDescriptorSets (ctx->device, 5, w, 0, nullptr);
		compoFog[fi] = fv, compoPrepass[fi] = prepassView, compoGen[fi] = halfGen;
	}
	VkCommandBuffer cmd = ctx->Cmd ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compoPipe);
	VkDescriptorSet s[2] = { frameSet, compoSet[fi] };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compoLayout, 0, 2, s, 0, nullptr);
	uint32_t on = fog ? 1u : 0u;
	vkCmdPushConstants (cmd, compoLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(on), &on);
	vkCmdDraw (cmd, 3, 1, 0, 0);
	ctx->GpuMark ("fb comp");
}

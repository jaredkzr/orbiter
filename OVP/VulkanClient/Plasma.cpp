// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Plasma.h"
#include "VkMesh.h"
#include "OrbiterAPI.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "plasma.vert.h"
#include "plasma.frag.h"
#include "plasma_flow.vert.h"
#include "plasma_flow.frag.h"
#include "plasma_jfa.comp.h"
#include "plasma_vol.vert.h"
#include "plasma_vol.frag.h"

namespace {
	// the shell's radiance at the stagnation point at peak heating, seen
	// face-on (about the sunlit hull's; more seen at a slant)
	constexpr float INTENSITY = 2.6f;
	// the volume's: the radiance of the shock layer crossed face-on is
	// some 0.5 of it, of a sheet streaming off the outline some 1.6 of it
	// (seen edge-on, many times that: the wall of light along the flank)
	constexpr float VOL_INTENSITY = 1.0f;
	constexpr uint32_t TILE = 256;             // a flow map's texels across
	constexpr uint32_t ATLAS_N = 4;            // tiles across the atlas
	constexpr uint32_t ATLAS = TILE * ATLAS_N;
	constexpr uint32_t MAX_TILES = ATLAS_N * ATLAS_N;
	constexpr float WN = 2.2f;                 // the flow map's half width [vessel sizes]
	constexpr VkFormat FLOW_FORMAT = VK_FORMAT_R16G16_SFLOAT;
	constexpr VkFormat FIELD_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

	struct PushConstants {
		float modelView[16];
		float flow[4];   // direction of motion through the air (view), w: heating level 0..1
		float shape[4];  // thickness [m], vessel radius [m], time [s], intensity
		float centre[4]; // the vessel's centre (view), w: -
	};
	static_assert (sizeof (PushConstants) <= 128, "push constants");
	struct FlowPC {
		float toFlow[16]; // object -> flow frame (units of the vessel's size)
		float scale[4];   // 1 / WN
	};
	struct JfaPC {
		int32_t mode;     // 0: seeds; 1: A -> B, 2: B -> A (step); 3, 4: the field from A, B
		int32_t step;
		float wn;
		float pad;
	};
	struct VolPC {
		float toFlow[16]; // view -> flow frame (units of the vessel's size)
		float tile[4];    // atlas offset (uv), tile scale (uv), WN
		float box[4];     // s: start, near/far split, end; the streaks' phase
		float look[4];    // glow, the streaks' stretch, shock layer thickness (units of size), size [m]
		float misc[4];    // jitter frame, sheet length, wake length (units of size), intensity
	};
	static_assert (sizeof (VolPC) <= 128, "push constants");
}

// ======================================================================
// SheathState

float SheathState::Level (double flux)
{
	return (float)std::clamp ((flux - 5e4) / (8e5 - 5e4), 0.0, 1.0);
}

float SheathState::Glow (double flux)
{
	// (the gas glows well before the hull is hot: Starship's onboard views
	// show it vivid at 83 km and 7.4 km/s, some 2.5e5 W/m^2 - by the log of
	// the flux, from about 100 km at orbital speed)
	const double q0 = 3e4, q1 = 5e5;
	if (flux <= q0) return 0.0f;
	return (float)std::clamp (std::log (flux / q0) / std::log (q1 / q0), 0.0, 1.0);
}

void SheathState::Colour (float t, float rgb[3])
{
	// the hull's heat glowing by its temperature, from a dull red at the
	// onset through orange to a pinkish white at peak heating
	static const float key[5][4] = {
		{ 0.00f, 0.55f, 0.06f, 0.02f },
		{ 0.25f, 1.00f, 0.30f, 0.06f },
		{ 0.50f, 1.00f, 0.52f, 0.22f },
		{ 0.75f, 1.00f, 0.66f, 0.62f },
		{ 1.00f, 1.00f, 0.86f, 0.92f } };
	t = std::clamp (t, 0.0f, 1.0f);
	int i = 0;
	while (i < 3 && t > key[i + 1][0]) i++;
	float f = (t - key[i][0]) / (key[i + 1][0] - key[i][0]);
	for (int k = 0; k < 3; k++) rgb[k] = key[i][k + 1] + (key[i + 1][k + 1] - key[i][k + 1]) * f;
}

void SheathState::GasColour (float glow, float rgb[3])
{
	// (the layer's mean: pink-magenta, whiter as its hot core grows)
	float g = std::clamp (glow, 0.0f, 1.0f);
	rgb[0] = 1.0f, rgb[1] = 0.32f + 0.22f * g, rgb[2] = 0.62f - 0.08f * g;
}

double SheathState::Thickness (double size) const
{
	// (the stand-off of a blunt body's shock is a few per cent of its size;
	// the glowing layer grows with the heating)
	return size * (0.02 + 0.05 * level);
}

float SheathState::Radiance () const
{
	// (the volume's shock layer and sheets around the vessel, seen from it:
	// their mean radiance)
	return 0.45f * VOL_INTENSITY * glow * glow;
}

// ======================================================================
// PlasmaRenderer

bool PlasmaRenderer::Init (vkc::Context *c, VkDescriptorSetLayout frameLayout, VkImageView detailNoise)
{
	ctx = c;
	detailView = detailNoise;

	// ---- the shell (more heated hulls than tiles)
	VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (PushConstants) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1, plci.pSetLayouts = &frameLayout;
	plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));

	// (the meshes' vertices: NTVERTEX)
	VkVertexInputBindingDescription bind{ 0, 8 * sizeof (float), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 } };
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 2, vi.pVertexAttributeDescriptions = attr;
	VkPipelineVertexInputStateCreateInfo viNone{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1, vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	// (both sides: a view ray grazing the vessel passes through the layer
	// on the near and the far side; the hull hides the far side elsewhere)
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = ctx->sceneSamples;
	VkPipelineMultisampleStateCreateInfo ms1{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms1.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_FALSE;
	ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed z
	VkPipelineDepthStencilStateCreateInfo dsNone{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorBlendAttachmentState cba{}; // additive
	cba.colorWriteMask = 0xF;
	cba.blendEnable = VK_TRUE;
	cba.srcColorBlendFactor = cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.colorBlendOp = VK_BLEND_OP_ADD;
	cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.alphaBlendOp = VK_BLEND_OP_ADD;
	// reactive mask (temporal anti-aliasing): the strongest contribution
	VkPipelineColorBlendAttachmentState cbm{};
	cbm.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
	cbm.blendEnable = VK_TRUE;
	cbm.srcColorBlendFactor = cbm.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cbm.colorBlendOp = VK_BLEND_OP_MAX;
	cbm.srcAlphaBlendFactor = cbm.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cbm.alphaBlendOp = VK_BLEND_OP_MAX;
	VkPipelineColorBlendAttachmentState cbs[2] = { cba, cbm };
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 2, cb.pAttachments = cbs;
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	VkFormat formats[2] = { ctx->sceneFormat, VK_FORMAT_R8_UNORM }; // (Scene::REACTIVE_FORMAT)
	rci.colorAttachmentCount = 2, rci.pColorAttachmentFormats = formats;
	rci.depthAttachmentFormat = ctx->depthFormat;
	auto stagesOf = [this](const unsigned char *v, size_t vn, const unsigned char *f, size_t fn, VkShaderModule mods[2],
		VkPipelineShaderStageCreateInfo st[2]) {
		mods[0] = ctx->CreateShaderModule (v, vn), mods[1] = ctx->CreateShaderModule (f, fn);
		st[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, mods[0], "main" };
		st[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mods[1], "main" };
	};
	VkShaderModule mods[2];
	VkPipelineShaderStageCreateInfo stages[2];
	stagesOf (spv_plasma_vert, spv_plasma_vert_size, spv_plasma_frag, spv_plasma_frag_size, mods, stages);
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
	gpi.layout = layout;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));
	for (VkShaderModule m : mods) vkDestroyShaderModule (ctx->device, m, nullptr);

	// ---- samplers, sets
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &linearClamp));
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &repeatSampler));
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &nearestClamp));
	const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CI = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	auto makeLayout = [this](const std::vector<VkDescriptorSetLayoutBinding> &b) {
		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = (uint32_t)b.size (), lci.pBindings = b.data ();
		VkDescriptorSetLayout l;
		VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &l));
		return l;
	};
	const VkShaderStageFlags CS = VK_SHADER_STAGE_COMPUTE_BIT, FS = VK_SHADER_STAGE_FRAGMENT_BIT;
	// field: the flow maps, the seeds (ping-pong), the field
	jfaSetLayout = makeLayout ({ { 0, CI, 1, CS, nullptr }, { 1, SI, 1, CS, nullptr }, { 2, SI, 1, CS, nullptr }, { 3, SI, 1, CS, nullptr } });
	// march: the field, the prepass, the noise
	volSetLayout = makeLayout ({ { 0, CI, 1, FS, nullptr }, { 1, CI, 1, FS, nullptr }, { 2, CI, 1, FS, nullptr } });
	VkDescriptorPoolSize psz[2] = { { SI, 3 }, { CI, 4 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2, pci.poolSizeCount = 2, pci.pPoolSizes = psz;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	VkDescriptorSetLayout ls[2] = { jfaSetLayout, volSetLayout };
	VkDescriptorSet sets[2];
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool, dai.descriptorSetCount = 2, dai.pSetLayouts = ls;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, sets));
	jfaSet = sets[0], volSet = sets[1];

	// ---- the flow maps: the meshes seen along the flow, the windward and
	// leeward surfaces' s (min of s, of -s)
	{
		VkPushConstantRange r{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof (FlowPC) };
		VkPipelineLayoutCreateInfo l{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		l.pushConstantRangeCount = 1, l.pPushConstantRanges = &r;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &l, nullptr, &flowLayout));
		VkPipelineColorBlendAttachmentState mn{};
		mn.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
		mn.blendEnable = VK_TRUE;
		mn.srcColorBlendFactor = mn.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		mn.colorBlendOp = VK_BLEND_OP_MIN;
		mn.srcAlphaBlendFactor = mn.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		mn.alphaBlendOp = VK_BLEND_OP_MIN;
		VkPipelineColorBlendStateCreateInfo cbf{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cbf.attachmentCount = 1, cbf.pAttachments = &mn;
		VkPipelineRenderingCreateInfo rf{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		VkFormat ff = FLOW_FORMAT;
		rf.colorAttachmentCount = 1, rf.pColorAttachmentFormats = &ff;
		stagesOf (spv_plasma_flow_vert, spv_plasma_flow_vert_size, spv_plasma_flow_frag, spv_plasma_flow_frag_size, mods, stages);
		VkPipelineVertexInputStateCreateInfo viPos = vi; // (the positions only)
		viPos.vertexAttributeDescriptionCount = 1;
		VkGraphicsPipelineCreateInfo g = gpi;
		g.pNext = &rf;
		g.pVertexInputState = &viPos;
		g.pStages = stages;
		g.pMultisampleState = &ms1;
		g.pDepthStencilState = &dsNone;
		g.pColorBlendState = &cbf;
		g.layout = flowLayout;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &g, nullptr, &flowPipe));
		for (VkShaderModule m : mods) vkDestroyShaderModule (ctx->device, m, nullptr);
	}
	// ---- the field: jump flooding from the outline
	{
		VkPushConstantRange r{ CS, 0, sizeof (JfaPC) };
		VkPipelineLayoutCreateInfo l{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		l.setLayoutCount = 1, l.pSetLayouts = &jfaSetLayout;
		l.pushConstantRangeCount = 1, l.pPushConstantRanges = &r;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &l, nullptr, &jfaLayout));
		VkShaderModule m = ctx->CreateShaderModule (spv_plasma_jfa_comp, spv_plasma_jfa_comp_size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main" };
		cpi.layout = jfaLayout;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &jfaPipe));
		vkDestroyShaderModule (ctx->device, m, nullptr);
	}
	// ---- the march: the box's far side, additive, up to the prepass depth
	{
		VkPushConstantRange r{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (VolPC) };
		VkDescriptorSetLayout sl[2] = { frameLayout, volSetLayout };
		VkPipelineLayoutCreateInfo l{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		l.setLayoutCount = 2, l.pSetLayouts = sl;
		l.pushConstantRangeCount = 1, l.pPushConstantRanges = &r;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &l, nullptr, &volLayout));
		stagesOf (spv_plasma_vol_vert, spv_plasma_vol_vert_size, spv_plasma_vol_frag, spv_plasma_vol_frag_size, mods, stages);
		VkGraphicsPipelineCreateInfo g = gpi;
		g.pStages = stages;
		g.pVertexInputState = &viNone;
		g.pDepthStencilState = &dsNone; // (none: the march stops at the prepass depth)
		g.layout = volLayout;
		VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &g, nullptr, &volPipe));
		for (VkShaderModule m : mods) vkDestroyShaderModule (ctx->device, m, nullptr);
	}
	return true;
}

void PlasmaRenderer::Shutdown ()
{
	if (!ctx) return;
	FreeAtlas ();
	for (VkPipeline p : { pipe, flowPipe, jfaPipe, volPipe }) if (p) vkDestroyPipeline (ctx->device, p, nullptr);
	for (VkPipelineLayout l : { layout, flowLayout, jfaLayout, volLayout }) if (l) vkDestroyPipelineLayout (ctx->device, l, nullptr);
	for (VkDescriptorSetLayout l : { jfaSetLayout, volSetLayout }) if (l) vkDestroyDescriptorSetLayout (ctx->device, l, nullptr);
	if (pool) vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	for (VkSampler s : { linearClamp, nearestClamp, repeatSampler }) if (s) vkDestroySampler (ctx->device, s, nullptr);
	pipe = flowPipe = jfaPipe = volPipe = VK_NULL_HANDLE;
	layout = flowLayout = jfaLayout = volLayout = VK_NULL_HANDLE;
	ctx = nullptr;
}

bool PlasmaRenderer::AllocAtlas ()
{
	// (kept once made: some 20 MB; its sets are written while no frame uses them)
	auto create = [this](VkFormat fmt, VkImageUsageFlags usage) {
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = fmt;
		ici.extent = { ATLAS, ATLAS, 1 };
		ici.mipLevels = 1, ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image, vci.viewType = VK_IMAGE_VIEW_TYPE_2D, vci.format = fmt;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		return im;
	};
	flowMap = create (FLOW_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	seedA = create (VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
	seedB = create (VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
	field = create (FIELD_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	VkCommandBuffer cmd = ctx->Cmd ();
	vkc::ImageBarrier (cmd, flowMap.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	for (VkImage im : { seedA.image, seedB.image, field.image })
		vkc::ImageBarrier (cmd, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
	atlasAlloc = true;
	const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkDescriptorImageInfo ji[4] = { { nearestClamp, flowMap.view, R }, { VK_NULL_HANDLE, seedA.view, G },
		{ VK_NULL_HANDLE, seedB.view, G }, { VK_NULL_HANDLE, field.view, G } };
	VkWriteDescriptorSet w[4]{};
	for (uint32_t i = 0; i < 4; i++) {
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = jfaSet, w[i].dstBinding = i, w[i].descriptorCount = 1;
		w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[i].pImageInfo = &ji[i];
	}
	vkUpdateDescriptorSets (ctx->device, 4, w, 0, nullptr);
	WriteSets ();
	return true;
}

void PlasmaRenderer::FreeAtlas ()
{
	if (!atlasAlloc) return;
	vkDeviceWaitIdle (ctx->device);
	for (vkc::Image *im : { &flowMap, &seedA, &seedB, &field }) ctx->DestroyImage (*im);
	atlasAlloc = false;
}

void PlasmaRenderer::WriteSets ()
{
	if (!atlasAlloc || !prepassView || !detailView) return;
	const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkDescriptorImageInfo vi[3] = { { linearClamp, field.view, G }, { nearestClamp, prepassView, R }, { repeatSampler, detailView, R } };
	VkWriteDescriptorSet w[3]{};
	for (uint32_t i = 0; i < 3; i++) {
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = volSet, w[i].dstBinding = i, w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[i].pImageInfo = &vi[i];
	}
	vkUpdateDescriptorSets (ctx->device, 3, w, 0, nullptr);
}

void PlasmaRenderer::SetPrepass (VkImageView prepass)
{
	if (prepass == prepassView) return;
	// (the march set is bound by no frame in flight while it is written:
	// a new prepass comes with a new swapchain, after an idle wait)
	if (prepassView && atlasAlloc) vkDeviceWaitIdle (ctx->device);
	prepassView = prepass;
	WriteSets ();
}

void PlasmaRenderer::Begin (VkDescriptorSet fs, double simt)
{
	frameSet = fs;
	count = 0;
	prepared = false;
	sheaths.clear ();
	time = (float)std::fmod (simt, 1000.0);
	if (dumpState == 0 && getenv ("ORBITER_VK_PLASMA_DUMP")) dumpState = 1;
	if (dumpState == 2) {
		vkDeviceWaitIdle (ctx->device);
		vmaInvalidateAllocation (ctx->allocator, dumpBuf.alloc, 0, VK_WHOLE_SIZE);
		if (FILE *f = fopen (getenv ("ORBITER_VK_PLASMA_DUMP"), "wb")) {
			fwrite (dumpBuf.mapped, 8, (size_t)ATLAS * ATLAS, f);
			fclose (f);
			oapiWriteLogV ("VulkanClient: plasma field dumped (%u x %u RGBA16F)", ATLAS, ATLAS);
		}
		ctx->DestroyBuffer (dumpBuf);
		dumpState = 3;
	}
	// (the march's jitter: a new sequence each simulation step, the same
	// while paused)
	stepDt = std::clamp (simt - lastSimT, 0.0, 1.0);
	if (simt != lastSimT) frameNo++, lastSimT = simt;
	// (bodies gone for a while: their streaks forgotten)
	for (auto it = flows.begin (); it != flows.end ();) it = simt - it->second.seen > 5.0 || simt < it->second.seen ? flows.erase (it) : ++it;
}

int PlasmaRenderer::AddSheath (const SheathState &s, double size, const VECTOR3 &centreView, const void *key)
{
	if (!enabled || s.glow <= 0.0f || size <= 0.0) return -1;
	Sheath sh;
	sh.s = s, sh.size = size, sh.centre = centreView;
	// the streaks move downstream with the gas streaming off the hull, some
	// 0.3 of the airspeed - at a fixed rate (some 90 m/s for a glider) the
	// plasma of a 47 km/s entry crept back in slow motion. Stretched along
	// the flow by its motion over an exposure of three steps (at least 0.1
	// s): its blur, and never more than a third of a noise period's move
	// per frame (faster, it strobed)
	{
		double Vg = 0.3 * s.speed;
		double T = std::max (0.1, 3.0 * stepDt);
		double B = Vg * T / size;             // [sizes] moved over the exposure
		double g = 1.0 / (1.0 + B / 4.0);    // (the finer layer's period 4 sizes)
		Flow &fl = flows[key];
		fl.phase = std::fmod (fl.phase + 0.10 * g * Vg * stepDt / size, 2.0); // (the coarse layer's frequency 0.10/size; mod 2: the fine layer's 2.5 times it whole)
		fl.seen = lastSimT;
		sh.phase = (float)fl.phase, sh.stretch = (float)g;
	}
	sh.px = size / std::max (length (centreView), 1.0);
	// the flow's frame: s downstream (against the motion through the air),
	// x, y across it; in units of the vessel's size, about its centre
	VECTOR3 e3 = -unit (s.dirView);
	VECTOR3 e1 = unit (crossp (e3, std::fabs (e3.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0)));
	VECTOR3 e2 = crossp (e3, e1);
	MATRIX3 R = _M(e1.x, e1.y, e1.z, e2.x, e2.y, e2.z, e3.x, e3.y, e3.z);
	VECTOR3 t = -mul (R, centreView);
	double k = 1.0 / size;
	sh.toFlow = Mat4::Scale (k, k, k) * Mat4::FromRotTrans (R, t);
	sheaths.push_back (std::move (sh));
	return (int)sheaths.size () - 1;
}

void PlasmaRenderer::AddMesh (int i, const VkMesh *mesh, const Mat4 &modelView, const std::vector<Mat4> *groupTransforms,
	const std::vector<uint8_t> *groupVisible)
{
	if (i < 0 || i >= (int)sheaths.size () || !mesh || !mesh->VertexBuffer () || !mesh->IndexBuffer ()) return;
	sheaths[i].meshes.push_back ({ mesh, modelView, groupTransforms, groupVisible });
}

namespace {
	// a mesh's groups to be drawn: (group, its model-view)
	template <class F>
	void ForGroups (const VkMesh *mesh, const Mat4 &modelView, const std::vector<Mat4> *grpT, const std::vector<uint8_t> *grpVis, F f)
	{
		for (uint32_t g = 0; g < mesh->nGroup (); g++) {
			const VkMesh::Group &grp = mesh->GetGroup (g);
			if (!grp.nIndex || (grp.usrFlag & VkMesh::GRP_HIDDEN) || grp.transparent) continue;
			if (grpVis && g < grpVis->size () && !(*grpVis)[g]) continue;
			f (grp, (grpT && g < grpT->size ()) ? modelView * (*grpT)[g] : modelView);
		}
	}
}

void PlasmaRenderer::Prepare ()
{
	if (!Pending ()) return;
	double simt0 = oapiGetSimTime ();
	// the largest on the screen get the tiles (the rest: shells)
	std::vector<size_t> order (sheaths.size ());
	for (size_t i = 0; i < order.size (); i++) order[i] = i;
	std::sort (order.begin (), order.end (), [this](size_t a, size_t b) { return sheaths[a].px > sheaths[b].px; });
	uint32_t nt = 0;
	for (size_t i : order)
		if (nt < MAX_TILES && !sheaths[i].meshes.empty ()) sheaths[i].tile = (int)nt++;
	idleSince = oapiGetSysTime ();
	if (!nt) return;
	if (!atlasAlloc) AllocAtlas ();
	if (!prepassView) return;

	VkCommandBuffer cmd = ctx->Cmd ();
	// the flow maps: the meshes seen along the flow (MIN of s, -s)
	vkc::ImageBarrier (cmd, flowMap.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	ca.imageView = flowMap.view;
	ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	ca.clearValue.color = { { 1000.0f, 1000.0f, 0.0f, 0.0f } }; // (none: far beyond any s)
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = { { 0, 0 }, { ATLAS, ATLAS } };
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca;
	vkCmdBeginRendering (cmd, &ri);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flowPipe);
	FlowPC fp{};
	fp.scale[0] = 1.0f / WN;
	for (const Sheath &sh : sheaths) {
		if (sh.tile < 0) continue;
		VkViewport vp{ (float)(sh.tile % ATLAS_N * TILE), (float)(sh.tile / ATLAS_N * TILE), (float)TILE, (float)TILE, 0.0f, 1.0f };
		VkRect2D sc{ { (int32_t)(sh.tile % ATLAS_N * TILE), (int32_t)(sh.tile / ATLAS_N * TILE) }, { TILE, TILE } };
		vkCmdSetViewport (cmd, 0, 1, &vp);
		vkCmdSetScissor (cmd, 0, 1, &sc);
		for (const MeshRef &m : sh.meshes) {
			VkDeviceSize ofs = 0;
			VkBuffer vb = m.mesh->VertexBuffer ();
			vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
			vkCmdBindIndexBuffer (cmd, m.mesh->IndexBuffer (), 0, VK_INDEX_TYPE_UINT16);
			ForGroups (m.mesh, m.mv, m.grpT, m.grpVis, [&](const VkMesh::Group &grp, const Mat4 &mv) {
				(sh.toFlow * mv).ToFloat (fp.toFlow);
				vkCmdPushConstants (cmd, flowLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof (fp), &fp);
				vkCmdDrawIndexed (cmd, grp.nIndex, 1, grp.firstIndex, grp.vertexOffset, 0);
			});
		}
	}
	vkCmdEndRendering (cmd);
	vkc::ImageBarrier (cmd, flowMap.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

	// the field: jump flooding from the outline, each tile on its own
	auto barrier = [cmd](VkPipelineStageFlags2 s0, VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = s0, mb.srcAccessMask = a0, mb.dstStageMask = s1, mb.dstAccessMask = a1;
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2 (cmd, &dep);
	};
	const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	const VkAccessFlags2 RD = VK_ACCESS_2_SHADER_READ_BIT, WR = VK_ACCESS_2_SHADER_WRITE_BIT;
	barrier (FS | CS, RD | WR, CS, RD | WR); // (the last frame's march read the field)
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, jfaPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, jfaLayout, 0, 1, &jfaSet, 0, nullptr);
	auto pass = [&](int mode, int step) {
		JfaPC p{ mode, step, WN, 0.0f };
		vkCmdPushConstants (cmd, jfaLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (p), &p);
		vkCmdDispatch (cmd, TILE / 8, TILE / 8, nt);
		barrier (CS, WR, CS, RD | WR);
	};
	pass (0, 0);
	bool inA = true;
	for (int step = TILE / 2; step >= 1; step /= 2, inA = !inA) pass (inA ? 1 : 2, step);
	pass (inA ? 3 : 4, 0);
	barrier (CS, WR, FS, RD);
	if (dumpState == 1 && simt0 > 1.0) {
		dumpBuf = ctx->CreateReadbackBuffer ((VkDeviceSize)ATLAS * ATLAS * 8);
		barrier (CS, WR, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy rg{};
		rg.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		rg.imageExtent = { ATLAS, ATLAS, 1 };
		vkCmdCopyImageToBuffer (cmd, field.image, VK_IMAGE_LAYOUT_GENERAL, dumpBuf.buffer, 1, &rg);
		dumpState = 2;
	}
	ctx->GpuMark ("plasma field");
	prepared = true;
}

void PlasmaRenderer::DrawShell (const Sheath &sh)
{
	VkCommandBuffer cmd = ctx->Cmd ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frameSet, 0, nullptr);
	const SheathState &s = sh.s;
	PushConstants pc{};
	pc.flow[0] = (float)s.dirView.x, pc.flow[1] = (float)s.dirView.y, pc.flow[2] = (float)s.dirView.z, pc.flow[3] = s.level;
	pc.shape[0] = (float)s.Thickness (sh.size), pc.shape[1] = (float)sh.size, pc.shape[2] = time, pc.shape[3] = INTENSITY;
	pc.centre[0] = (float)sh.centre.x, pc.centre[1] = (float)sh.centre.y, pc.centre[2] = (float)sh.centre.z, pc.centre[3] = 0.0f;
	for (const MeshRef &m : sh.meshes) {
		VkDeviceSize ofs = 0;
		VkBuffer vb = m.mesh->VertexBuffer ();
		vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
		vkCmdBindIndexBuffer (cmd, m.mesh->IndexBuffer (), 0, VK_INDEX_TYPE_UINT16);
		ForGroups (m.mesh, m.mv, m.grpT, m.grpVis, [&](const VkMesh::Group &grp, const Mat4 &mv) {
			mv.ToFloat (pc.modelView);
			vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (pc), &pc);
			vkCmdDrawIndexed (cmd, grp.nIndex, 1, grp.firstIndex, grp.vertexOffset, 0);
		});
	}
}

void PlasmaRenderer::Draw ()
{
	if (!enabled || sheaths.empty ()) return;
	VkCommandBuffer cmd = ctx->Cmd ();
	bool bound = false;
	for (const Sheath &sh : sheaths) {
		if (sh.meshes.empty ()) continue;
		if (sh.tile < 0 || !prepared) {
			// (the shell: the level's cubed glow, as before the volume)
			if (sh.s.level > 0.0f) DrawShell (sh), bound = false, count++;
			continue;
		}
		if (!bound) {
			vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, volPipe);
			VkDescriptorSet s[2] = { frameSet, volSet };
			vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, volLayout, 0, 2, s, 0, nullptr);
			bound = true;
		}
		const float g = sh.s.glow;
		VolPC p{};
		sh.toFlow.ToFloat (p.toFlow);
		p.tile[0] = (float)(sh.tile % ATLAS_N) / ATLAS_N, p.tile[1] = (float)(sh.tile / ATLAS_N) / ATLAS_N;
		p.tile[2] = 1.0f / ATLAS_N, p.tile[3] = WN;
		// (the sheets fade over some vessel sizes, the lee wake's glow
		// behind it over more: longer at stronger heating)
		float Lc = 1.5f + 3.0f * g, Lw = 1.0f + 2.5f * g;
		p.box[0] = -1.6f, p.box[1] = 4.0f, p.box[2] = std::min (4.0f + 3.0f * Lw, 20.0f), p.box[3] = sh.phase;
		p.look[0] = g, p.look[1] = sh.stretch, p.look[2] = 0.035f + 0.035f * g, p.look[3] = (float)sh.size;
		p.misc[0] = (float)(frameNo & 0xffff), p.misc[1] = Lc, p.misc[2] = Lw, p.misc[3] = VOL_INTENSITY * g * g;
		vkCmdPushConstants (cmd, volLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (p), &p);
		vkCmdDraw (cmd, 36, 1, 0, 0);
		count++;
	}
	if (count) ctx->GpuMark ("plasma");
}

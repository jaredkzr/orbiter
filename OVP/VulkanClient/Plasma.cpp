// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Plasma.h"
#include "VkMesh.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "plasma.vert.h"
#include "plasma.frag.h"

namespace {
	// radiance of the layer at the stagnation point at peak heating, seen
	// face-on (about the sunlit hull's; more seen at a slant)
	constexpr float INTENSITY = 2.6f;
	struct PushConstants {
		float modelView[16];
		float flow[4];   // direction of motion through the air (view), w: heating level 0..1
		float shape[4];  // thickness [m], vessel radius [m], time [s], intensity
		float centre[4]; // the vessel's centre (view), w: -
	};
	static_assert (sizeof (PushConstants) <= 128, "push constants");
}

// ======================================================================
// SheathState

void SheathState::Colour (float t, float rgb[3])
{
	// the shock layer's air glowing by its temperature, from a dull red at
	// the onset through orange to a pinkish white at peak heating (the
	// violet-blue of ionised nitrogen and oxygen at its outer edge: the shader)
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

double SheathState::Thickness (double size) const
{
	// (the stand-off of a blunt body's shock is a few per cent of its size;
	// the glowing layer grows with the heating)
	return size * (0.02 + 0.05 * level);
}

float SheathState::Radiance () const
{
	// (shader: intensity * level^3 at the stagnation point; the windward
	// side's mean some 0.4 of it)
	return 0.4f * INTENSITY * level * level * level;
}

// ======================================================================
// PlasmaRenderer

bool PlasmaRenderer::Init (vkc::Context *c, VkDescriptorSetLayout frameLayout)
{
	ctx = c;
	VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (PushConstants) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1, plci.pSetLayouts = &frameLayout;
	plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));

	VkShaderModule vs = ctx->CreateShaderModule (spv_plasma_vert, spv_plasma_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_plasma_frag, spv_plasma_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};
	// (the meshes' vertices: NTVERTEX)
	VkVertexInputBindingDescription bind{ 0, 8 * sizeof (float), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 } };
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 2, vi.pVertexAttributeDescriptions = attr;
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
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_FALSE;
	ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed z
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
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);
	return true;
}

void PlasmaRenderer::Shutdown ()
{
	if (!ctx) return;
	if (pipe) vkDestroyPipeline (ctx->device, pipe, nullptr);
	if (layout) vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	pipe = VK_NULL_HANDLE, layout = VK_NULL_HANDLE;
	ctx = nullptr;
}

void PlasmaRenderer::Begin (VkDescriptorSet fs, double simt)
{
	frameSet = fs;
	bound = false;
	count = 0;
	lastState = nullptr;
	time = (float)std::fmod (simt, 1000.0);
}

void PlasmaRenderer::DrawMesh (const VkMesh *mesh, const Mat4 &modelView, const std::vector<Mat4> *groupTransforms,
	const std::vector<uint8_t> *groupVisible, const SheathState &s, double size)
{
	if (!enabled || !pipe || !mesh || !mesh->VertexBuffer () || !mesh->IndexBuffer () || s.level <= 0.0f) return;
	VkCommandBuffer cmd = ctx->Cmd ();
	if (!bound) {
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frameSet, 0, nullptr);
		bound = true;
	}
	if (lastState != &s) count++, lastState = &s;
	VkDeviceSize ofs = 0;
	VkBuffer vb = mesh->VertexBuffer ();
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
	vkCmdBindIndexBuffer (cmd, mesh->IndexBuffer (), 0, VK_INDEX_TYPE_UINT16);
	PushConstants pc{};
	pc.flow[0] = (float)s.dirView.x, pc.flow[1] = (float)s.dirView.y, pc.flow[2] = (float)s.dirView.z, pc.flow[3] = s.level;
	pc.shape[0] = (float)s.Thickness (size), pc.shape[1] = (float)size, pc.shape[2] = time, pc.shape[3] = INTENSITY;
	VECTOR3 c = modelView.TransformPoint (_V(0, 0, 0));
	pc.centre[0] = (float)c.x, pc.centre[1] = (float)c.y, pc.centre[2] = (float)c.z, pc.centre[3] = 0.0f;
	for (uint32_t g = 0; g < mesh->nGroup (); g++) {
		const VkMesh::Group &grp = mesh->GetGroup (g);
		if (!grp.nIndex || (grp.usrFlag & VkMesh::GRP_HIDDEN) || grp.transparent) continue;
		if (groupVisible && g < groupVisible->size () && !(*groupVisible)[g]) continue;
		Mat4 mv = (groupTransforms && g < groupTransforms->size ()) ? modelView * (*groupTransforms)[g] : modelView;
		mv.ToFloat (pc.modelView);
		vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (pc), &pc);
		vkCmdDrawIndexed (cmd, grp.nIndex, 1, grp.firstIndex, grp.vertexOffset, 0);
	}
}

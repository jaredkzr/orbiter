// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Shadows.h"
#include "VkMesh.h"
#include <cmath>

#include "shadow.vert.h"

namespace {
	// depth range of the shadow volume, in units of its radius: casters
	// up to DEPTH_NEAR radii towards the sun, receivers DEPTH_FAR beyond
	constexpr double DEPTH_NEAR = 4.0, DEPTH_FAR = 2.0;
	const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
}

bool ShadowMap::Init (vkc::Context *c)
{
	ctx = c;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = DEPTH_FORMAT;
	ici.extent = { SIZE, SIZE, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &depth.image, &depth.alloc, nullptr));
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = depth.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = DEPTH_FORMAT;
	vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &depth.view));
	VkImage img = depth.image;
	ctx->ImmediateSubmit ([img](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_DEPTH_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.compareEnable = VK_TRUE;
	sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // lit if not behind the stored depth
	sci.maxLod = 0.0f;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &sampler));

	VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof(float) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));

	VkShaderModule vs = ctx->CreateShaderModule (spv_shadow_vert, spv_shadow_vert_size);
	VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" };
	VkVertexInputBindingDescription bind{ 0, sizeof(NTVERTEX), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(NTVERTEX, x) };
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 1;
	vi.pVertexAttributeDescriptions = &attr;
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.depthBiasEnable = VK_TRUE;
	rs.depthBiasConstantFactor = 1.5f;
	rs.depthBiasSlopeFactor = 2.0f;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;
	VkFormat depthFormat = DEPTH_FORMAT;
	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.depthAttachmentFormat = depthFormat;
	VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gpi.pNext = &rci;
	gpi.stageCount = 1;
	gpi.pStages = &stage;
	gpi.pVertexInputState = &vi;
	gpi.pInputAssemblyState = &ia;
	gpi.pViewportState = &vps;
	gpi.pRasterizationState = &rs;
	gpi.pMultisampleState = &ms;
	gpi.pDepthStencilState = &ds;
	gpi.pColorBlendState = &cb;
	gpi.pDynamicState = &dy;
	gpi.layout = layout;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline));
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	return true;
}

void ShadowMap::Shutdown ()
{
	if (!ctx) return;
	vkDestroyPipeline (ctx->device, pipeline, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	vkDestroySampler (ctx->device, sampler, nullptr);
	ctx->DestroyImage (depth);
	ctx = nullptr;
}

void ShadowMap::Begin (const VECTOR3 &c, double r, const VECTOR3 &sunDir)
{
	radius = r;
	// light frame: f points along the sunlight, ex/ey span the map
	VECTOR3 f = -sunDir;
	VECTOR3 ref = fabs (f.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0);
	VECTOR3 ex = unit (crossp (ref, f));
	VECTOR3 ey = crossp (f, ex);

	// move the centre in whole texels across the map, so that the shadow
	// edges don't crawl when the camera moves
	double texel = TexelSize ();
	double cx = dotp (c, ex), cy = dotp (c, ey);
	centre = c + ex * (std::floor (cx / texel) * texel - cx) + ey * (std::floor (cy / texel) * texel - cy);

	// orthographic projection of the shadow volume (depth 0 towards the sun)
	double dn = DEPTH_NEAR * r, df = DEPTH_FAR * r;
	Mat4 m = Mat4::Identity ();
	m.m[0] = ex.x / r; m.m[4] = ex.y / r; m.m[8]  = ex.z / r;
	m.m[1] = ey.x / r; m.m[5] = ey.y / r; m.m[9]  = ey.z / r;
	m.m[2] = f.x / (dn + df); m.m[6] = f.y / (dn + df); m.m[10] = f.z / (dn + df);
	m.m[14] = dn / (dn + df);
	lightVP = m;

	ctx->BindDepthTarget ({ depth.image, depth.view, DEPTH_FORMAT, { SIZE, SIZE } });
	vkCmdBindPipeline (ctx->Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	active = true;
}

void ShadowMap::End ()
{
	if (!active) return;
	ctx->EndTarget ();
	active = false;
}

void ShadowMap::DrawMesh (const VkMesh *mesh, const Mat4 &model,
	const std::vector<Mat4> *groupTransforms, const std::vector<uint8_t> *groupVisible)
{
	if (!active || !mesh->VertexBuffer() || !mesh->IndexBuffer()) return;
	VkCommandBuffer cmd = ctx->Cmd();
	VkDeviceSize ofs = 0;
	VkBuffer vb = mesh->VertexBuffer();
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
	vkCmdBindIndexBuffer (cmd, mesh->IndexBuffer(), 0, VK_INDEX_TYPE_UINT16);
	Mat4 mvp0 = lightVP * model;
	for (uint32_t g = 0; g < mesh->nGroup(); g++) {
		const VkMesh::Group &grp = mesh->GetGroup (g);
		if (!grp.nIndex || grp.transparent || (grp.usrFlag & (VkMesh::GRP_HIDDEN | VkMesh::GRP_NOSHADOW))) continue;
		if (groupVisible && g < groupVisible->size() && !(*groupVisible)[g]) continue;
		Mat4 mvp = (groupTransforms && g < groupTransforms->size()) ? mvp0 * (*groupTransforms)[g] : mvp0;
		float f[16];
		mvp.ToFloat (f);
		vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f), f);
		vkCmdDrawIndexed (cmd, grp.nIndex, 1, grp.firstIndex, grp.vertexOffset, 0);
	}
}

void ShadowMap::DrawGeometry (VkBuffer vb, VkBuffer ib, uint32_t nIdx, const Mat4 &model)
{
	if (!active) return;
	VkCommandBuffer cmd = ctx->Cmd();
	VkDeviceSize ofs = 0;
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
	vkCmdBindIndexBuffer (cmd, ib, 0, VK_INDEX_TYPE_UINT16);
	float f[16];
	(lightVP * model).ToFloat (f);
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f), f);
	vkCmdDrawIndexed (cmd, nIdx, 1, 0, 0, 0);
}

bool ShadowMap::MayCast (const VECTOR3 &gpos, double rad) const
{
	// the shadow volume: the map's square across, from DEPTH_NEAR radii
	// towards the sun to DEPTH_FAR beyond (tested as a bounding sphere)
	return length (gpos - centre) - rad < radius * (DEPTH_NEAR + 1.5);
}

Mat4 ShadowMap::ReceiverMatrix (const VECTOR3 &camPos, const MATRIX3 &camRot) const
{
	// view space -> global relative to the shadow centre -> shadow clip -> texture coordinates
	Mat4 bias = Mat4::Identity ();
	bias.m[0] = 0.5; bias.m[12] = 0.5;
	bias.m[5] = 0.5; bias.m[13] = 0.5;
	return bias * lightVP * Mat4::FromRotTrans (camRot, camPos - centre);
}

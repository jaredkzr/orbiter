// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Draw2D.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "draw2d.vert.h"
#include "draw2d.frag.h"

namespace {

	// Must match the push constant block of draw2d.vert/frag
	struct PushConstants2D {
		float scale[2];
		float pad[2];
		float ckey[4];
		uint32_t mode;
	};

	constexpr uint32_t MODE_FETCH = 1;
	constexpr uint32_t MODE_CKEY = 2;

	struct LinearTable {
		float v[256];
		LinearTable () {
			for (int i = 0; i < 256; i++) {
				double c = i / 255.0;
				v[i] = (float)(c <= 0.04045 ? c / 12.92 : std::pow ((c + 0.055) / 1.055, 2.4));
			}
		}
	};
	const LinearTable linearTable;

} // namespace

float SRGBToLinear (uint8_t c)
{
	return linearTable.v[c];
}

void ColorRefToLinear (uint32_t bgr, float alpha, float out[4])
{
	out[0] = SRGBToLinear (bgr & 0xFF);
	out[1] = SRGBToLinear ((bgr >> 8) & 0xFF);
	out[2] = SRGBToLinear ((bgr >> 16) & 0xFF);
	out[3] = alpha;
}

void DeviceColorToLinear (uint32_t rgb, float alpha, float out[4])
{
	out[0] = SRGBToLinear ((rgb >> 16) & 0xFF);
	out[1] = SRGBToLinear ((rgb >> 8) & 0xFF);
	out[2] = SRGBToLinear (rgb & 0xFF);
	out[3] = alpha;
}

// ======================================================================

bool Renderer2D::Init (vkc::Context *c, TextureManager *tm)
{
	ctx = c;
	texmgr = tm;

	VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants2D) };
	VkDescriptorSetLayout setLayout = texmgr->Layout();
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));

	vs = ctx->CreateShaderModule (spv_draw2d_vert, spv_draw2d_vert_size);
	fs = ctx->CreateShaderModule (spv_draw2d_frag, spv_draw2d_frag_size);
	return true;
}

void Renderer2D::Shutdown ()
{
	if (!ctx) return;
	batch.clear();
	if (scratch) texmgr->Release (scratch);
	scratch = nullptr;
	for (auto &it : pipelines) vkDestroyPipeline (ctx->device, it.second, nullptr);
	pipelines.clear();
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	ctx = nullptr;
}

VkPipeline Renderer2D::Pipeline (VkFormat fmt, Blend2D blend)
{
	auto key = std::make_pair (fmt, blend);
	auto it = pipelines.find (key);
	if (it != pipelines.end()) return it->second;

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vs;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fs;
	stages[1].pName = "main";

	VkVertexInputBindingDescription bind{ 0, sizeof(Vtx2D), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[3] = {
		{ 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vtx2D, x) },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vtx2D, u) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vtx2D, r) },
	};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 3;
	vi.pVertexAttributeDescriptions = attr;

	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	switch (blend) {
	case Blend2D::Copy:
		break;
	case Blend2D::Alpha:
		cba.blendEnable = VK_TRUE;
		cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		cba.colorBlendOp = VK_BLEND_OP_ADD;
		cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		cba.alphaBlendOp = VK_BLEND_OP_ADD;
		break;
	case Blend2D::Additive:
		cba.blendEnable = VK_TRUE;
		cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.colorBlendOp = VK_BLEND_OP_ADD;
		cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.alphaBlendOp = VK_BLEND_OP_ADD;
		break;
	}
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;

	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dsi.dynamicStateCount = 2;
	dsi.pDynamicStates = dyn;

	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.colorAttachmentCount = 1;
	rci.pColorAttachmentFormats = &fmt;

	VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gpci.pNext = &rci;
	gpci.stageCount = 2;
	gpci.pStages = stages;
	gpci.pVertexInputState = &vi;
	gpci.pInputAssemblyState = &ia;
	gpci.pViewportState = &vps;
	gpci.pRasterizationState = &rs;
	gpci.pMultisampleState = &ms;
	gpci.pDepthStencilState = &ds;
	gpci.pColorBlendState = &cb;
	gpci.pDynamicState = &dsi;
	gpci.layout = layout;
	VkPipeline pipe = VK_NULL_HANDLE;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe));
	pipelines[key] = pipe;
	return pipe;
}

// ======================================================================
// Targets and batches

bool Renderer2D::Begin (VkTexture *tgt)
{
	if (hasTarget && tgt == target && (tgt || ctx->InFrame())) return true;
	Flush ();
	if (!tgt) {
		if (!ctx->InFrame()) { hasTarget = false; return false; }
	} else if (!EnsureRenderTarget (tgt)) {
		hasTarget = false;
		return false;
	}
	target = tgt;
	hasTarget = true;
	return true;
}

VkExtent2D Renderer2D::TargetSize () const
{
	if (target) return { target->width, target->height };
	return ctx->Extent();
}

void Renderer2D::BindCurrent ()
{
	if (target) ctx->BindTarget (target->AsTarget());
	else ctx->BindBackbuffer (false);
}

Vtx2D *Renderer2D::AddTris (const DrawState2D &st, size_t nvtx)
{
	if (!hasTarget) return nullptr;
	if (!batch.empty() && !(st == batchState)) Flush ();
	batchState = st;
	size_t n = batch.size();
	batch.resize (n + nvtx);
	return batch.data() + n;
}

void Renderer2D::Quad (const DrawState2D &st, float x0, float y0, float x1, float y1,
	float u0, float v0, float u1, float v1, const float col[4])
{
	Vtx2D *v = AddTris (st, 6);
	if (!v) return;
	Vtx2D a{ x0, y0, u0, v0, col[0], col[1], col[2], col[3] };
	Vtx2D b{ x1, y0, u1, v0, col[0], col[1], col[2], col[3] };
	Vtx2D c{ x1, y1, u1, v1, col[0], col[1], col[2], col[3] };
	Vtx2D d{ x0, y1, u0, v1, col[0], col[1], col[2], col[3] };
	v[0] = a, v[1] = b, v[2] = c;
	v[3] = a, v[4] = c, v[5] = d;
}

void Renderer2D::Flush ()
{
	if (batch.empty()) return;
	if (!hasTarget || (!target && !ctx->InFrame())) { batch.clear(); return; }

	BindCurrent ();
	VkCommandBuffer cmd = ctx->Cmd();
	VkExtent2D ext = ctx->BoundExtent();

	vkc::Context::Transient vb = ctx->AllocTransient (batch.size() * sizeof(Vtx2D));
	memcpy (vb.ptr, batch.data(), batch.size() * sizeof(Vtx2D));

	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipeline (ctx->BoundFormat(), batchState.blend));
	VkTexture *tex = batchState.tex ? batchState.tex : texmgr->White();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &tex->set, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb.buffer, &vb.offset);

	VkRect2D sc{ {0, 0}, ext };
	if (batchState.clip.extent.width && batchState.clip.extent.height) {
		int32_t x0 = std::max (0, batchState.clip.offset.x);
		int32_t y0 = std::max (0, batchState.clip.offset.y);
		int32_t x1 = std::min ((int32_t)ext.width, batchState.clip.offset.x + (int32_t)batchState.clip.extent.width);
		int32_t y1 = std::min ((int32_t)ext.height, batchState.clip.offset.y + (int32_t)batchState.clip.extent.height);
		sc = { { x0, y0 }, { (uint32_t)std::max (0, x1 - x0), (uint32_t)std::max (0, y1 - y0) } };
	}
	vkCmdSetScissor (cmd, 0, 1, &sc);

	PushConstants2D pc{};
	pc.scale[0] = 2.0f / ext.width;
	pc.scale[1] = 2.0f / ext.height;
	if (batchState.tex && batchState.fetch) pc.mode |= MODE_FETCH;
	if (batchState.tex && batchState.ckey != VkTexture::NO_CKEY) {
		pc.mode |= MODE_CKEY;
		pc.ckey[0] = ((batchState.ckey >> 16) & 0xFF) / 255.0f;
		pc.ckey[1] = ((batchState.ckey >> 8) & 0xFF) / 255.0f;
		pc.ckey[2] = (batchState.ckey & 0xFF) / 255.0f;
	}
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	if (sc.extent.width && sc.extent.height)
		vkCmdDraw (cmd, (uint32_t)batch.size(), 1, 0, 0);

	// restore the full scissor for other users of this rendering scope
	VkRect2D full{ {0, 0}, ext };
	vkCmdSetScissor (cmd, 0, 1, &full);
	batch.clear();
}

// ======================================================================
// Surface operations

bool Renderer2D::EnsureRenderTarget (VkTexture *tex)
{
	if (tex->renderTarget) return true;
	vkc::Image img;
	if (!texmgr->CreateTargetImage (tex->width, tex->height, img)) return false;
	Flush ();
	VkCommandBuffer cmd = ctx->Cmd();
	ctx->EndTarget ();
	vkc::ImageBarrier (cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);

	// copy the top mip level into the new image
	vkc::Target t{ img.image, img.view, VK_FORMAT_R8G8B8A8_SRGB, { tex->width, tex->height } };
	ctx->BindTarget (t);
	Vtx2D v[6];
	float w = (float)tex->width, h = (float)tex->height;
	Vtx2D a{ 0, 0, 0, 0, 1, 1, 1, 1 }, b{ w, 0, w, 0, 1, 1, 1, 1 }, c{ w, h, w, h, 1, 1, 1, 1 }, d{ 0, h, 0, h, 1, 1, 1, 1 };
	v[0] = a, v[1] = b, v[2] = c, v[3] = a, v[4] = c, v[5] = d;
	vkc::Context::Transient vb = ctx->AllocTransient (sizeof(v));
	memcpy (vb.ptr, v, sizeof(v));
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipeline (VK_FORMAT_R8G8B8A8_SRGB, Blend2D::Copy));
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &tex->set, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb.buffer, &vb.offset);
	PushConstants2D pc{};
	pc.scale[0] = 2.0f / w;
	pc.scale[1] = 2.0f / h;
	pc.mode = MODE_FETCH;
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw (cmd, 6, 1, 0, 0);
	ctx->EndTarget ();

	texmgr->ReplaceImage (tex, img);
	return true;
}

bool Renderer2D::CopyToScratch (VkTexture *src, int sx, int sy, int sw, int sh)
{
	if (!scratch || scratch->width < (uint32_t)sw || scratch->height < (uint32_t)sh) {
		uint32_t w = std::max ((uint32_t)sw, scratch ? scratch->width : 0u);
		uint32_t h = std::max ((uint32_t)sh, scratch ? scratch->height : 0u);
		if (scratch) texmgr->Release (scratch);
		scratch = texmgr->CreateSurface (w, h);
		if (!scratch) return false;
	}
	Flush ();
	VkCommandBuffer cmd = ctx->Cmd();
	ctx->EndTarget ();
	vkc::ImageBarrier (cmd, src->img.image, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	vkc::ImageBarrier (cmd, scratch->img.image, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkImageCopy region{};
	region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.srcOffset = { sx, sy, 0 };
	region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.extent = { (uint32_t)sw, (uint32_t)sh, 1 };
	vkCmdCopyImage (cmd, src->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		scratch->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	vkc::ImageBarrier (cmd, src->img.image, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	vkc::ImageBarrier (cmd, scratch->img.image, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	return true;
}

bool Renderer2D::Blit (VkTexture *tgt, int tx, int ty, int tw, int th,
	VkTexture *src, int sx, int sy, int sw, int sh, bool useCkey)
{
	if (!src || sw <= 0 || sh <= 0 || tw <= 0 || th <= 0) return false;
	// clip the source rectangle to the source surface
	if (sx < 0 || sy < 0 || sx + sw > (int)src->width || sy + sh > (int)src->height) {
		int x0 = std::max (sx, 0), y0 = std::max (sy, 0);
		int x1 = std::min (sx + sw, (int)src->width), y1 = std::min (sy + sh, (int)src->height);
		if (x1 <= x0 || y1 <= y0) return false;
		float fx = (float)tw / sw, fy = (float)th / sh;
		tx += (int)std::lround ((x0 - sx) * fx);
		ty += (int)std::lround ((y0 - sy) * fy);
		tw = (int)std::lround ((x1 - x0) * fx);
		th = (int)std::lround ((y1 - y0) * fy);
		sx = x0, sy = y0, sw = x1 - x0, sh = y1 - y0;
	}
	if (!Begin (tgt)) return false;

	VkTexture *s = src;
	bool self = (src == tgt);
	if (self) {
		if (!CopyToScratch (src, sx, sy, sw, sh)) return false;
		s = scratch;
		sx = sy = 0;
	}
	DrawState2D st;
	st.tex = s;
	st.blend = Blend2D::Copy;
	st.fetch = (tw == sw && th == sh);
	if (useCkey) st.ckey = src->ckey;
	static const float white[4] = { 1, 1, 1, 1 };
	if (st.fetch)
		Quad (st, (float)tx, (float)ty, (float)(tx + tw), (float)(ty + th),
			(float)sx, (float)sy, (float)(sx + sw), (float)(sy + sh), white);
	else
		Quad (st, (float)tx, (float)ty, (float)(tx + tw), (float)(ty + th),
			(float)sx / s->width, (float)sy / s->height, (float)(sx + sw) / s->width, (float)(sy + sh) / s->height, white);
	if (self) Flush (); // the scratch surface is reused by the next blit
	return true;
}

bool Renderer2D::Fill (VkTexture *tgt, int x, int y, int w, int h, uint32_t rgb)
{
	if (!Begin (tgt)) return false;
	Flush ();
	VkExtent2D ext = TargetSize ();
	int x0 = std::max (x, 0), y0 = std::max (y, 0);
	int x1 = std::min (x + w, (int)ext.width), y1 = std::min (y + h, (int)ext.height);
	if (x1 <= x0 || y1 <= y0) return true;
	BindCurrent ();
	VkClearAttachment ca{};
	ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ca.colorAttachment = 0;
	float col[4];
	DeviceColorToLinear (rgb, 1.0f, col);
	memcpy (ca.clearValue.color.float32, col, sizeof(col));
	VkClearRect cr{};
	cr.rect = { { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
	cr.layerCount = 1;
	vkCmdClearAttachments (ctx->Cmd(), 1, &ca, 1, &cr);
	return true;
}

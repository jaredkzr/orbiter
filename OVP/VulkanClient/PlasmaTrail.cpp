// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "PlasmaTrail.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "plasma_trail.vert.h"
#include "plasma_trail.frag.h"

namespace {
	// a vertex of the strip: the point (view, camera-relative), its half
	// width [m]; its direction along the trail (view), the side (-1, 1); its
	// radiance (rgb), the distance along the trail (units of its width laid)
	struct Vertex { float pos[4], tan[4], col[4]; };

	// (the recombining air: from the colour it was laid with to violet,
	// dimming - as the particles' ramp, ParticleRamp, PC_PLASMA_WAKE)
	void Ramp (const float tint[3], float t, float out[3])
	{
		static const float V[3] = { 0.55f, 0.28f, 0.95f };
		t = std::clamp (t, 0.0f, 1.0f);
		float x = std::clamp ((t - 0.1f) / 0.8f, 0.0f, 1.0f);
		float s = x * x * (3.0f - 2.0f * x);
		float d = std::pow (1.0f - t, 1.5f);
		for (int c = 0; c < 3; c++) out[c] = (tint[c] + (V[c] - tint[c]) * s) * d;
	}
}

bool PlasmaTrails::Init (vkc::Context *c, VkDescriptorSetLayout frameLayout)
{
	ctx = c;
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1, plci.pSetLayouts = &frameLayout;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));

	VkVertexInputBindingDescription bind{ 0, sizeof (Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 } };
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 3, vi.pVertexAttributeDescriptions = attr;
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
	VkPipelineColorBlendAttachmentState cbm{}; // reactive mask: the strongest contribution
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
	VkShaderModule mods[2] = {
		ctx->CreateShaderModule (spv_plasma_trail_vert, spv_plasma_trail_vert_size),
		ctx->CreateShaderModule (spv_plasma_trail_frag, spv_plasma_trail_frag_size) };
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, mods[0], "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mods[1], "main" } };
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
	return true;
}

void PlasmaTrails::Shutdown ()
{
	if (!ctx) return;
	if (pipe) vkDestroyPipeline (ctx->device, pipe, nullptr);
	if (layout) vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	pipe = VK_NULL_HANDLE, layout = VK_NULL_HANDLE;
	trails.clear ();
}

uint32_t PlasmaTrails::Add (uint32_t id, const Lay &l)
{
	if (!id || !trails.count (id)) {
		if (!id) id = nextId++;
		if (!nextId) nextId = 1; // (wrapped: 0 means none)
	}
	Trail &t = trails[id];
	t.orphan = false;
	Point p{};
	p.pos = l.pos, p.vel = l.airVel, p.t0 = l.simt;
	p.w0 = l.width, p.growth = l.growth, p.life = l.life, p.alpha = l.alpha;
	for (int c = 0; c < 3; c++) p.col[c] = l.col[c];
	// (the points about as far apart as the trail is wide, a hundred or so
	// along its length: a frame's step at orbital speed is some 120 m, the
	// path nearly straight over it)
	double spacing = std::max (0.5 * l.width, l.speed * l.life / 96.0);
	// (not laid for a while - the heating below the afterglow's onset, or
	// the time went back: a new run, not joined to the last)
	bool fresh = t.pts.size () < 2 || l.simt - t.lastLay > 0.5 || l.simt < t.lastLay;
	if (l.simt < t.lastLay) t.pts.clear ();
	if (fresh) {
		p.along = t.pts.empty () ? 0.0 : t.pts.back ().along + 4.0 * spacing;
		p.brk = true;
		t.pts.push_back (p); // (where it starts)
		p.brk = false;
		t.pts.push_back (p); // (the body's)
		t.unit = std::max (l.width, 0.1f);
	} else {
		// the body's point moved on with it; fixed where it left the one
		// before by the spacing (a new one for the body's)
		const Point &q = t.pts[t.pts.size () - 2];
		VECTOR3 qp = q.pos + q.vel * (l.simt - q.t0);
		double d = length (l.pos - qp);
		p.along = q.along + d;
		p.brk = false;
		t.pts.back () = p;
		if (d >= spacing) t.pts.push_back (p);
	}
	t.fadeIn = l.fadeIn;
	t.lastLay = l.simt;
	return id;
}

void PlasmaTrails::Release (uint32_t id)
{
	auto it = trails.find (id);
	if (it != trails.end ()) it->second.orphan = true;
}

void PlasmaTrails::Draw (VkDescriptorSet frameSet, const VECTOR3 &camPos, const MATRIX3 &camRot)
{
	count = 0;
	if (!enabled || trails.empty ()) return;
	double simt = oapiGetSimTime ();
	// (faded points off the old end; trails faded and gone, or a body's not
	// laid for a minute, dropped)
	size_t nv = 0;
	for (auto it = trails.begin (); it != trails.end ();) {
		Trail &t = it->second;
		if (simt < t.lastLay - 1.0) t.pts.clear ();
		while (!t.pts.empty () && (simt - t.pts.front ().t0 > t.pts.front ().life || simt < t.pts.front ().t0 - 1.0)) t.pts.pop_front ();
		if (t.pts.empty () && (t.orphan || simt - t.lastLay > 60.0)) { it = trails.erase (it); continue; }
		for (size_t i = 1; i < t.pts.size (); i++) if (!t.pts[i].brk) nv += 6;
		++it;
	}
	if (!nv) return;
	vkc::Context::Transient tr = ctx->AllocTransient (nv * sizeof (Vertex));
	if (!tr.ptr) return;
	Vertex *out = (Vertex*)tr.ptr;
	size_t written = 0;
	std::vector<Vertex> pv; // (each point's: the side's sign set per vertex)
	std::vector<VECTOR3> vp;
	for (auto &kv : trails) {
		Trail &t = kv.second;
		size_t n = t.pts.size ();
		if (n < 2) continue;
		vp.resize (n), pv.resize (n);
		for (size_t i = 0; i < n; i++) {
			const Point &p = t.pts[i];
			vp[i] = tmul (camRot, p.pos + p.vel * (simt - p.t0) - camPos);
		}
		double head = t.pts.back ().along;
		for (size_t i = 0; i < n; i++) {
			const Point &p = t.pts[i];
			size_t a = (i > 0 && !p.brk) ? i - 1 : i;
			size_t b = (i + 1 < n && !t.pts[i + 1].brk) ? i + 1 : i;
			VECTOR3 tg = vp[b] - vp[a];
			double tl = length (tg);
			tg = tl > 1e-6 ? tg / tl : _V(0, 0, 1);
			float age = (float)(simt - p.t0);
			float w = p.w0 + p.growth * age;
			// (the gas spreading: its light over a wider strip, the brightness
			// at its centre falling - not all of it: thinner, it still reads)
			float k = p.alpha * std::sqrt (p.w0 / std::max (w, 1e-3f));
			// (fading in behind the body: its sheath and wake there)
			if (t.fadeIn > 0.0f) {
				float x = std::clamp ((float)(head - p.along) / t.fadeIn, 0.0f, 1.0f);
				k *= x * x * (3.0f - 2.0f * x);
			}
			float col[3];
			Ramp (p.col, age / p.life, col);
			Vertex &v = pv[i];
			v.pos[0] = (float)vp[i].x, v.pos[1] = (float)vp[i].y, v.pos[2] = (float)vp[i].z, v.pos[3] = w; // (the strip twice its width: room for the glow's soft edges)
			v.tan[0] = (float)tg.x, v.tan[1] = (float)tg.y, v.tan[2] = (float)tg.z, v.tan[3] = 0.0f;
			v.col[0] = col[0] * k, v.col[1] = col[1] * k, v.col[2] = col[2] * k, v.col[3] = (float)std::fmod (p.along / t.unit, 8192.0);
		}
		for (size_t i = 1; i < n; i++) {
			if (t.pts[i].brk) continue;
			Vertex a0 = pv[i - 1], a1 = pv[i - 1], b0 = pv[i], b1 = pv[i];
			a0.tan[3] = b0.tan[3] = -1.0f, a1.tan[3] = b1.tan[3] = 1.0f;
			// (the along coordinate wraps every 8192 widths: the segment
			// that crosses it continues from its start's)
			if (b0.col[3] < a0.col[3]) b0.col[3] += 8192.0f, b1.col[3] += 8192.0f;
			const Vertex q[6] = { a0, a1, b1, a0, b1, b0 };
			std::copy (q, q + 6, out + written);
			written += 6;
		}
		count += (uint32_t)n;
	}
	if (!written) return;
	VkCommandBuffer cmd = ctx->Cmd ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frameSet, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 1, &tr.buffer, &tr.offset);
	vkCmdDraw (cmd, (uint32_t)written, 1, 0, 0);
	ctx->GpuMark ("plasma trails");
}

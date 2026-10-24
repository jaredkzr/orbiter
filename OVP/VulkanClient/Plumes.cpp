// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Plumes.h"
#include "VkTexture.h"
#include "VesselAPI.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "plume.vert.h"
#include "plume.frag.h"

namespace {

	// emission colours are linear; brightness 1 is about a sunlit white surface
	const PlumeClass classes[(int)PlumeType::Count] = {
		//  name          pe [Pa]  core                 bright  diamonds             bright  afterburning        bright smoke  vac   turb  flow
		{ "ion",          1.0,   { 0.35f, 0.55f, 1.0f }, 0.7f, { 0, 0, 0 },          0.0f, { 0, 0, 0 },          0.0f, 0.0f,  1.0f, 0.2f,  4.0f },
		{ "plasma",       5e3,   { 0.55f, 0.62f, 1.0f }, 1.4f, { 0.85f, 0.8f, 1.0f }, 4.0f, { 0.8f, 0.55f, 1.0f }, 0.6f, 0.0f,  0.6f, 0.5f, 60.0f },
		{ "hydrolox",     2e4,   { 0.45f, 0.55f, 1.0f }, 0.35f, { 1.0f, 0.82f, 0.7f }, 5.0f, { 1.0f, 0.55f, 0.3f }, 0.3f, 0.0f,  0.4f, 0.6f, 50.0f },
		{ "methalox",     4e4,   { 0.40f, 0.50f, 1.0f }, 0.6f, { 0.9f, 0.75f, 1.0f }, 5.0f, { 1.0f, 0.5f, 0.2f },  0.8f, 0.0f,  0.5f, 0.7f, 45.0f },
		{ "kerolox",      7e4,   { 1.0f, 0.55f, 0.22f }, 2.0f, { 1.0f, 0.85f, 0.6f }, 5.0f, { 1.0f, 0.45f, 0.12f }, 4.0f, 0.02f, 0.35f, 1.0f, 40.0f },
		{ "hypergolic",   3e4,   { 1.0f, 0.55f, 0.45f }, 0.9f, { 1.0f, 0.75f, 0.6f }, 3.0f, { 1.0f, 0.5f, 0.3f },  1.2f, 0.01f, 0.5f, 0.7f, 45.0f },
		{ "solid",        7e4,   { 1.0f, 0.85f, 0.6f }, 3.5f, { 1.0f, 0.9f, 0.7f }, 4.0f, { 1.0f, 0.6f, 0.25f }, 6.0f, 0.25f, 0.6f, 1.2f, 35.0f },
		{ "rcs",          3e3,   { 0.9f, 0.85f, 0.8f },  0.9f, { 0, 0, 0 },          0.0f, { 1.0f, 0.6f, 0.4f },  0.3f, 0.0f,  1.0f, 0.5f, 30.0f },
		{ "texture",      0.0,   { 1, 1, 1 },            1.0f, { 0, 0, 0 },          0.0f, { 0, 0, 0 },          0.0f, 0.0f,  1.0f, 0.0f,  0.0f },
	};

	double Clamp01 (double x) { return std::clamp (x, 0.0, 1.0); }

	std::string Lower (std::string s)
	{
		for (auto &c : s) c = (char)tolower ((unsigned char)c);
		return s;
	}

	bool InGroup (VESSEL *v, THGROUP_TYPE g, THRUSTER_HANDLE th, int *idx = nullptr)
	{
		DWORD n = v->GetGroupThrusterCount (g);
		for (DWORD i = 0; i < n; i++)
			if (v->GetGroupThruster (g, i) == th) {
				if (idx) *idx = (int)i;
				return true;
			}
		return false;
	}

	// index of the thruster among the vessel's attitude thrusters (-1: none)
	int AttitudeIndex (VESSEL *v, THRUSTER_HANDLE th)
	{
		std::vector<THRUSTER_HANDLE> seen;
		for (int g = THGROUP_ATT_PITCHUP; g <= THGROUP_ATT_BACK; g++) {
			DWORD n = v->GetGroupThrusterCount ((THGROUP_TYPE)g);
			for (DWORD i = 0; i < n; i++) {
				THRUSTER_HANDLE h = v->GetGroupThruster ((THGROUP_TYPE)g, i);
				if (std::find (seen.begin(), seen.end(), h) != seen.end()) continue;
				if (h == th) return (int)seen.size();
				seen.push_back (h);
			}
		}
		return -1;
	}

} // namespace

// ======================================================================
// Engine classes and configuration

const PlumeClass &PlumeConfig::Class (PlumeType t)
{
	return classes[std::clamp ((int)t, 0, (int)PlumeType::Count - 1)];
}

PlumeType PlumeConfig::Infer (VESSEL *v, void *thp)
{
	THRUSTER_HANDLE th = (THRUSTER_HANDLE)thp;
	if (!th) return PlumeType::Texture; // (an exhaust without a thruster: keep the add-on's look)
	bool primary = InGroup (v, THGROUP_MAIN, th) || InGroup (v, THGROUP_RETRO, th) || InGroup (v, THGROUP_HOVER, th);
	if (!primary && AttitudeIndex (v, th) >= 0) return PlumeType::RCS;
	double isp = v->GetThrusterIsp0 (th), thrust = v->GetThrusterMax0 (th);
	if (isp >= 1e6) return PlumeType::Texture; // (a helper, e.g. separation bolts)
	if (isp >= 15e3) return thrust < 5e3 ? PlumeType::Ion : PlumeType::Plasma;
	if (isp >= 4e3) return PlumeType::Hydrolox; // (and nuclear thermal: hot hydrogen)
	if (isp >= 3.4e3) return PlumeType::Methalox;
	if (isp >= 2.9e3) return thrust >= 3e5 ? PlumeType::Kerolox : PlumeType::Hypergolic;
	return thrust >= 3e5 ? PlumeType::Solid : PlumeType::Hypergolic;
}

const std::vector<PlumeConfig::Rule> &PlumeConfig::Rules (const std::string &cls)
{
	auto it = rules.find (cls);
	if (it != rules.end()) return it->second;
	std::vector<Rule> &list = rules[cls];
	std::string path = "Config/VK/Plumes/" + cls + ".cfg";
	for (auto &c : path) if (c == '\\') c = '/';
	std::ifstream f (path);
	std::string line;
	while (std::getline (f, line)) {
		if (auto c = line.find (';'); c != std::string::npos) line.resize (c);
		std::istringstream ss (line);
		std::string tag;
		if (!(ss >> tag)) continue;
		tag = Lower (tag);
		Rule r;
		std::string tok;
		if (tag == "exhaust") r.group = -2;
		else if (tag == "thruster") {
			if (!(ss >> tok)) continue;
			tok = Lower (tok);
			if (tok == "main") r.group = THGROUP_MAIN;
			else if (tok == "retro") r.group = THGROUP_RETRO;
			else if (tok == "hover") r.group = THGROUP_HOVER;
			else if (tok == "attitude" || tok == "rcs") r.group = THGROUP_ATT_PITCHUP;
			else if (tok == "all") r.group = -1;
			else { oapiWriteLogV ("VulkanClient: %s: unknown thruster group %s", path.c_str(), tok.c_str()); continue; }
		} else continue;
		// indices, then key/value pairs
		while (ss >> tok) {
			if (isdigit ((unsigned char)tok[0])) { r.indices.push_back (atoi (tok.c_str())); continue; }
			std::string key = Lower (tok), val;
			if (!(ss >> val)) break;
			if (key == "type") {
				std::string t = Lower (val);
				for (int k = 0; k < (int)PlumeType::Count; k++)
					if (t == classes[k].name) r.o.type = k, r.hasType = true;
				if (!r.hasType) oapiWriteLogV ("VulkanClient: %s: unknown plume type %s", path.c_str(), val.c_str());
			}
			else if (key == "scale") r.o.scale = (float)atof (val.c_str()), r.hasScale = true;
			else if (key == "length") r.o.length = (float)atof (val.c_str()), r.hasLength = true;
			else if (key == "brightness") r.o.brightness = (float)atof (val.c_str()), r.hasBrightness = true;
			else oapiWriteLogV ("VulkanClient: %s: unknown plume setting %s", path.c_str(), tok.c_str());
		}
		list.push_back (r);
	}
	if (!list.empty()) oapiWriteLogV ("VulkanClient: plume configuration %s: %zu rules", path.c_str(), list.size());
	return list;
}

PlumeOverride PlumeConfig::Get (VESSEL *v, unsigned idx)
{
	PlumeOverride o;
	EXHAUSTSPEC es;
	if (!v->GetExhaustSpec (idx, &es)) return o;
	// (testing: ORBITER_VK_PLUME_TYPE forces the class of all engines but attitude thrusters)
	static int forced = -2;
	if (forced == -2) {
		forced = -1;
		if (const char *e = getenv ("ORBITER_VK_PLUME_TYPE"))
			for (int k = 0; k < (int)PlumeType::Count; k++)
				if (Lower (e) == classes[k].name) forced = k;
	}
	if (forced >= 0 && Infer (v, es.th) != PlumeType::RCS) o.type = forced;
	const char *cn = v->GetClassName ();
	const std::vector<Rule> &list = Rules (cn ? cn : "");
	for (const Rule &r : list) {
		bool match;
		if (r.group == -2) match = r.indices.empty() || std::find (r.indices.begin(), r.indices.end(), (int)idx) != r.indices.end();
		else if (r.group == -1) match = true;
		else {
			int i = -1;
			if (r.group == THGROUP_ATT_PITCHUP) i = es.th ? AttitudeIndex (v, es.th) : -1;
			else if (es.th) InGroup (v, (THGROUP_TYPE)r.group, es.th, &i);
			match = i >= 0 && (r.indices.empty() || std::find (r.indices.begin(), r.indices.end(), i) != r.indices.end());
		}
		if (!match) continue;
		if (r.hasType) o.type = r.o.type;
		if (r.hasScale) o.scale = r.o.scale;
		if (r.hasLength) o.length = r.o.length;
		if (r.hasBrightness) o.brightness = r.o.brightness;
	}
	return o;
}

// ======================================================================
// Renderer

struct PlumeRenderer::Push {
	float toLocal[16]; // view -> plume space [m]: z along the flow, origin at the nozzle exit
	float shape[4];    // exit radius [m], length, jet radius after the exit (expansion), spread (tan of the half angle)
	float shape2[4];   // shock cell spacing, shock diamond strength, box length, box radius
	float core[4];     // core emission (rgb), vacuum fraction
	float diamond[4];  // shock diamond emission (rgb), spread of the mixing layer
	float outer[4];    // afterburning emission (rgb), ion beam (1)
	float misc[4];     // time [s], turbulence drift, smoke extinction, turbulence
	float misc2[4];    // texture tint amount, seed, sunlight reaching the plume, -
};                     // (lengths in exit radii)

bool PlumeRenderer::Init (vkc::Context *c, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout texLayout,
	VkImageView shapeNoise, VkImageView detailNoise)
{
	ctx = c;
	shapeView = shapeNoise, detailView = detailNoise;
	if (const char *e = getenv ("ORBITER_VK_PLUMES")) enabled = atoi (e) != 0;

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &repeatSampler));
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &clampSampler));

	VkDescriptorSetLayoutBinding b[3]{};
	for (uint32_t i = 0; i < 3; i++)
		b[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 3;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));
	VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 1, pci.poolSizeCount = 1, pci.pPoolSizes = &ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool, dai.descriptorSetCount = 1, dai.pSetLayouts = &setLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &set));

	VkDescriptorSetLayout sets[3] = { frameLayout, setLayout, texLayout };
	VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 3, plci.pSetLayouts = sets;
	plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));

	VkShaderModule vs = ctx->CreateShaderModule (spv_plume_vert, spv_plume_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_plume_frag, spv_plume_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};
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
	ms.rasterizationSamples = ctx->sceneSamples;
	// no depth test: the march stops at the prepass depth
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	cba.blendEnable = VK_TRUE; // premultiplied: emission plus the scene times the transmittance
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	cba.colorBlendOp = VK_BLEND_OP_ADD;
	cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
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

void PlumeRenderer::Shutdown ()
{
	if (!ctx) return;
	vkDestroyPipeline (ctx->device, pipe, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	vkDestroySampler (ctx->device, clampSampler, nullptr);
	vkDestroySampler (ctx->device, repeatSampler, nullptr);
	ctx = nullptr;
}

void PlumeRenderer::SetPrepass (VkImageView prepass)
{
	if (prepass == prepassView) return;
	if (prepassView) vkDeviceWaitIdle (ctx->device); // (the set may be in use)
	prepassView = prepass;
	VkDescriptorImageInfo ii[3] = {
		{ clampSampler, prepass, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ repeatSampler, shapeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ repeatSampler, detailView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	VkWriteDescriptorSet wr[3]{};
	for (uint32_t i = 0; i < 3; i++) {
		wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[i].dstSet = set, wr[i].dstBinding = i, wr[i].descriptorCount = 1;
		wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[i].pImageInfo = &ii[i];
	}
	vkUpdateDescriptorSets (ctx->device, 3, wr, 0, nullptr);
}

void PlumeRenderer::Draw (const PlumeDraw &p, VkDescriptorSet frameSet, VkTexture *white, double simt)
{
	if (!prepassView || p.level <= 0.0 || p.exitRadius <= 0.0) return;
	const PlumeClass &c = PlumeConfig::Class (p.type);
	double re = p.exitRadius;
	bool ion = p.type == PlumeType::Ion;

	// expansion: the exit pressure over the ambient pressure
	double k = std::min (c.exitPressure / std::max (p.ambientPressure, 1e-3), 1e5);
	double vac = ion ? 1.0 : Clamp01 ((log10 (k) - 0.5) / 3.0);
	double air = Clamp01 (p.ambientPressure / 101325.0);
	double rj = ion ? 1.0 : std::clamp (pow (k, 0.25), 0.7, 3.0);
	double spread = ion ? 0.12 : 0.05 + 0.5 * vac;
	double mixSpread = 0.08 * (1.0 - vac);
	double len = p.length / re * (0.45 + 0.55 * p.level) * (1.0 - 0.3 * vac);
	double cell = 2.6 * rj;
	double lk = (log (k) - log (0.6)) / 1.3;
	double diamonds = exp (-lk * lk) * (1.0 - vac);
	// bounds of the visible volume (exit radii)
	double boxZ = 1.6 * len;
	double boxR = 1.8 * (std::max (rj, 1.0) + boxZ * spread + std::max (0.0, boxZ - 0.5 * len) * mixSpread) + 0.5;
	double bright = p.level * p.brightness * p.flicker * (1.0 + (c.vacuumBrightness - 1.0) * vac);

	// plume frame (view space)
	VECTOR3 ez = unit (p.dir);
	VECTOR3 ex = crossp (ez, fabs (ez.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0));
	ex = unit (ex);
	VECTOR3 ey = crossp (ez, ex);

	Push pc{};
	const VECTOR3 *e[3] = { &ex, &ey, &ez };
	for (int i = 0; i < 3; i++) {
		pc.toLocal[0 + i] = (float)e[i]->x;
		pc.toLocal[4 + i] = (float)e[i]->y;
		pc.toLocal[8 + i] = (float)e[i]->z;
		pc.toLocal[12 + i] = (float)-dotp (*e[i], p.pos);
	}
	pc.toLocal[15] = 1.0f;
	float sh[4] = { (float)re, (float)len, (float)rj, (float)spread };
	float sh2[4] = { (float)cell, (float)diamonds, (float)boxZ, (float)boxR };
	memcpy (pc.shape, sh, sizeof(sh));
	memcpy (pc.shape2, sh2, sizeof(sh2));
	for (int i = 0; i < 3; i++) {
		pc.core[i] = (float)(c.core[i] * c.coreBrightness * bright);
		pc.diamond[i] = (float)(c.diamond[i] * c.diamondBrightness * bright);
		pc.outer[i] = (float)(c.outer[i] * c.outerBrightness * bright * pow (air, 0.7));
	}
	pc.core[3] = (float)vac;
	pc.diamond[3] = (float)mixSpread;
	pc.outer[3] = ion ? 1.0f : 0.0f;
	pc.misc[0] = (float)fmod (simt, 1000.0);
	pc.misc[1] = c.flow;
	pc.misc[2] = (float)(c.smoke * air);
	pc.misc[3] = c.turbulence;
	pc.misc2[0] = p.tint ? 0.35f : 0.0f;
	pc.misc2[1] = (float)(p.seed % 1024);
	pc.misc2[2] = p.sunlight;

	// bounding box (view space)
	float v[36 * 3];
	static const int faces[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
	VECTOR3 corner[8];
	for (int i = 0; i < 8; i++) {
		double x = (i & 1) ? boxR : -boxR, y = (i & 2) ? boxR : -boxR, z = (i & 4) ? boxZ : -0.2;
		corner[i] = p.pos + (ex * x + ey * y + ez * z) * re;
	}
	int n = 0;
	for (auto &f : faces) {
		int idx[6] = { f[0], f[1], f[2], f[0], f[2], f[3] };
		for (int i : idx) {
			v[n++] = (float)corner[i].x, v[n++] = (float)corner[i].y, v[n++] = (float)corner[i].z;
		}
	}

	VkCommandBuffer cmd = ctx->Cmd();
	vkc::Context::Transient vb = ctx->AllocTransient (sizeof(v));
	memcpy (vb.ptr, v, sizeof(v));
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	VkTexture *tex = p.tint ? p.tint : white;
	VkDescriptorSet sets[3] = { frameSet, set, tex->set };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, sets, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb.buffer, &vb.offset);
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw (cmd, 36, 1, 0, 0);
}

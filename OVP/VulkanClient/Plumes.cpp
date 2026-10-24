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
	// Flame: colours and brightness after photographs of launches (Shuttle
	// SRBs and SSMEs, Saturn V, Falcon 9, Delta IV, Titan II, Proton).
	// Smoke: white aluminium oxide and condensed water of solid motors in a
	// dense, lasting column; tan soot of kerosene near the ground; steam of
	// hydrogen and methane engines in dense air; brownish nitrogen dioxide
	// of hypergolic propellants.
	const PlumeClass classes[(int)PlumeType::Count] = {
		//  name          pe [Pa]  core                 bright  diamonds             bright  afterburning        bright smoke  vac   turb  flow
		//                smoke colour              opacity life  air0   air1
		{ "ion",          1.0,   { 0.35f, 0.55f, 1.0f }, 0.7f, { 0, 0, 0 },          0.0f, { 0, 0, 0 },          0.0f, 0.0f,  1.0f, 0.2f,  4.0f,
		                  { 0, 0, 0 },              0.0f,  0.0f, 0.0f,  1.0f },
		{ "plasma",       5e3,   { 0.55f, 0.62f, 1.0f }, 1.4f, { 0.85f, 0.8f, 1.0f }, 4.0f, { 0.8f, 0.55f, 1.0f }, 0.6f, 0.0f,  0.6f, 0.5f, 60.0f,
		                  { 0, 0, 0 },              0.0f,  0.0f, 0.0f,  1.0f },
		{ "hydrolox",     2e4,   { 0.45f, 0.55f, 1.0f }, 0.35f, { 1.0f, 0.82f, 0.7f }, 5.0f, { 1.0f, 0.55f, 0.3f }, 0.3f, 0.0f,  0.4f, 0.6f, 50.0f,
		                  { 0.96f, 0.96f, 0.96f },  0.30f, 12.0f, 0.4f, 0.9f },
		{ "methalox",     4e4,   { 0.40f, 0.50f, 1.0f }, 0.6f, { 0.9f, 0.75f, 1.0f }, 5.0f, { 1.0f, 0.5f, 0.2f },  0.8f, 0.0f,  0.5f, 0.7f, 45.0f,
		                  { 0.95f, 0.95f, 0.95f },  0.22f, 10.0f, 0.5f, 0.95f },
		{ "kerolox",      7e4,   { 1.0f, 0.62f, 0.25f }, 2.5f, { 1.0f, 0.85f, 0.6f }, 5.0f, { 1.0f, 0.45f, 0.12f }, 4.0f, 0.02f, 0.35f, 1.0f, 40.0f,
		                  { 0.58f, 0.49f, 0.40f },  0.45f, 10.0f, 0.6f, 0.95f },
		{ "hypergolic",   3e4,   { 1.0f, 0.58f, 0.45f }, 0.9f, { 1.0f, 0.75f, 0.6f }, 3.0f, { 1.0f, 0.5f, 0.3f },  1.2f, 0.01f, 0.5f, 0.7f, 45.0f,
		                  { 0.74f, 0.50f, 0.32f },  0.40f, 12.0f, 0.3f, 0.9f },
		{ "solid",        7e4,   { 1.0f, 0.88f, 0.65f }, 6.0f, { 1.0f, 0.9f, 0.7f }, 4.0f, { 1.0f, 0.65f, 0.3f }, 6.0f, 0.25f, 0.6f, 1.2f, 35.0f,
		                  { 0.95f, 0.95f, 0.93f },  0.85f, 40.0f, 0.0f, 0.05f },
		{ "rcs",          3e3,   { 0.9f, 0.85f, 0.8f },  0.9f, { 0, 0, 0 },          0.0f, { 1.0f, 0.6f, 0.4f },  0.3f, 0.0f,  1.0f, 0.5f, 30.0f,
		                  { 0, 0, 0 },              0.0f,  0.0f, 0.0f,  1.0f },
		{ "texture",      0.0,   { 1, 1, 1 },            1.0f, { 0, 0, 0 },          0.0f, { 0, 0, 0 },          0.0f, 0.0f,  1.0f, 0.0f,  0.0f,
		                  { 0, 0, 0 },              0.0f,  0.0f, 0.0f,  1.0f },
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

struct PlumeRenderer::Params { // plume.glsl
	float toLocal[16]; // view -> plume space [m]: z along the flow, origin at the nozzle exit
	float shape[4];    // exit radius [m], length, jet radius after the exit (expansion), spread (tan of the half angle)
	float shape2[4];   // shock cell spacing, shock diamond strength, -, -
	float core[4];     // core emission (rgb), vacuum fraction
	float diamond[4];  // shock diamond emission (rgb), spread of the mixing layer
	float outer[4];    // afterburning emission (rgb), -
	float misc[4];     // time [s], turbulence drift, smoke extinction, turbulence
	float misc2[4];    // texture tint amount, seed, sunlight reaching the plume, -
	float bend[4];     // crossflow direction (xy, plume space), bending scale, -
	float boxMin[4];   // bounding box
	float boxMax[4];
	float hit[4];      // where the jet meets a surface, heat there (0: none)
	float hitN[4];     // surface normal (towards the jet), jet radius there
	float hit2[4];     // reach of the gas spreading along the surface, fraction of the jet blocked
};                     // (lengths in exit radii)

VkDeviceSize PlumeRenderer::BufferSize () const
{
	return 16 + sizeof(Params) * MAX_PLUMES * vkc::FRAMES_IN_FLIGHT;
}

bool PlumeRenderer::Init (vkc::Context *c, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout texLayout,
	VkDescriptorSetLayout atmLayout, VkImageView shapeNoise, VkImageView detailNoise)
{
	ctx = c;
	buf = ctx->CreateBuffer (BufferSize (), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	memset (buf.mapped, 0, BufferSize ());
	shapeView = shapeNoise, detailView = detailNoise;
	if (const char *e = getenv ("ORBITER_VK_PLUMES")) enabled = atoi (e) != 0;

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &repeatSampler));
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &clampSampler));

	// prepass, plume buffer, noise volumes
	VkDescriptorSetLayoutBinding b[4]{};
	for (uint32_t i = 0; i < 4; i++)
		b[i] = { i, i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 4;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 1, pci.poolSizeCount = 2, pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool, dai.descriptorSetCount = 1, dai.pSetLayouts = &setLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &set));

	VkDescriptorSetLayout sets[4] = { frameLayout, setLayout, texLayout, atmLayout };
	VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 4, plci.pSetLayouts = sets;
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
	rci.colorAttachmentCount = 2;
	rci.pColorAttachmentFormats = formats;
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
	ctx->DestroyBuffer (buf);
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
	VkDescriptorBufferInfo bi{ buf.buffer, 0, BufferSize () };
	VkWriteDescriptorSet wr[4]{};
	const uint32_t binding[3] = { 0, 2, 3 };
	for (uint32_t i = 0; i < 4; i++) {
		wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[i].dstSet = set, wr[i].descriptorCount = 1;
		if (i < 3) {
			wr[i].dstBinding = binding[i];
			wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			wr[i].pImageInfo = &ii[i];
		} else {
			wr[i].dstBinding = 1;
			wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			wr[i].pBufferInfo = &bi;
		}
	}
	vkUpdateDescriptorSets (ctx->device, 4, wr, 0, nullptr);
}

void PlumeRenderer::BeginFrame (double t)
{
	slot = ctx->FrameIndex ();
	simt = t;
	list.clear ();
	((uint32_t*)buf.mapped)[slot] = 0;
}

int PlumeRenderer::Add (const PlumeDraw &p, uint32_t owner, PlumeInfo *info)
{
	if (p.level <= 0.0 || p.exitRadius <= 0.0 || list.size() >= MAX_PLUMES) return -1;
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
	double bright = p.level * p.brightness * p.flicker * (1.0 + (c.vacuumBrightness - 1.0) * vac);

	// plume frame (view space)
	VECTOR3 ez = unit (p.dir);
	VECTOR3 ex = crossp (ez, fabs (ez.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0));
	ex = unit (ex);
	VECTOR3 ey = crossp (ez, ex);

	// the airflow: the jet's momentum flux (thrust over the exit area)
	// against the dynamic pressure of the air across it (bending) and
	// against it (a shorter, wider plume)
	double qJet = p.thrust > 0.0 ? p.thrust * p.level / (PI * re * re) : 10.8 * c.exitPressure;
	double windAlong = dotp (p.wind, ez);
	VECTOR3 cross = p.wind - ez * windAlong;
	double qCross = 0.5 * p.airDensity * dotp (cross, cross);
	double bendScale = 0.0, bx = 0.0, by = 0.0, stiff = 0.0;
	if (qCross > 1e-3 * qJet && qJet > 0.0) {
		bendScale = 2.0 * sqrt (qJet / qCross); // r d (exit radii)
		stiff = std::min (0.4 * bendScale, 6.0); // the hot core near the exit resists
		VECTOR3 cu = unit (cross);
		bx = dotp (cu, ex), by = dotp (cu, ey);
	}
	if (windAlong < 0.0 && qJet > 0.0) {
		double qHead = 0.5 * p.airDensity * windAlong * windAlong;
		len /= 1.0 + 4.0 * qHead / qJet;
		spread += 0.3 * qHead / (qHead + qJet);
	}

	// bounds of the visible volume (exit radii)
	double boxZ = 1.6 * len;
	double boxR = 1.8 * (std::max (rj, 1.0) + boxZ * spread + std::max (0.0, boxZ - 0.5 * len) * mixSpread) + 0.5;
	// (PlumeBend in plume.glsl; beyond bendLimit the bent jet has mixed out)
	auto bendAt = [&](double z) {
		if (bendScale <= 0.0) return 0.0;
		const double SC = 1.46;
		double zs = std::max (z - stiff, 0.0), s = zs / (1.6 * bendScale);
		return s < SC ? bendScale * s * s * s : bendScale * SC * SC * SC + 4.0 * (zs - 1.6 * bendScale * SC);
	};
	double bendLimit = 2.2 * (0.75 * bendScale + 2.5);
	if (bendScale > 0.0) {
		// the jet ends where it has mixed out
		double lo = 0.0, hi = boxZ;
		if (bendAt (hi) > bendLimit) {
			for (int k = 0; k < 30; k++) {
				double m = 0.5 * (lo + hi);
				(bendAt (m) > bendLimit ? hi : lo) = m;
			}
			boxZ = hi;
		}
	}
	double bendMax = std::min (bendAt (boxZ), bendLimit);
	double bmin[3] = { -boxR + std::min (0.0, bx * bendMax), -boxR + std::min (0.0, by * bendMax), -0.2 };
	double bmax[3] = { boxR + std::max (0.0, bx * bendMax), boxR + std::max (0.0, by * bendMax), boxZ };

	// a surface in the way: along the (bent) jet axis and along its edge
	// (six lines at three quarters of the jet radius), in segments; the
	// fraction of the lines blocked (the axis counting double) is how much
	// of the jet the surface takes
	auto radiusAt = [&](double z) { // jet radius (exit radii)
		double bz = bendAt (z), sArc = sqrt (z * z + bz * bz);
		return 1.0 + (rj - 1.0) * (1.0 - exp (-0.8 * sArc)) + sArc * spread + std::max (0.0, sArc - 0.5 * len) * mixSpread + 0.3 * bz;
	};
	auto toView = [&](const VECTOR3 &q) { return p.pos + (ex * q.x + ey * q.y + ez * q.z) * re; };
	bool hit = false;
	double coverage = 0.0;
	VECTOR3 hitP{}, hitN{};
	if (rayCast) {
		const int SEG = 8, EDGE = 6;
		const double z0 = 0.3; // (clear of the nozzle)
		double wSum = 0.0;
		VECTOR3 pSum{}, nSum{};
		for (int k = 0; k <= EDGE; k++) {
			double ang = 2.0 * PI * (k - 1) / EDGE, w = k ? 1.0 : 2.0;
			double ox = k ? cos (ang) : 0.0, oy = k ? sin (ang) : 0.0;
			auto pathPoint = [&](double z) {
				double b = bendAt (z), r = 0.75 * radiusAt (z);
				return _V(bx * b + ox * r, by * b + oy * r, z);
			};
			for (int i = 0; i < SEG; i++) {
				VECTOR3 a = pathPoint (z0 + (boxZ - z0) * i / SEG), b = pathPoint (z0 + (boxZ - z0) * (i + 1) / SEG);
				VECTOR3 va = toView (a), vb = toView (b);
				double segLen = length (vb - va), t;
				VECTOR3 n;
				if (segLen > 0.0 && rayCast (owner, va, (vb - va) / segLen, segLen, t, n)) {
					VECTOR3 q = va + (vb - va) * (t / segLen) - p.pos;
					VECTOR3 hn = _V(dotp (n, ex), dotp (n, ey), dotp (n, ez));
					if (dotp (hn, b - a) > 0.0) hn = -hn; // (facing the jet)
					pSum += _V(dotp (q, ex), dotp (q, ey), dotp (q, ez)) / re * w;
					nSum += hn * w;
					wSum += w;
					break;
				}
			}
		}
		if (wSum > 0.0) {
			hit = true;
			coverage = wSum / (EDGE + 2.0);
			hitP = pSum / wSum;
			hitN = length (nSum) > 1e-6 ? unit (nSum) : _V(0, 0, -1);
		}
	}
	double hitHeat = 0.0, hitR = 0.0, reach = 0.0;
	if (hit) {
		double z = std::max (hitP.z, 0.0), bz = bendAt (z);
		double sArc = sqrt (z * z + bz * bz), xi = sArc / len;
		hitR = radiusAt (z);
		double mixed = bendScale > 0.0 ? exp (-pow (bz / (0.75 * bendScale + 2.5), 2.0)) : 1.0;
		hitHeat = exp (-3.0 * xi * xi) * (xi < 1.6 ? 1.0 : 0.0) * mixed;
		// the spreading gas reaches further with more of the jet left
		reach = 2.5 * hitR + 0.4 * std::max (0.0, 1.6 * len - sArc);
		double ext = 1.3 * reach + hitR;
		for (int k = 0; k < 3; k++) {
			double c = (&hitP.x)[k];
			bmin[k] = std::min (bmin[k], c - ext), bmax[k] = std::max (bmax[k], c + ext);
		}
		bmax[2] = std::min (bmax[2], hitP.z + ext);
		hit = hitHeat > 0.0;
	}

	Params pc{};
	const VECTOR3 *e[3] = { &ex, &ey, &ez };
	for (int i = 0; i < 3; i++) {
		pc.toLocal[0 + i] = (float)e[i]->x;
		pc.toLocal[4 + i] = (float)e[i]->y;
		pc.toLocal[8 + i] = (float)e[i]->z;
		pc.toLocal[12 + i] = (float)-dotp (*e[i], p.pos);
	}
	pc.toLocal[15] = 1.0f;
	float sh[4] = { (float)re, (float)len, (float)rj, (float)spread };
	memcpy (pc.shape, sh, sizeof(sh));
	pc.shape2[0] = (float)cell, pc.shape2[1] = (float)diamonds;
	for (int i = 0; i < 3; i++) {
		pc.core[i] = (float)(c.core[i] * c.coreBrightness * bright);
		pc.diamond[i] = (float)(c.diamond[i] * c.diamondBrightness * bright);
		pc.outer[i] = (float)(c.outer[i] * c.outerBrightness * bright * pow (air, 0.7));
		pc.boxMin[i] = (float)bmin[i];
		pc.boxMax[i] = (float)bmax[i];
	}
	pc.core[3] = (float)vac;
	pc.diamond[3] = (float)mixSpread;
	pc.misc[0] = (float)fmod (simt, 1000.0);
	pc.misc[1] = c.flow;
	pc.misc[2] = (float)(c.smoke * air);
	pc.misc[3] = c.turbulence;
	pc.misc2[0] = p.tint ? 0.35f : 0.0f;
	pc.misc2[1] = (float)(p.seed % 1024);
	pc.misc2[2] = p.sunlight;
	pc.bend[0] = (float)bx, pc.bend[1] = (float)by, pc.bend[2] = (float)bendScale, pc.bend[3] = (float)stiff;
	if (hit) {
		pc.hit[0] = (float)hitP.x, pc.hit[1] = (float)hitP.y, pc.hit[2] = (float)hitP.z, pc.hit[3] = (float)hitHeat;
		pc.hitN[0] = (float)hitN.x, pc.hitN[1] = (float)hitN.y, pc.hitN[2] = (float)hitN.z, pc.hitN[3] = (float)hitR;
		pc.hit2[0] = (float)reach;
		pc.hit2[1] = (float)coverage;
	}

	if (info) {
		// the end of the flame (where the core has cooled), and the surface
		double zEnd = std::min (len, boxZ);
		if (hit && coverage > 0.5) zEnd = std::min (zEnd, std::max (hitP.z, 0.3)); // (the jet ends at the surface)
		auto viewOf = [&](double z) {
			double b = bendAt (z);
			return p.pos + (ex * (bx * b) + ey * (by * b) + ez * z) * re;
		};
		info->end = viewOf (zEnd);
		// light: from about a third down the flame; the mean emission of the
		// core (half of its peak, cooling along the flame), the afterburning
		// and the shock diamonds
		double zl = 0.3 * zEnd;
		if (hit && coverage > 0.5) zl = 0.6 * zEnd; // (towards the bright spot where it meets the surface)
		info->lightPos = viewOf (zl);
		// (radiance through the jet: about 1.1 times the emission per exit radius)
		for (int k = 0; k < 3; k++)
			info->lightCol[k] = 1.1f * (0.5f * pc.core[k] + 0.35f * pc.outer[k] + (float)(0.05 * diamonds) * pc.diamond[k]);
		info->lightArea = 2.0 * radiusAt (zl) * re * zEnd * re;
		info->endDir = unit (viewOf (zEnd + 0.5) - viewOf (zEnd - 0.5));
		info->endRadius = radiusAt (zEnd) * re;
		info->hit = hit;
		info->coverage = coverage;
		info->heat = hitHeat;
		if (hit) {
			info->hitPos = p.pos + (ex * hitP.x + ey * hitP.y + ez * hitP.z) * re;
			info->hitN = ex * hitN.x + ey * hitN.y + ez * hitN.z;
		}
	}
	uint32_t index = slot * MAX_PLUMES + (uint32_t)list.size();
	memcpy ((char*)buf.mapped + 16 + index * sizeof(Params), &pc, sizeof(pc));
	((uint32_t*)buf.mapped)[slot] = (uint32_t)list.size() + 1;

	// bounding box (view space)
	Entry en{ owner, index, p.tint, length (p.pos), {} };
	static const int faces[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
	VECTOR3 corner[8];
	for (int i = 0; i < 8; i++) {
		double x = bmin[0] + (bmax[0] - bmin[0]) * (i & 1), y = bmin[1] + (bmax[1] - bmin[1]) * ((i >> 1) & 1);
		double z = (i & 4) ? bmax[2] : bmin[2];
		corner[i] = p.pos + (ex * x + ey * y + ez * z) * re;
	}
	int n = 0;
	for (auto &f : faces) {
		int idx[6] = { f[0], f[1], f[2], f[0], f[2], f[3] };
		for (int i : idx)
			en.box[n++] = (float)corner[i].x, en.box[n++] = (float)corner[i].y, en.box[n++] = (float)corner[i].z;
	}
	list.push_back (en);
	return (int)index;
}

void PlumeRenderer::Draw (VkDescriptorSet frameSet, VkDescriptorSet atmSet, VkTexture *white)
{
	if (!prepassView || list.empty()) return;
	VkCommandBuffer cmd = ctx->Cmd();
	std::vector<const Entry*> order;
	for (const Entry &en : list) order.push_back (&en);
	std::sort (order.begin(), order.end(), [](const Entry *a, const Entry *b) { return a->dist > b->dist; });
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	VkDescriptorSet s[2] = { frameSet, set };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, s, 0, nullptr);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 3, 1, &atmSet, 0, nullptr);
	for (const Entry *en : order) {
		VkTexture *tex = en->tint ? en->tint : white;
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &tex->set, 0, nullptr);
		vkc::Context::Transient vb = ctx->AllocTransient (sizeof(en->box));
		memcpy (vb.ptr, en->box, sizeof(en->box));
		vkCmdBindVertexBuffers (cmd, 0, 1, &vb.buffer, &vb.offset);
		vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t), &en->index);
		vkCmdDraw (cmd, 36, 1, 0, 0);
	}
}

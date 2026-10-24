// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Atmosphere.h"
#include "Scene.h"
#include "VulkanClient.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "sky.vert.h"
#include "sky.frag.h"

namespace {

	// Must match the uniform block in atmosphere.glsl
	struct AtmUniforms {
		float viewToPlanet[16];
		float camPos[4];
		float sunDir[4];
		float betaR[4];
		float betaM[4];
		float radii[4];
		float camC[4];
		float misc[4];
	};

	constexpr uint32_t MAX_ATMOSPHERES = 64; // plus one "no atmosphere" set

	// Rayleigh scattering of Earth's air is close to proportional to the
	// square of its sky colour (AtmColor0), which is used to derive the
	// coefficients of other atmospheres: beta = K * colour^2 * density factor
	constexpr double RAYLEIGH_K = 3.9e-5;  // [1/m]
	constexpr double MIE_BETA = 1.0e-5;    // [1/m] for Earth-like density
	constexpr double EARTH_RHO0 = 1.225;   // [kg/m^3]

} // namespace

// ======================================================================
// AtmosphereRenderer

bool AtmosphereRenderer::Init (vkc::Context *c)
{
	ctx = c;
	VkDescriptorSetLayoutBinding b{};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 1;
	lci.pBindings = &b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));

	VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_ATMOSPHERES * vkc::FRAMES_IN_FLIGHT + 1 };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pci.maxSets = MAX_ATMOSPHERES * vkc::FRAMES_IN_FLIGHT + 1;
	pci.poolSizeCount = 1;
	pci.pPoolSizes = &ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));

	noneUbo = ctx->CreateBuffer (sizeof(AtmUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
	memset (noneUbo.mapped, 0, sizeof(AtmUniforms));
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &setLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &noneSet));
	VkDescriptorBufferInfo dbi{ noneUbo.buffer, 0, sizeof(AtmUniforms) };
	VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	wr.dstSet = noneSet;
	wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	wr.pBufferInfo = &dbi;
	vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);

	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &skyLayout));

	VkShaderModule vs = ctx->CreateShaderModule (spv_sky_vert, spv_sky_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_sky_frag, spv_sky_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
	cba.colorWriteMask = 0xF;
	cba.blendEnable = VK_TRUE; // the sky is added to the star background
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.colorBlendOp = VK_BLEND_OP_ADD;
	cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.alphaBlendOp = VK_BLEND_OP_ADD;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.colorAttachmentCount = 1;
	rci.pColorAttachmentFormats = &ctx->sceneFormat;
	rci.depthAttachmentFormat = ctx->depthFormat;
	VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gpi.pNext = &rci;
	gpi.stageCount = 2;
	gpi.pStages = stages;
	gpi.pVertexInputState = &vi;
	gpi.pInputAssemblyState = &ia;
	gpi.pViewportState = &vps;
	gpi.pRasterizationState = &rs;
	gpi.pMultisampleState = &ms;
	gpi.pDepthStencilState = &ds;
	gpi.pColorBlendState = &cb;
	gpi.pDynamicState = &dy;
	gpi.layout = skyLayout;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &skyPipeline));
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);
	return true;
}

void AtmosphereRenderer::Shutdown ()
{
	if (!ctx) return;
	vkDestroyPipeline (ctx->device, skyPipeline, nullptr);
	vkDestroyPipelineLayout (ctx->device, skyLayout, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	ctx->DestroyBuffer (noneUbo);
	ctx = nullptr;
}

// ======================================================================
// Atmosphere

Atmosphere::Atmosphere (OBJHANDLE h, Scene *s)
	: hPlanet(h), scene(s)
{
	ctx = scene->Client()->Ctx();
	radius = oapiGetSize (hPlanet);
	atmRadius = radius;

	const ATMCONST *ac = oapiPlanetHasAtmosphere (hPlanet) ? oapiGetPlanetAtmConstants (hPlanet) : nullptr;
	if (ac && ac->rho0 > 0.0 && ac->p0 > 0.0) {
		double g = GGRAV * oapiGetMass (hPlanet) / (radius * radius);
		double H = ac->p0 / (ac->rho0 * g); // scale height [m]
		double dens = std::clamp (ac->rho0 / EARTH_RHO0, 0.1, 10.0);
		const double col[3] = { ac->color0.x, ac->color0.y, ac->color0.z };
		// aerosol (Mie) scattering takes the colour of the configured haze
		double haze[3] = { 1.0, 1.0, 1.0 };
		if (const VECTOR3 *hc = (const VECTOR3*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_HAZECOLOUR)) {
			double m = std::max ({ hc->x, hc->y, hc->z });
			if (m > 0.0) haze[0] = hc->x / m, haze[1] = hc->y / m, haze[2] = hc->z / m;
		}
		for (int i = 0; i < 3; i++) {
			betaR[i] = (float)(RAYLEIGH_K * col[i] * col[i] * dens);
			betaM[i] = (float)(MIE_BETA * dens * haze[i]);
		}
		betaR[3] = (float)H;
		betaM[3] = (float)(H / 6.0);
		atmRadius = radius + std::min (ac->altlimit > 0.0 ? ac->altlimit : 1e30, 12.0 * H);
		present = true;
	}

	AtmosphereRenderer *ar = scene->Atm();
	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++) {
		ubo[i] = ctx->CreateBuffer (sizeof(AtmUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
		memset (ubo[i].mapped, 0, sizeof(AtmUniforms));
		VkDescriptorSetLayout l = ar->SetLayout();
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = ar->Pool();
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &l;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &sets[i]));
		VkDescriptorBufferInfo dbi{ ubo[i].buffer, 0, sizeof(AtmUniforms) };
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = sets[i];
		wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		wr.pBufferInfo = &dbi;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	}
}

Atmosphere::~Atmosphere ()
{
	vkc::Context *c = ctx;
	VkDescriptorPool pool = scene->Atm()->Pool();
	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++) {
		vkc::Buffer b = ubo[i];
		VkDescriptorSet s = sets[i];
		ctx->DeferDestroy ([c, b, s, pool]() mutable {
			c->DestroyBuffer (b);
			vkFreeDescriptorSets (c->device, pool, 1, &s);
		});
	}
}

VkDescriptorSet Atmosphere::Set () const
{
	return sets[ctx->FrameIndex()];
}

void Atmosphere::Update ()
{
	AtmUniforms u{};
	VECTOR3 ppos;
	MATRIX3 prot;
	oapiGetGlobalPos (hPlanet, &ppos);
	oapiGetRotationMatrix (hPlanet, &prot);

	// view space (camera frame) -> planet frame
	MATRIX3 v2p = mul (transp (prot), scene->CamRot());
	Mat4::FromRotTrans (v2p, _V(0, 0, 0)).ToFloat (u.viewToPlanet);
	VECTOR3 cam = tmul (prot, scene->CameraPos() - ppos);
	double camR = length (cam);
	u.camPos[0] = (float)cam.x, u.camPos[1] = (float)cam.y, u.camPos[2] = (float)cam.z;
	u.camPos[3] = (float)camR;
	u.camC[0] = (float)((camR - radius) * (camR + radius));
	u.camC[1] = (float)((camR - atmRadius) * (camR + atmRadius));

	VECTOR3 sun = _V(1, 0, 0);
	if (OBJHANDLE hSun = scene->Sun()) {
		VECTOR3 sp;
		oapiGetGlobalPos (hSun, &sp);
		sun = unit (tmul (prot, sp - ppos));
	}
	u.sunDir[0] = (float)sun.x, u.sunDir[1] = (float)sun.y, u.sunDir[2] = (float)sun.z;
	memcpy (u.betaR, betaR, sizeof(betaR));
	memcpy (u.betaM, betaM, sizeof(betaM));
	u.radii[0] = (float)radius;
	u.radii[1] = (float)atmRadius;
	u.radii[2] = mieG;
	u.radii[3] = sunIntensity;
	u.misc[0] = present ? 1.0f : 0.0f;
	u.misc[1] = (float)(1.0 / scene->ProjX());
	u.misc[2] = (float)(1.0 / scene->ProjY());
	memcpy (ubo[ctx->FrameIndex()].mapped, &u, sizeof(u));

	// the sky pass is needed when the atmosphere covers a noticeable part of the view
	visible = present && scene->PixelSize (atmRadius, length (scene->CameraPos() - ppos)) > 2.0;
}

void Atmosphere::RenderSky ()
{
	if (!visible) return;
	AtmosphereRenderer *ar = scene->Atm();
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ar->SkyPipeline());
	VkDescriptorSet s = Set ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ar->SkyLayout(), 0, 1, &s, 0, nullptr);
	vkCmdDraw (cmd, 3, 1, 0, 0);
}

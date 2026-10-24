// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Atmosphere.h"
#include "Scene.h"
#include "VulkanClient.h"
#include "VkTexture.h"
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
		float ground[4];
		float cloud[4];
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

bool AtmosphereRenderer::Init (vkc::Context *c, TextureManager *tm)
{
	ctx = c;
	texmgr = tm;
	VkDescriptorSetLayoutBinding b[5]{};
	b[0].binding = 0;
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	b[0].descriptorCount = 1;
	b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	for (int i = 1; i < 5; i++) {
		b[i].binding = i;
		b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 5;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));

	const uint32_t nSets = MAX_ATMOSPHERES * vkc::FRAMES_IN_FLIGHT + 1;
	VkDescriptorPoolSize ps[2] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nSets },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * nSets }
	};
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pci.maxSets = nSets;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
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
	WriteGround (noneSet, texmgr->White(), texmgr->White());
	WriteGround (noneSet, texmgr->White(), texmgr->White(), 3);

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
	ms.rasterizationSamples = ctx->sceneSamples;
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

void AtmosphereRenderer::WriteGround (VkDescriptorSet set, VkTexture *west, VkTexture *east, int binding) const
{
	// the textures' own descriptor sets carry the sampler and view
	VkCopyDescriptorSet cp[2]{};
	VkTexture *t[2] = { west ? west : texmgr->White(), east ? east : texmgr->White() };
	for (int i = 0; i < 2; i++) {
		cp[i].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
		cp[i].srcSet = t[i]->set;
		cp[i].srcBinding = 0;
		cp[i].dstSet = set;
		cp[i].dstBinding = binding + i;
		cp[i].descriptorCount = 1;
	}
	vkUpdateDescriptorSets (ctx->device, 0, nullptr, 2, cp);
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
		ar->WriteGround (sets[i], nullptr, nullptr);
		ar->WriteGround (sets[i], nullptr, nullptr, 3);
	}
}

void Atmosphere::SetClouds (VkTexture *west, VkTexture *east, double alt)
{
	if (!west || !east) return;
	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++)
		scene->Atm()->WriteGround (sets[i], west, east, 3);
	cloudParams[0] = (float)(radius + alt);
	cloudParams[2] = 0.3f;
	if (const float *d = (const float*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_CLOUDSHADOWCOL))
		cloudParams[2] = std::clamp (*d, 0.0f, 1.0f);
	cloudParams[3] = 1.0f;
}

void Atmosphere::SetGround (VkTexture *west, VkTexture *east, const float colour[3])
{
	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++)
		scene->Atm()->WriteGround (sets[i], west, east);
	for (int i = 0; i < 3; i++) groundColour[i] = colour[i];
	groundColour[3] = (west && east) ? 1.0f : 0.0f;
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
	memcpy (u.ground, groundColour, sizeof(groundColour));
	if (cloudParams[3] > 0.0f)
		if (const double *rot = (const double*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_CLOUDROTATION))
			cloudParams[1] = (float)*rot;
	memcpy (u.cloud, cloudParams, sizeof(cloudParams));
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

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
#include "atm_transmittance.comp.h"
#include "atm_multiscat.comp.h"

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
		float water[4];
		float ozone[4];
	};

	const VkFormat LUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
	const VkExtent2D TRANSMITTANCE_SIZE = { 256, 64 }, MULTISCAT_SIZE = { 32, 32 }; // as in atmlut.glsl

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
	VkDescriptorSetLayoutBinding b[7]{};
	b[0].binding = 0;
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	b[0].descriptorCount = 1;
	b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	for (int i = 1; i < 7; i++) { // ground, clouds, transmittance and multiple-scattering tables
		b[i].binding = i;
		b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 7;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));

	const uint32_t nSets = MAX_ATMOSPHERES * vkc::FRAMES_IN_FLIGHT + 1;
	VkDescriptorPoolSize ps[2] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nSets },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * nSets }
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
	WriteGround (noneSet, texmgr->White(), texmgr->White(), 5); // (tables unused without an atmosphere)

	// precomputed tables: sampler and compute pipelines
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &lutSampler));
	VkDescriptorSetLayoutBinding lb[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
	VkDescriptorSetLayoutCreateInfo llci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	llci.bindingCount = 2;
	llci.pBindings = lb;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &llci, nullptr, &lutSetLayout));
	VkPushConstantRange lpcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LutParams) };
	VkPipelineLayoutCreateInfo lplci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	lplci.setLayoutCount = 1;
	lplci.pSetLayouts = &lutSetLayout;
	lplci.pushConstantRangeCount = 1;
	lplci.pPushConstantRanges = &lpcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &lplci, nullptr, &lutLayout));
	auto compute = [this](const unsigned char *code, size_t size) {
		VkShaderModule cs = ctx->CreateShaderModule (code, size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = lutLayout;
		VkPipeline pipe = VK_NULL_HANDLE;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
		return pipe;
	};
	transmittancePipe = compute (spv_atm_transmittance_comp, spv_atm_transmittance_comp_size);
	multiscatPipe = compute (spv_atm_multiscat_comp, spv_atm_multiscat_comp_size);

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
	cba.blendEnable = VK_TRUE; // the sky is added to the star background, the ground covers it
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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

void AtmosphereRenderer::ComputeLuts (const LutParams &p, const vkc::Image &trans, const vkc::Image &ms)
{
	// one-off (at the creation of a planet's visual): a temporary descriptor
	// pool, executed and waited for
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
	VkDescriptorPool tmpPool;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &tmpPool));
	VkDescriptorSetLayout l[2] = { lutSetLayout, lutSetLayout };
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = tmpPool;
	dai.descriptorSetCount = 2;
	dai.pSetLayouts = l;
	VkDescriptorSet s[2];
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, s));
	VkDescriptorImageInfo tOut{ VK_NULL_HANDLE, trans.view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo tIn{ lutSampler, trans.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo mOut{ VK_NULL_HANDLE, ms.view, VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet wr[4]{};
	for (auto &w : wr) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w.descriptorCount = 1;
	wr[0].dstSet = s[0], wr[0].dstBinding = 0, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[0].pImageInfo = &tOut;
	wr[1].dstSet = s[0], wr[1].dstBinding = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[1].pImageInfo = &tIn;
	wr[2].dstSet = s[1], wr[2].dstBinding = 0, wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[2].pImageInfo = &mOut;
	wr[3].dstSet = s[1], wr[3].dstBinding = 1, wr[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[3].pImageInfo = &tIn;
	vkUpdateDescriptorSets (ctx->device, 4, wr, 0, nullptr);
	VkPipeline tp = transmittancePipe, mp = multiscatPipe;
	VkPipelineLayout pl = lutLayout;
	VkImage ti = trans.image, mi = ms.image;
	ctx->ImmediateSubmit ([&](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, ti, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
		vkc::ImageBarrier (cmd, mi, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
		vkCmdPushConstants (cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tp);
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s[0], 0, nullptr);
		vkCmdDispatch (cmd, (TRANSMITTANCE_SIZE.width + 7) / 8, (TRANSMITTANCE_SIZE.height + 7) / 8, 1);
		vkc::ImageBarrier (cmd, ti, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mp);
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s[1], 0, nullptr);
		vkCmdDispatch (cmd, MULTISCAT_SIZE.width, MULTISCAT_SIZE.height, 1);
		vkc::ImageBarrier (cmd, mi, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	vkDestroyDescriptorPool (ctx->device, tmpPool, nullptr);
}

void AtmosphereRenderer::Shutdown ()
{
	if (!ctx) return;
	vkDestroyPipeline (ctx->device, transmittancePipe, nullptr);
	vkDestroyPipeline (ctx->device, multiscatPipe, nullptr);
	vkDestroyPipelineLayout (ctx->device, lutLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, lutSetLayout, nullptr);
	vkDestroySampler (ctx->device, lutSampler, nullptr);
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
		// Earth's ozone layer absorbs in the orange (the blue of the sky at twilight)
		char name[64];
		oapiGetObjectName (hPlanet, name, 64);
		if (!strcmp (name, "Earth"))
			ozone[0] = 0.650e-6f, ozone[1] = 1.881e-6f, ozone[2] = 0.085e-6f;
	}

	if (const bool *r = (const bool*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_SURFACERIPPLE)) ripple = *r;

	AtmosphereRenderer *ar = scene->Atm();
	if (present) {
		auto image = [this](VkExtent2D e) {
			vkc::Image im;
			VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ici.imageType = VK_IMAGE_TYPE_2D;
			ici.format = LUT_FORMAT;
			ici.extent = { e.width, e.height, 1 };
			ici.mipLevels = 1;
			ici.arrayLayers = 1;
			ici.samples = VK_SAMPLE_COUNT_1_BIT;
			ici.tiling = VK_IMAGE_TILING_OPTIMAL;
			ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			VmaAllocationCreateInfo aci{};
			aci.usage = VMA_MEMORY_USAGE_AUTO;
			VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
			VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vci.image = im.image;
			vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vci.format = LUT_FORMAT;
			vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
			return im;
		};
		transmittanceLut = image (TRANSMITTANCE_SIZE);
		multiscatLut = image (MULTISCAT_SIZE);
		AtmosphereRenderer::LutParams lp{};
		memcpy (lp.betaR, betaR, sizeof(betaR));
		memcpy (lp.betaM, betaM, sizeof(betaM));
		memcpy (lp.ozone, ozone, sizeof(ozone));
		lp.radii[0] = (float)radius, lp.radii[1] = (float)atmRadius, lp.radii[2] = groundAlbedo, lp.radii[3] = mieG;
		ar->ComputeLuts (lp, transmittanceLut, multiscatLut);
	}
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
		ar->WriteGround (sets[i], nullptr, nullptr, 5);
		if (present) {
			VkDescriptorImageInfo ii[2] = {
				{ ar->LutSampler(), transmittanceLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
				{ ar->LutSampler(), multiscatLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
			VkWriteDescriptorSet lw[2]{};
			for (int k = 0; k < 2; k++) {
				lw[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				lw[k].dstSet = sets[i];
				lw[k].dstBinding = 5 + k;
				lw[k].descriptorCount = 1;
				lw[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				lw[k].pImageInfo = &ii[k];
			}
			vkUpdateDescriptorSets (ctx->device, 2, lw, 0, nullptr);
		}
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
	vkc::Image tl = transmittanceLut, ml = multiscatLut;
	ctx->DeferDestroy ([c, tl, ml]() mutable { c->DestroyImage (tl); c->DestroyImage (ml); });
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
	memcpy (u.ozone, ozone, sizeof(ozone));
	memcpy (u.ground, groundColour, sizeof(groundColour));
	if (cloudParams[3] > 0.0f)
		if (const double *rot = (const double*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_CLOUDROTATION))
			cloudParams[1] = (float)*rot;
	memcpy (u.cloud, cloudParams, sizeof(cloudParams));
	// origin of world-anchored surface patterns (waves, terrain detail): the
	// camera position snapped to 1 km, passed relative to the camera (small
	// values, no loss of precision)
	VECTOR3 origin = _V(std::floor (cam.x / 1000.0), std::floor (cam.y / 1000.0), std::floor (cam.z / 1000.0)) * 1000.0;
	VECTOR3 rel = cam - origin;
	u.water[0] = (float)rel.x, u.water[1] = (float)rel.y, u.water[2] = (float)rel.z;
	u.water[3] = ripple ? (float)fmod (oapiGetSimTime (), 3600.0) : -1.0f;
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

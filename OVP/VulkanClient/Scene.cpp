// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Scene.h"
#include "AO.h"
#include "VObjects.h"
#include "VkMesh.h"
#include "VkTexture.h"
#include "VulkanClient.h"
#include "StarSphere.h"
#include "Particles.h"
#include "PlanetSurface.h"
#include "Atmosphere.h"
#include "Shadows.h"
#include <algorithm>
#include <cstring>

#include "mesh.vert.h"
#include "mesh.frag.h"
#include "prepass.frag.h"
#include "stars.vert.h"
#include "stars.frag.h"
#include "particle.vert.h"
#include "particle.frag.h"

namespace {

	// Must match the Frame uniform block in frame.glsl
	struct FrameUniforms {
		float proj[16];
		float sunDir[4];
		float sunColor[4];
		float ambient[4];
		float shadowMat[16];   // view space -> shadow map
		float shadowParams[4]; // enabled, normal offset, depth bias, texel size
		float shadowMat2[16];  // far cascade
		float shadowParams2[4];
		float aoParams[4];     // 1/width, 1/height of the occlusion image, fraction applied to direct light
		float lightCount[4];
		struct {
			float pos[4];   // view space position, range
			float dir[4];   // view space direction, type (1 point, 2 spot)
			float col[4];   // colour times intensity
			float att[4];   // attenuation coefficients, cos(umbra/2)
			float spot[4];  // cos(penumbra/2)
		} lights[MAX_LIGHTS];
	};

	// shadows are cast within this range of the camera target, in units of
	// the camera distance (plus the target's size)
	constexpr double SHADOW_RANGE = 1.5;
	constexpr double SHADOW_RADIUS_MIN = 20.0, SHADOW_RADIUS_MAX = 5000.0; // [m]
	constexpr double SHADOW_FAR_RADIUS = 4000.0; // far cascade around the camera [m]

	struct StarPushConstants {
		float viewProj[16];
		float pointSize;
	};

	// Distance up to which vessel visuals are kept, in units of the vessel size
	constexpr double VESSEL_VIS_RANGE = 5e3;
	constexpr double VESSEL_VIS_MIN = 1e5; // [m]

	void MulMat4f (const float *a, const float *b, float *r) // column-major r = a*b
	{
		for (int c = 0; c < 4; c++)
			for (int rr = 0; rr < 4; rr++) {
				float s = 0.0f;
				for (int k = 0; k < 4; k++) s += a[k*4 + rr] * b[c*4 + k];
				r[c*4 + rr] = s;
			}
	}

} // namespace

Scene::Scene (VulkanClient *client) : gc(client)
{
}

Scene::~Scene ()
{
	Shutdown ();
}

VkCommandBuffer Scene::Cmd () const
{
	return ctx->Cmd();
}

bool Scene::Init ()
{
	ctx = gc->Ctx();
	if (ctx->deviceProps.limits.maxPushConstantsSize < sizeof(MeshPushConstants))
		oapiWriteLogError ("VulkanClient: device supports only %u bytes of push constants (%zu required)",
			ctx->deviceProps.limits.maxPushConstantsSize, sizeof(MeshPushConstants));

	shadows = std::make_unique<ShadowMap>();
	shadows->Init (ctx);
	shadowsFar = std::make_unique<ShadowMap>();
	shadowsFar->Init (ctx);

	// per-frame uniform buffer and the shadow map (set 0)
	VkDescriptorSetLayoutBinding b[4]{};
	b[0].binding = 0;
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	b[0].descriptorCount = 1;
	b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	b[1].binding = 1;
	b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b[1].descriptorCount = 1;
	b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	b[2] = b[1];
	b[2].binding = 2;
	b[3] = b[1];
	b[3].binding = 3; // ambient occlusion
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 4;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &frameLayout));

	VkDescriptorPoolSize ps[2] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, vkc::FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * vkc::FRAMES_IN_FLIGHT }
	};
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = vkc::FRAMES_IN_FLIGHT;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &framePool));

	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++) {
		frameUbo[i] = ctx->CreateBuffer (sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
		memset (frameUbo[i].mapped, 0, sizeof(FrameUniforms));
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = framePool;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &frameLayout;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &frameSet[i]));
		VkDescriptorBufferInfo dbi{ frameUbo[i].buffer, 0, sizeof(FrameUniforms) };
		VkDescriptorImageInfo dii{ shadows->Sampler(), shadows->View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo dii2{ shadowsFar->Sampler(), shadowsFar->View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet wr[3]{};
		wr[0].sType = wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[0].dstSet = wr[1].dstSet = frameSet[i];
		wr[0].dstBinding = 0;
		wr[0].descriptorCount = 1;
		wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		wr[0].pBufferInfo = &dbi;
		wr[1].dstBinding = 1;
		wr[1].descriptorCount = 1;
		wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[1].pImageInfo = &dii;
		wr[2] = wr[1];
		wr[2].dstBinding = 2;
		wr[2].pImageInfo = &dii2;
		vkUpdateDescriptorSets (ctx->device, 3, wr, 0, nullptr);
	}

	ao = std::make_unique<AmbientOcclusion>();
	ao->Init (ctx);
	WriteAODescriptors ();

	atm = std::make_unique<AtmosphereRenderer>();
	atm->Init (ctx, gc->TexMgr());
	CreatePipelines ();
	BuildSphere ();
	tiles = std::make_unique<TileRenderer>();
	tiles->Init (ctx, gc->TexMgr(), frameLayout, atm->SetLayout());
	exhaustTex = gc->TexMgr()->Load ("Exhaust.dds");
	{
		// glow sprite: bright core with a soft halo, zero at the edge
		const int N = 128;
		std::vector<uint8_t> px (N * N * 4);
		for (int y = 0; y < N; y++)
			for (int x = 0; x < N; x++) {
				double dx = (x + 0.5) / N * 2.0 - 1.0, dy = (y + 0.5) / N * 2.0 - 1.0;
				double r = std::sqrt (dx*dx + dy*dy);
				double a = r < 1.0 ? (0.75 * std::exp (-r*r*40.0) + 0.25 * std::pow (1.0 - r, 3.0)) : 0.0;
				uint8_t *p = &px[(y*N + x) * 4];
				p[0] = p[1] = p[2] = 255;
				p[3] = (uint8_t)std::lround (std::clamp (a, 0.0, 1.0) * 255.0);
			}
		glowTex = gc->TexMgr()->CreateRGBA (N, N, px.data());
	}

	stars = std::make_unique<StarSphere> (gc);
	stars->Init (ctx);

	// the sun lights the scene
	for (DWORD i = 0; i < oapiGetGbodyCount(); i++) {
		OBJHANDLE h = oapiGetGbodyByIndex (i);
		if (oapiGetObjectType (h) == OBJTP_STAR) { hSun = h; break; }
	}
	return true;
}

void Scene::Shutdown ()
{
	if (!ctx) return;
	vkDeviceWaitIdle (ctx->device);
	for (auto &it : visuals) gc->UnregisterVisObject (it.first);
	visuals.clear();
	ctx->Drain (); // releases deferred by the visuals use the shared renderers' pools
	if (tiles) tiles->Shutdown ();
	tiles.reset();
	if (atm) atm->Shutdown ();
	atm.reset();
	if (shadows) shadows->Shutdown ();
	shadows.reset();
	if (shadowsFar) shadowsFar->Shutdown ();
	shadowsFar.reset();
	if (stars) stars->Shutdown ();
	stars.reset();
	if (ao) ao->Shutdown ();
	ao.reset();
	ctx->DestroyBuffer (sphereVtx);
	ctx->DestroyBuffer (sphereIdx);
	for (auto &u : frameUbo) ctx->DestroyBuffer (u);
	VkPipeline pipes[] = { meshOpaque, meshBlend, meshAdditive, meshPrepass, starPipeline, particleAdditive, particleBlend };
	for (auto p : pipes) if (p) vkDestroyPipeline (ctx->device, p, nullptr);
	vkDestroyPipelineLayout (ctx->device, meshLayout, nullptr);
	vkDestroyPipelineLayout (ctx->device, particleLayout, nullptr);
	gc->TexMgr()->Release (exhaustTex);
	gc->TexMgr()->Release (glowTex);
	vkDestroyPipelineLayout (ctx->device, starLayout, nullptr);
	vkDestroyDescriptorPool (ctx->device, framePool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, frameLayout, nullptr);
	ctx = nullptr;
}

// ======================================================================
// Pipelines

VkPipeline Scene::CreateMeshPipeline (bool blend, bool additive, bool depthWrite, bool prepass)
{
	VkShaderModule vs = ctx->CreateShaderModule (spv_mesh_vert, spv_mesh_vert_size);
	VkShaderModule fs = prepass ? ctx->CreateShaderModule (spv_prepass_frag, spv_prepass_frag_size)
	                            : ctx->CreateShaderModule (spv_mesh_frag, spv_mesh_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};

	VkVertexInputBindingDescription bind{ 0, sizeof(NTVERTEX), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(NTVERTEX, x) },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(NTVERTEX, nx) },
		{ 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NTVERTEX, tu) }
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
	rs.cullMode = VK_CULL_MODE_BACK_BIT;
	// Orbiter meshes use clockwise front faces in a left-handed frame; with the
	// y flip of the projection they remain clockwise in framebuffer coordinates
	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = prepass ? VK_SAMPLE_COUNT_1_BIT : ctx->sceneSamples;

	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
	ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed z

	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	if (blend) {
		cba.blendEnable = VK_TRUE;
		cba.srcColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
		cba.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		cba.colorBlendOp = VK_BLEND_OP_ADD;
		cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		cba.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;

	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;

	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.colorAttachmentCount = 1;
	const VkFormat prepassFmt = AmbientOcclusion::PREPASS_FORMAT;
	rci.pColorAttachmentFormats = prepass ? &prepassFmt : &ctx->sceneFormat;
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
	gpi.layout = meshLayout;
	VkPipeline pipe = VK_NULL_HANDLE;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);
	return pipe;
}

void Scene::CreatePipelines ()
{
	VkDescriptorSetLayout sets[4] = { frameLayout, gc->TexMgr()->Layout(), atm->SetLayout(), gc->TexMgr()->MapLayout() };
	VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 4;
	plci.pSetLayouts = sets;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &meshLayout));

	meshOpaque = CreateMeshPipeline (false, false, true);
	meshBlend = CreateMeshPipeline (true, false, false);
	meshAdditive = CreateMeshPipeline (true, true, false);
	meshPrepass = CreateMeshPipeline (false, false, true, true);

	// stars: points at infinity, drawn first without depth test
	VkPushConstantRange spcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(StarPushConstants) };
	VkPipelineLayoutCreateInfo slci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	slci.pushConstantRangeCount = 1;
	slci.pPushConstantRanges = &spcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &slci, nullptr, &starLayout));

	VkShaderModule vs = ctx->CreateShaderModule (spv_stars_vert, spv_stars_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_stars_frag, spv_stars_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};
	VkVertexInputBindingDescription bind{ 0, 6 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float) }
	};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexAttributeDescriptions = attr;
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
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
	cba.blendEnable = VK_TRUE; // additive, so overlapping stars accumulate
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.colorBlendOp = VK_BLEND_OP_ADD;
	cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
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
	gpi.layout = starLayout;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &starPipeline));
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);

	// billboards: view-space vertices, one texture
	VkDescriptorSetLayout texLayout = gc->TexMgr()->Layout();
	VkPushConstantRange ppcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(proj) };
	VkPipelineLayoutCreateInfo plci2{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci2.setLayoutCount = 1;
	plci2.pSetLayouts = &texLayout;
	plci2.pushConstantRangeCount = 1;
	plci2.pPushConstantRanges = &ppcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci2, nullptr, &particleLayout));
	particleAdditive = CreateParticlePipeline (true);
	particleBlend = CreateParticlePipeline (false);
}

VkPipeline Scene::CreateParticlePipeline (bool additive)
{
	VkShaderModule vs = ctx->CreateShaderModule (spv_particle_vert, spv_particle_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_particle_frag, spv_particle_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};
	VkVertexInputBindingDescription bind{ 0, sizeof(PVtx), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PVtx, x) },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PVtx, u) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PVtx, r) }
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
	ms.rasterizationSamples = ctx->sceneSamples;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_FALSE;
	ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed z
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	cba.blendEnable = VK_TRUE;
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	cba.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
	gpi.layout = particleLayout;
	VkPipeline pipe = VK_NULL_HANDLE;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);
	return pipe;
}

void Scene::DrawBillboards (VkTexture *tex, bool additive, const PVtx *v, size_t n)
{
	if (!n || !tex) return;
	VkCommandBuffer cmd = ctx->Cmd();
	vkc::Context::Transient vb = ctx->AllocTransient (n * sizeof(PVtx));
	memcpy (vb.ptr, v, n * sizeof(PVtx));
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, additive ? particleAdditive : particleBlend);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleLayout, 0, 1, &tex->set, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb.buffer, &vb.offset);
	vkCmdPushConstants (cmd, particleLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(proj), proj);
	vkCmdDraw (cmd, (uint32_t)n, 1, 0, 0);
}

void Scene::BuildSphere ()
{
	// unit sphere, y axis = rotation axis, u = longitude, v = latitude
	const int nLat = 128, nLng = 256;
	std::vector<NTVERTEX> vtx;
	for (int i = 0; i <= nLat; i++) {
		double lat = PI05 - PI * i / nLat;
		for (int j = 0; j <= nLng; j++) {
			double lng = PI2 * j / nLng;
			NTVERTEX v;
			v.nx = v.x = (float)(cos(lat) * cos(lng));
			v.ny = v.y = (float)sin(lat);
			v.nz = v.z = (float)(cos(lat) * sin(lng));
			v.tu = (float)j / nLng;
			v.tv = (float)i / nLat;
			vtx.push_back (v);
		}
	}
	std::vector<uint32_t> idx32;
	for (int i = 0; i < nLat; i++)
		for (int j = 0; j < nLng; j++) {
			uint32_t a = i * (nLng+1) + j, b = a + 1, c = a + (nLng+1), d = c + 1;
			// clockwise when seen from outside (left-handed frame)
			idx32.insert (idx32.end(), { a, b, c, b, d, c });
		}
	sphereVtx = ctx->CreateDeviceBuffer (vtx.data(), vtx.size() * sizeof(NTVERTEX), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	sphereIdx = ctx->CreateDeviceBuffer (idx32.data(), idx32.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	sphereNIdx = (uint32_t)idx32.size();
}

// ======================================================================
// Visuals

VObject *Scene::GetVisual (OBJHANDLE hObj)
{
	auto it = visuals.find (hObj);
	return it != visuals.end() ? it->second.get() : nullptr;
}

void Scene::DeleteVisual (OBJHANDLE hObj)
{
	auto it = visuals.find (hObj);
	if (it == visuals.end()) return;
	gc->UnregisterVisObject (hObj);
	visuals.erase (it);
}

void Scene::Update ()
{
	UpdateCamera ();

	DWORD n = oapiGetObjectCount();
	for (DWORD i = 0; i < n; i++) {
		OBJHANDLE h = oapiGetObjectByIndex (i);
		int type = oapiGetObjectType (h);
		bool want;
		if (type == OBJTP_PLANET || type == OBJTP_STAR) {
			want = true;
		} else if (type == OBJTP_VESSEL) {
			VECTOR3 p;
			oapiGetGlobalPos (h, &p);
			double range = std::max (VESSEL_VIS_MIN, oapiGetSize (h) * VESSEL_VIS_RANGE);
			double d = length (p - camPos);
			bool have = visuals.count (h) != 0;
			want = have ? d < range * 1.2 : d < range; // hysteresis
		} else {
			want = false;
		}
		bool have = visuals.count (h) != 0;
		if (want && !have) {
			std::unique_ptr<VObject> vo;
			if (type == OBJTP_VESSEL) vo = std::make_unique<VVessel> (h, this);
			else vo = std::make_unique<VPlanet> (h, this, type == OBJTP_STAR);
			VObject *raw = vo.get();
			visuals[h] = std::move (vo);
			gc->RegisterVisObject (h, (VISHANDLE)raw);
		} else if (!want && have) {
			DeleteVisual (h);
		}
	}
	for (auto &it : visuals) it.second->Update();
}

// ======================================================================
// Camera

void Scene::UpdateCamera ()
{
	oapiCameraGlobalPos (&camPos);
	oapiCameraRotationMatrix (&camRot);
	// view = transpose(camRot): global direction -> camera frame
	MATRIX3 vt = {
		camRot.m11, camRot.m21, camRot.m31,
		camRot.m12, camRot.m22, camRot.m32,
		camRot.m13, camRot.m23, camRot.m33
	};
	view = Mat4::FromRotTrans (vt, _V(0, 0, 0));

	VkExtent2D ext = ctx->Extent();
	viewW = std::max (1u, ext.width);
	viewH = std::max (1u, ext.height);
	tanAperture = tan (oapiCameraAperture());
	double fy = 1.0 / tanAperture;
	double fx = fy * viewH / viewW;

	// view (x right, y up, z forward) -> Vulkan clip (y down, reversed z, infinite far plane)
	memset (proj, 0, sizeof(proj));
	proj[0] = (float)fx;
	proj[5] = (float)-fy;
	proj[11] = 1.0f;               // w = z
	proj[14] = (float)nearPlane;   // z = near  ->  depth = near / z
}

void Scene::RenderShadows ()
{
	shadowsValid = shadowsFarValid = false;
	if (!hSun) return;
	OBJHANDLE hTgt = oapiCameraTarget ();
	VECTOR3 c = camPos;
	double size = 0.0;
	if (hTgt) {
		oapiGetGlobalPos (hTgt, &c);
		size = oapiGetSize (hTgt);
	}
	double r = std::clamp (SHADOW_RANGE * length (camPos - c) + 2.0 * size, SHADOW_RADIUS_MIN, SHADOW_RADIUS_MAX);
	if (Sunlight (c) <= 0.0) return; // the region is in darkness anyway
	VECTOR3 sp;
	oapiGetGlobalPos (hSun, &sp);
	shadows->Begin (c, r, unit (sp - c));
	for (auto &it : visuals) it.second->RenderShadow (*shadows);
	shadows->End ();
	shadowsValid = true;

	// far cascade around the camera, for structures beyond the near map
	if (r < 0.5 * SHADOW_FAR_RADIUS) {
		shadowsFar->Begin (camPos, SHADOW_FAR_RADIUS, unit (sp - camPos));
		for (auto &it : visuals) it.second->RenderShadow (*shadowsFar);
		shadowsFar->End ();
		shadowsFarValid = true;
	}
}

double Scene::Sunlight (const VECTOR3 &gpos)
{
	if (!hSun) return 1.0;
	VECTOR3 sp;
	oapiGetGlobalPos (hSun, &sp);
	VECTOR3 s = sp - gpos;
	double ds = length (s);
	double as = asin (std::min (1.0, oapiGetSize (hSun) / ds)); // angular radius of the sun
	double f = 1.0;
	for (DWORD i = 0; i < oapiGetGbodyCount(); i++) {
		OBJHANDLE h = oapiGetGbodyByIndex (i);
		if (h == hSun) continue;
		VECTOR3 pp;
		oapiGetGlobalPos (h, &pp);
		VECTOR3 p = pp - gpos;
		double dp = length (p);
		if (dp >= ds) continue; // behind the sun
		double ap = asin (std::min (1.0, oapiGetSize (h) / dp)); // angular radius of the body
		double theta = acos (std::clamp (dotp (s, p) / (ds * dp), -1.0, 1.0));
		// linear transition across the penumbra
		double vis = std::clamp ((theta - (ap - as)) / (2.0 * as), 0.0, 1.0);
		f = std::min (f, vis);
	}
	objLight = (float)f;
	return f;
}

Mat4 Scene::ModelView (const VECTOR3 &gpos, const MATRIX3 &grot) const
{
	return view * Mat4::FromRotTrans (grot, gpos - camPos);
}

double Scene::PixelSize (double size, double dist) const
{
	if (dist <= size) return 1e10;
	return size / (dist * tanAperture) * viewH;
}

// ======================================================================
// Rendering

void Scene::WriteAODescriptors ()
{
	VkDescriptorImageInfo dii{ ao->Sampler(), ao->View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++) {
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = frameSet[i];
		wr.dstBinding = 3;
		wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr.pImageInfo = &dii;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	}
}

void Scene::BeginFrame ()
{
	frameNo++;
	uint32_t fi = ctx->FrameIndex();
	if (ao->EnsureSize ()) WriteAODescriptors (); // the GPU is idle then

	// frame uniforms
	FrameUniforms fu{};
	fu.aoParams[0] = 1.0f / ao->Extent().width;
	fu.aoParams[1] = 1.0f / ao->Extent().height;
	fu.aoParams[2] = ao->directFrac;
	memcpy (fu.proj, proj, sizeof(proj));
	VECTOR3 sunDir = _V(1, 0, 0);
	if (hSun) {
		VECTOR3 sp;
		oapiGetGlobalPos (hSun, &sp);
		sunDir = sp - camPos;
		double len = length (sunDir);
		if (len > 0) sunDir /= len;
	}
	VECTOR3 sv = view.TransformPoint (sunDir);
	fu.sunDir[0] = (float)sv.x, fu.sunDir[1] = (float)sv.y, fu.sunDir[2] = (float)sv.z;
	fu.sunColor[0] = fu.sunColor[1] = fu.sunColor[2] = 1.0f;
	// ambient light: the user's ambient level, plus skylight in daylight
	// inside an atmosphere (tinted with the sky colour)
	const DWORD *amb = (const DWORD*)gc->GetConfigParam (CFGPRM_AMBIENTLEVEL);
	float a0 = std::max (0.01f, amb ? (float)*amb / 255.0f * 0.5f : 0.03f);
	fu.ambient[0] = fu.ambient[1] = fu.ambient[2] = a0;
	if (OBJHANDLE hp = oapiCameraProxyGbody ()) {
		const ATMCONST *ac = oapiPlanetHasAtmosphere (hp) ? oapiGetPlanetAtmConstants (hp) : nullptr;
		if (ac && ac->rho0 > 0.0) {
			VECTOR3 pp;
			oapiGetGlobalPos (hp, &pp);
			ATMPARAM prm;
			oapiGetPlanetAtmParams (hp, length (camPos - pp), &prm);
			double f = 0.35 * std::sqrt (std::clamp (prm.rho / ac->rho0, 0.0, 1.0)) * Sunlight (camPos);
			fu.ambient[0] += (float)(f * ac->color0.x);
			fu.ambient[1] += (float)(f * ac->color0.y);
			fu.ambient[2] += (float)(f * ac->color0.z);
		}
	}
	auto shadowUniforms = [this](const ShadowMap &sm, float *mat, float *prm) {
		sm.ReceiverMatrix (camPos, camRot).ToFloat (mat);
		double texel = sm.TexelSize ();
		prm[0] = 1.0f;
		prm[1] = (float)(1.5 * texel);                       // normal offset [m]
		prm[2] = (float)(texel / (6.0 * sm.Radius()));       // depth bias (one texel in depth units)
		prm[3] = 1.0f / ShadowMap::SIZE;
	};
	if (shadowsValid) shadowUniforms (*shadows, fu.shadowMat, fu.shadowParams);
	if (shadowsFarValid) shadowUniforms (*shadowsFar, fu.shadowMat2, fu.shadowParams2);
	// local light sources, nearest to the camera first
	std::vector<LightSource> ls;
	for (auto &it : visuals) it.second->CollectLights (ls);
	std::sort (ls.begin(), ls.end(), [this](const LightSource &a, const LightSource &b) {
		return length (a.pos - camPos) - a.range < length (b.pos - camPos) - b.range;
	});
	int nl = (int)std::min (ls.size(), (size_t)MAX_LIGHTS);
	fu.lightCount[0] = (float)nl;
	for (int i = 0; i < nl; i++) {
		const LightSource &l = ls[i];
		VECTOR3 p = view.TransformPoint (l.pos - camPos), d = view.TransformPoint (l.dir);
		auto &u = fu.lights[i];
		u.pos[0] = (float)p.x, u.pos[1] = (float)p.y, u.pos[2] = (float)p.z, u.pos[3] = (float)l.range;
		u.dir[0] = (float)d.x, u.dir[1] = (float)d.y, u.dir[2] = (float)d.z, u.dir[3] = l.spot ? 2.0f : 1.0f;
		for (int k = 0; k < 3; k++) u.col[k] = l.col[k], u.att[k] = (float)l.att[k];
		u.att[3] = (float)cos (0.5 * l.umbra);
		u.spot[0] = (float)cos (0.5 * l.penumbra);
	}
	memcpy (frameUbo[fi].mapped, &fu, sizeof(fu));

	// atmosphere of the planet the camera is at, for vessels and bases
	camAtmSet = atm->NoAtmosphereSet ();
	if (OBJHANDLE hp = oapiCameraProxyGbody ()) {
		auto it = visuals.find (hp);
		if (it != visuals.end())
			if (Atmosphere *a = it->second->GetAtmosphere ())
				camAtmSet = a->Set (); // also without atmosphere: the planet is reflected
	}

}

void Scene::RenderPrepass ()
{
	if (!ao->enabled) return;
	// planet surfaces select their tiles and upload data first (transfers
	// end rendering scopes, and the prepass target is cleared when bound)
	for (auto &it : visuals) it.second->PrepareFrame ();
	ao->BeginPrepass ();
	for (auto &it : visuals) it.second->Render (RenderPass::Depth);
	ao->Compute (proj[0], -proj[5]);
}

void Scene::Render ()
{
	VkCommandBuffer cmd = ctx->Cmd();
	uint32_t fi = ctx->FrameIndex();

	// background stars
	float viewf[16], vp[16];
	view.ToFloat (viewf);
	MulMat4f (proj, viewf, vp);
	stars->Render (cmd, starPipeline, starLayout, vp);

	std::vector<VObject*> list;
	for (auto &it : visuals) list.push_back (it.second.get());

	// skies of planetary atmospheres, over the stars
	for (auto *vo : list) vo->RenderSky ();

	// scene geometry
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &frameSet[fi], 0, nullptr);
	for (auto *vo : list) vo->Render (RenderPass::Opaque);

	std::sort (list.begin(), list.end(), [](VObject *a, VObject *b) { return a->CamDist() > b->CamDist(); });
	for (auto *vo : list) vo->Render (RenderPass::Transparent);

	for (auto *ps : gc->ParticleStreams()) ps->Render (this);
}

void Scene::DrawMesh (const VkMesh *mesh, const Mat4 &modelView, RenderPass pass,
	const std::vector<Mat4> *groupTransforms, const std::vector<uint8_t> *groupVisible,
	const std::vector<GroupTexOverride> *texOverride)
{
	if (!mesh->VertexBuffer() || !mesh->IndexBuffer()) return;
	VkCommandBuffer cmd = ctx->Cmd();
	VkDeviceSize ofs = 0;
	VkBuffer vb = mesh->VertexBuffer();
	// billboards drawn in between use a different layout for set 0
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &frameSet[ctx->FrameIndex()], 0, nullptr);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 2, 1, &camAtmSet, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
	vkCmdBindIndexBuffer (cmd, mesh->IndexBuffer(), 0, VK_INDEX_TYPE_UINT16);

	VkPipeline bound = VK_NULL_HANDLE;
	VkDescriptorSet boundTex = VK_NULL_HANDLE, boundMaps = VK_NULL_HANDLE;
	for (uint32_t g = 0; g < mesh->nGroup(); g++) {
		const VkMesh::Group &grp = mesh->GetGroup (g);
		if (!grp.nIndex || (grp.flags & VkMesh::GRP_HIDDEN) || (grp.usrFlag & 0x2)) continue;
		if (groupVisible && g < groupVisible->size() && !(*groupVisible)[g]) continue;
		const GroupTexOverride *ovr = nullptr;
		if (texOverride)
			for (auto &o : *texOverride)
				if (o.group == g) { ovr = &o; break; }
		bool transparent = ovr ? ovr->additive : grp.transparent;
		if (transparent != (pass == RenderPass::Transparent)) continue;

		VkPipeline pipe = pass == RenderPass::Depth ? meshPrepass
			: !transparent ? meshOpaque : (ovr || (grp.flags & VkMesh::GRP_ADDITIVE)) ? meshAdditive : meshBlend;
		if (pipe != bound) {
			vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
			bound = pipe;
		}
		VkTexture *tex = ovr ? ovr->tex : grp.tex ? grp.tex : gc->TexMgr()->White();
		if (tex->set != boundTex) {
			vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &tex->set, 0, nullptr);
			boundTex = tex->set;
		}
		const TexMaps *maps = ovr ? nullptr : tex->maps;
		VkDescriptorSet ms = maps ? maps->set : gc->TexMgr()->DefaultMapSet();
		if (ms != boundMaps) {
			vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 3, 1, &ms, 0, nullptr);
			boundMaps = ms;
		}

		const MATERIAL &m = mesh->Material (grp.material);
		MeshPushConstants pc;
		Mat4 mv = (groupTransforms && g < groupTransforms->size()) ? modelView * (*groupTransforms)[g] : modelView;
		mv.ToFloat (pc.modelView);
		pc.diffuse[0] = m.diffuse.r, pc.diffuse[1] = m.diffuse.g, pc.diffuse[2] = m.diffuse.b, pc.diffuse[3] = m.diffuse.a;
		pc.emissive[0] = m.emissive.r, pc.emissive[1] = m.emissive.g, pc.emissive[2] = m.emissive.b;
		pc.emissive[3] = (grp.flags & VkMesh::GRP_NOLIGHT) ? 1.0f : 0.0f;
		pc.specular[0] = m.specular.r, pc.specular[1] = m.specular.g, pc.specular[2] = m.specular.b;
		pc.specular[3] = m.power;
		pc.light[0] = pc.light[1] = pc.light[2] = objLight;
		const PBRParams &pp = mesh->PBR (grp.material);
		pc.pbr[0] = pp.metalness, pc.pbr[1] = pp.roughness, pc.pbr[2] = pp.reflectivity, pc.pbr[3] = pp.f0;
		memcpy (pc.heat, objHeat, sizeof(objHeat));
		pc.maps[0] = maps ? (float)maps->flags : 0.0f;
		pc.maps[1] = transparent ? 1.0f : 0.0f; // not in the prepass: no ambient occlusion
		pc.maps[2] = pc.maps[3] = 0.0f;
		if (transparent && !ovr)
			for (int k = 0; k < 3; k++) pc.emissive[k] += objGlassGlow[k];
		if (ovr) { // self-illuminated display
			pc.diffuse[0] = pc.diffuse[1] = pc.diffuse[2] = pc.diffuse[3] = 1.0f;
			pc.emissive[3] = 1.0f;
		}
		vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
		vkCmdDrawIndexed (cmd, grp.nIndex, 1, grp.firstIndex, grp.vertexOffset, 0);
	}
}

void Scene::DrawSphere (const Mat4 &modelView, const float colour[4], bool unlit, VkTexture *tex)
{
	DrawGeometry (sphereVtx.buffer, sphereIdx.buffer, sphereNIdx, modelView, tex, colour, unlit, false);
}

void Scene::DrawGeometry (VkBuffer vb, VkBuffer ib, uint32_t nIdx, const Mat4 &modelView,
	VkTexture *tex, const float colour[4], bool unlit, bool blend)
{
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blend ? meshBlend : meshOpaque);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &frameSet[ctx->FrameIndex()], 0, nullptr);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 2, 1, &camAtmSet, 0, nullptr);
	VkDescriptorSet ts = (tex ? tex : gc->TexMgr()->White())->set;
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &ts, 0, nullptr);
	VkDescriptorSet ms = gc->TexMgr()->DefaultMapSet();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 3, 1, &ms, 0, nullptr);
	VkDeviceSize ofs = 0;
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
	vkCmdBindIndexBuffer (cmd, ib, 0, VK_INDEX_TYPE_UINT32);
	MeshPushConstants pc{};
	modelView.ToFloat (pc.modelView);
	memcpy (pc.diffuse, colour, 4 * sizeof(float));
	pc.light[0] = pc.light[1] = pc.light[2] = 1.0f;
	pc.pbr[1] = 1.0f; // matte, no reflections
	pc.emissive[3] = unlit ? 1.0f : 0.0f;
	pc.maps[1] = 1.0f; // not in the prepass: no ambient occlusion
	vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDrawIndexed (cmd, nIdx, 1, 0, 0, 0);
}

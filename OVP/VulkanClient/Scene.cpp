// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Scene.h"
#include "AO.h"
#include "SSR.h"
#include "RayTracing.h"
#include "RTLighting.h"
#include "TAA.h"
#include "Fsr.h"
#include "Clouds.h"
#include "EnvProbe.h"
#include "VObjects.h"
#include "PlanetSurface.h"
#include <chrono>
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
#include "prepass.vert.h"
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
		float shadowMat[3][16];   // view space -> shadow map, per cascade
		float shadowParams[3][4]; // enabled, normal offset, depth bias, texel size
		float jitter[4];       // projection jitter (NDC), render size
		float aoParams[4];     // 1/width, 1/height of the occlusion image, fraction applied to direct light, w: 1 = no occlusion
		float viewToGlobal[16];// view space -> global frame (rotation), for the environment probe
		float probe[4];        // x: environment probe valid, y: highest mip level, z: screen-space reflections valid, w: their debug view
		float rt[4];           // x: ray traced lighting valid, y: shadowed local lights
		float planetDir[4];    // direction to the planet nearby (view), w: sine of its angular radius
		float planetLight[4];  // its irradiance at normal incidence, w: present
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
	// cascades around the camera [m]; each is used if the previous one is
	// less than half its size
	constexpr double SHADOW_CAM_RADIUS[2] = { 600.0, 4000.0 };

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

	for (auto &sm : shadows) {
		sm = std::make_unique<ShadowMap>();
		sm->Init (ctx);
	}

	// per-frame uniform buffer, the shadow maps, ambient occlusion, the
	// environment probe and screen-space reflections (set 0)
	VkDescriptorSetLayoutBinding b[7]{};
	b[0].binding = 0;
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	b[0].descriptorCount = 1;
	b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT; // (ray traced reflections)
	b[1].binding = 1;
	b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b[1].descriptorCount = SHADOW_CASCADES;
	b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	b[2] = b[1];
	b[2].binding = 3; // ambient occlusion
	b[2].descriptorCount = 1;
	b[3] = b[2];
	b[3].binding = 4; // environment probe
	b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	b[4] = b[2];
	b[4].binding = 5; // screen-space reflections
	b[5] = b[2];
	b[5].binding = 6; // ray traced lighting: sun, ambient occlusion, planet light
	b[6] = b[2];
	b[6].binding = 7; // ray traced lighting: local lights
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 7;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &frameLayout));

	VkDescriptorPoolSize ps[2] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * vkc::FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (SHADOW_CASCADES + 5) * vkc::FRAMES_IN_FLIGHT }
	};
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2 * vkc::FRAMES_IN_FLIGHT;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &framePool));
	probe = std::make_unique<EnvProbe>();
	probe->Init (ctx);
	if (const char *e = getenv ("ORBITER_VK_PROBE")) probeEnabled = atoi (e) != 0;
	if (ctx->sceneSamples != VK_SAMPLE_COUNT_1_BIT) probeEnabled = false; // (pipelines are multisampled)

	// frame sets of the main view and of the environment probe
	for (uint32_t j = 0; j < 2 * vkc::FRAMES_IN_FLIGHT; j++) {
		uint32_t i = j % vkc::FRAMES_IN_FLIGHT;
		bool forProbe = j >= vkc::FRAMES_IN_FLIGHT;
		vkc::Buffer &ubo = forProbe ? probeFrameUbo[i] : frameUbo[i];
		VkDescriptorSet &set = forProbe ? probeFrameSet[i] : frameSet[i];
		ubo = ctx->CreateBuffer (sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
		memset (ubo.mapped, 0, sizeof(FrameUniforms));
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = framePool;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &frameLayout;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &set));
		VkDescriptorBufferInfo dbi{ ubo.buffer, 0, sizeof(FrameUniforms) };
		VkDescriptorImageInfo dii[SHADOW_CASCADES];
		for (int k = 0; k < SHADOW_CASCADES; k++)
			dii[k] = { shadows[k]->Sampler(), shadows[k]->View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo pii{ probe->Sampler(), probe->CubeView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet wr[3]{};
		for (auto &w : wr) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w.dstSet = set;
		wr[0].dstBinding = 0;
		wr[0].descriptorCount = 1;
		wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		wr[0].pBufferInfo = &dbi;
		wr[1].dstBinding = 1;
		wr[1].descriptorCount = SHADOW_CASCADES;
		wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[1].pImageInfo = dii;
		wr[2].dstBinding = 4;
		wr[2].descriptorCount = 1;
		wr[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[2].pImageInfo = &pii;
		vkUpdateDescriptorSets (ctx->device, 3, wr, 0, nullptr);
	}

	ao = std::make_unique<AmbientOcclusion>();
	ao->Init (ctx);
	ssr = std::make_unique<ScreenReflections>();
	ssr->Init (ctx);
	ssr->EnsureSize (ao->PrepassView (), ao->MaterialView (), ao->VelocityView ());
	ssr->BeginFrame ();
	WriteSSRDescriptors ();
	if (const char *e = getenv ("ORBITER_VK_TAA")) taaEnabled = atoi (e) != 0;
	taa = std::make_unique<TemporalAA>();
	taa->Init (ctx);
	// temporal upscaler: FSR 3 if built in (ORBITER_VK_UPSCALER = fsr or taa);
	// it needs a single-sampled scene
	fsr = std::make_unique<FsrUpscaler>();
	if (taaEnabled && FsrUpscaler::Available () && ctx->sceneSamples == VK_SAMPLE_COUNT_1_BIT) {
		const char *u = getenv ("ORBITER_VK_UPSCALER");
		useFsr = !(u && !strcmp (u, "taa"));
	}
	if (useFsr) fsr->Init (ctx);
	oapiWriteLogV ("VulkanClient: temporal anti-aliasing: %s", !taaEnabled ? "off" : useFsr ? "FSR 3" : "TAA");
	WriteAODescriptors ();

	atm = std::make_unique<AtmosphereRenderer>();
	atm->Init (ctx, gc->TexMgr());
	vclouds = std::make_unique<VolumetricClouds>();
	vclouds->Init (ctx, gc->TexMgr(), atm->SetLayout());
	// ray traced reflections (hardware ray queries)
	rt = std::make_unique<RTScene>();
	if (rt->Init (ctx, gc->TexMgr()) && ssr->enabled) {
		ssr->InitRT (frameLayout, rt->SetLayout(), rt->TextureLayout(), atm->SetLayout());
		gc->TexMgr()->onDestroy = [this](VkTexture *t) { if (rt) rt->ForgetTexture (t); };
	}
	// ray traced shadows, ambient occlusion and planet light
	rtl = std::make_unique<RTLighting>();
	if (rt->Enabled ()) rtl->Init (ctx, frameLayout, rt->SetLayout());
	rtl->EnsureSize (ao->PrepassView (), ao->VelocityView (), ao->MaterialView ());
	WriteRTLightDescriptors ();
	CreatePipelines ();
	BuildSphere ();
	tiles = std::make_unique<TileRenderer>();
	tiles->Init (ctx, gc->TexMgr(), frameLayout, atm->SetLayout());
	exhaustTex = gc->TexMgr()->Load ("Exhaust.dds");
	plumes = std::make_unique<PlumeRenderer>();
	plumes->Init (ctx, frameLayout, gc->TexMgr()->Layout(), atm->SetLayout(), vclouds->ShapeNoiseView (), vclouds->DetailNoiseView ());
	ssr->SetPlumes (plumes->Buffer (), plumes->BufferSize (), plumes->ShapeNoise (), plumes->DetailNoise ());
	particles = std::make_unique<ParticleSystem>();
	particles->Init (ctx, gc->TexMgr(), frameLayout, atm->SetLayout(), gc->TexMgr()->Layout(), vclouds->ShapeNoiseView (), vclouds->DetailNoiseView ());
	if (rt->Enabled ()) particles->InitRT (frameLayout, rt->SetLayout(), rt->TextureLayout(), atm->SetLayout());
	plumes->rayCast = [this](uint32_t owner, const VECTOR3 &vo, const VECTOR3 &vd, double maxDist, double &t, VECTOR3 &vn, int *kind) {
		VECTOR3 o = camPos + mul (camRot, vo), d = mul (camRot, vd);
		bool found = false;
		t = maxDist;
		for (auto &it : visuals) {
			if (it.second->Id () == owner) continue;
			double ti;
			VECTOR3 ni;
			if (it.second->RayHit (o, d, t, ti, ni)) {
				t = ti, vn = tmul (camRot, ni), found = true;
				if (kind) *kind = it.second->LastHitKind ();
			}
		}
		return found;
	};
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
	for (auto &sm : shadows) {
		if (sm) sm->Shutdown ();
		sm.reset();
	}
	if (stars) stars->Shutdown ();
	stars.reset();
	if (ao) ao->Shutdown ();
	ao.reset();
	if (rtl) rtl->Shutdown ();
	rtl.reset();
	if (gc->TexMgr()) gc->TexMgr()->onDestroy = nullptr;
	if (rt) rt->Shutdown ();
	rt.reset();
	if (ssr) ssr->Shutdown ();
	ssr.reset();
	if (taa) taa->Shutdown ();
	taa.reset();
	if (particles) particles->Shutdown ();
	particles.reset();
	if (plumes) plumes->Shutdown ();
	plumes.reset();
	ctx->DestroyImage (reactive);
	ctx->DestroyImage (reactiveMS);
	if (vclouds) vclouds->Shutdown ();
	vclouds.reset();
	if (fsr) fsr->Shutdown ();
	fsr.reset();
	ctx->DestroyBuffer (sphereVtx);
	ctx->DestroyBuffer (sphereIdx);
	for (auto &u : frameUbo) ctx->DestroyBuffer (u);
	for (auto &u : probeFrameUbo) ctx->DestroyBuffer (u);
	if (probe) probe->Shutdown ();
	probe.reset();
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
	VkShaderModule vs = prepass ? ctx->CreateShaderModule (spv_prepass_vert, spv_prepass_vert_size)
	                            : ctx->CreateShaderModule (spv_mesh_vert, spv_mesh_vert_size);
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
	VkPipelineColorBlendAttachmentState cbas[AmbientOcclusion::PREPASS_TARGETS] = { cba, cba, cba };
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = prepass ? AmbientOcclusion::PREPASS_TARGETS : 1;
	cb.pAttachments = cbas;

	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;

	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.colorAttachmentCount = prepass ? AmbientOcclusion::PREPASS_TARGETS : 1;
	rci.pColorAttachmentFormats = prepass ? AmbientOcclusion::PrepassFormats () : &ctx->sceneFormat;
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

void Scene::EnsureReactiveMask ()
{
	if (reactiveGen == ctx->SwapchainGeneration ()) return;
	vkDeviceWaitIdle (ctx->device);
	ctx->DestroyImage (reactive);
	ctx->DestroyImage (reactiveMS);
	VkExtent2D e = ctx->RenderExtent ();
	auto create = [this, e](VkSampleCountFlagBits samples, VkImageUsageFlags usage) {
		vkc::Image im;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = REACTIVE_FORMAT;
		ici.extent = { e.width, e.height, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = samples;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &im.image, &im.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = im.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = REACTIVE_FORMAT;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &im.view));
		return im;
	};
	reactive = create (VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	if (ctx->sceneSamples != VK_SAMPLE_COUNT_1_BIT)
		reactiveMS = create (ctx->sceneSamples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
	// (the images are written in every frame's scene before they are read)
	VkImage img = reactive.image;
	ctx->ImmediateSubmit ([img](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});
	reactiveGen = ctx->SwapchainGeneration ();
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

	VkExtent2D ext = ctx->RenderExtent();
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
	frameNo++;
	for (bool &v : shadowValid) v = false;
	// planet surfaces select their tiles and upload data first (transfers
	// end rendering scopes, and shadow maps are cleared when bound)
	for (auto &it : visuals) it.second->PrepareFrame ();
	if (!hSun) return;
	if (rtl && rtl->Enabled ()) return; // ray traced shadows instead (glass and probe faces are unshadowed)
	OBJHANDLE hTgt = oapiCameraTarget ();
	VECTOR3 c = camPos;
	double size = 0.0;
	if (hTgt) {
		oapiGetGlobalPos (hTgt, &c);
		size = oapiGetSize (hTgt);
	}
	VECTOR3 sp;
	oapiGetGlobalPos (hSun, &sp);
	auto render = [&](int i, const VECTOR3 &centre, double radius) {
		shadows[i]->Begin (centre, radius, unit (sp - centre));
		for (auto &it : visuals) it.second->RenderShadow (*shadows[i]);
		shadows[i]->End ();
		shadowValid[i] = true;
	};
	// around the camera target
	double r = std::clamp (SHADOW_RANGE * length (camPos - c) + 2.0 * size, SHADOW_RADIUS_MIN, SHADOW_RADIUS_MAX);
	if (Sunlight (c) > 0.0) render (0, c, r); // else the region is in darkness anyway
	// around the camera, for structures and terrain beyond the first map
	if (Sunlight (camPos) <= 0.0) return;
	for (int k = 0; k < 2; k++)
		if (r < 0.5 * SHADOW_CAM_RADIUS[k]) {
			render (k + 1, camPos, SHADOW_CAM_RADIUS[k]);
			r = SHADOW_CAM_RADIUS[k];
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

RTScene *Scene::RT () const
{
	return rt && rt->Enabled () ? rt.get() : nullptr;
}

void Scene::SetObjectMotion (const Mat4 &modelView, const Mat4 &prevModelView)
{
	// view space now -> object -> view space then -> clip then
	Mat4 m = Mat4::FromFloat (prevProj) * prevModelView * modelView.RigidInverse ();
	m.ToFloat (objMotion);
}

Mat4 Scene::PrevModelView (const VECTOR3 &gpos, const MATRIX3 &grot) const
{
	if (!havePrev) return ModelView (gpos, grot);
	return prevView * Mat4::FromRotTrans (grot, gpos - prevCamPos);
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
	for (uint32_t j = 0; j < 2 * vkc::FRAMES_IN_FLIGHT; j++) {
		uint32_t i = j % vkc::FRAMES_IN_FLIGHT;
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = j < vkc::FRAMES_IN_FLIGHT ? frameSet[i] : probeFrameSet[i];
		wr.dstBinding = 3;
		wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr.pImageInfo = &dii;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	}
}

void Scene::WriteRTLightDescriptors ()
{
	// (without ray tracing: the occlusion image, never read)
	bool on = rtl && rtl->Enabled ();
	VkDescriptorImageInfo dii[2] = {
		{ on ? rtl->Sampler() : ao->Sampler(), on ? rtl->VisView() : ao->View(), on ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ on ? rtl->Sampler() : ao->Sampler(), on ? rtl->LightView() : ao->View(), on ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	for (uint32_t j = 0; j < 2 * vkc::FRAMES_IN_FLIGHT; j++) {
		uint32_t i = j % vkc::FRAMES_IN_FLIGHT;
		for (int k = 0; k < 2; k++) {
			VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			wr.dstSet = j < vkc::FRAMES_IN_FLIGHT ? frameSet[i] : probeFrameSet[i];
			wr.dstBinding = 6 + k;
			wr.descriptorCount = 1;
			wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			wr.pImageInfo = &dii[k];
			vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
		}
	}
}

void Scene::WriteSSRDescriptors ()
{
	// (the sets of the current frame slot are not in use)
	uint32_t fi = ctx->FrameIndex();
	VkDescriptorImageInfo dii{ ssr->Sampler(), ssr->View(), VK_IMAGE_LAYOUT_GENERAL };
	for (VkDescriptorSet set : { frameSet[fi], probeFrameSet[fi] }) {
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = set;
		wr.dstBinding = 5;
		wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr.pImageInfo = &dii;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	}
}

void Scene::BeginFrame ()
{
	uint32_t fi = ctx->FrameIndex();
	if (ao->EnsureSize ()) WriteAODescriptors (), WriteRTLightDescriptors (); // the GPU is idle then
	if (rtl->EnsureSize (ao->PrepassView (), ao->VelocityView (), ao->MaterialView ())) WriteRTLightDescriptors ();
	ssr->EnsureSize (ao->PrepassView (), ao->MaterialView (), ao->VelocityView ());
	ssr->BeginFrame ();
	WriteSSRDescriptors ();
	EnsureReactiveMask ();
	// (before any of their sets are bound in this frame's commands)
	plumes->SetPrepass (ao->PrepassView ());
	particles->SetPrepass (ao->PrepassView ());
	if (rt) rt->BeginFrame ();
	// the previous frame does not continue into this one (camera cut)
	cameraCut = !havePrev || length (camPos - prevCamPos) > 5e3;

	// sub-pixel projection jitter for temporal anti-aliasing (Halton 2, 3)
	jitter[0] = jitter[1] = 0.0f;
	if (taaEnabled) {
		auto halton = [](uint32_t i, uint32_t b) {
			double f = 1.0, r = 0.0;
			for (; i; i /= b) { f /= b; r += f * (i % b); }
			return r;
		};
		if (useFsr) { // the upscaler's sequence (longer when upscaling)
			FsrUpscaler::Jitter (jitterIndex++, viewW, ctx->Extent().width, jitterPixels[0], jitterPixels[1]);
		} else {
			jitterIndex = jitterIndex % 16 + 1;
			jitterPixels[0] = (float)(halton (jitterIndex, 2) - 0.5);
			jitterPixels[1] = (float)(halton (jitterIndex, 3) - 0.5);
		}
		// (Vulkan clip space: y down, as pixel rows)
		jitter[0] = 2.0f * jitterPixels[0] / viewW;
		jitter[1] = 2.0f * jitterPixels[1] / viewH;
	}
	proj[8] = jitter[0]; // clip.xy += jitter * z (= w)
	proj[9] = jitter[1];
	if (!havePrev) {
		memcpy (prevProj, proj, sizeof(prevProj));
		prevProj[8] = prevProj[9] = 0.0f;
	}
	SetObjectMotion (Mat4::Identity (), Mat4::Identity ());

	// engine plumes (reflections march through them)
	auto tc0 = std::chrono::steady_clock::now ();
	plumes->BeginFrame (oapiGetSimTime ());
	for (auto &it : visuals) it.second->CollectPlumes (*plumes);
	{
		// (ORBITER_VK_PROFILE: CPU time of the plume collection, surface tests included)
		static const bool profile = getenv ("ORBITER_VK_PROFILE") && atoi (getenv ("ORBITER_VK_PROFILE"));
		static double acc = 0.0;
		static int n = 0;
		acc += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - tc0).count ();
		if (profile && ++n == 120) {
			oapiWriteLogV ("VulkanClient: plume collection (CPU) %.3f ms, %u plumes", acc / n, plumes->Count ());
			acc = 0.0, n = 0;
		}
	}
	ssr->SetPlumeSlot (plumes->Slot ());

	// frame uniforms
	FrameUniforms fu{};
	FillFrameUniforms (&fu);
	fu.probe[0] = probeFacesDone >= 6 ? 1.0f : 0.0f;
	fu.probe[1] = (float)(EnvProbe::LEVELS - 1);
	fu.probe[2] = ssr->Valid () && !cameraCut ? 1.0f : 0.0f;
	fu.probe[3] = ssr->debug ? 1.0f : 0.0f;
	fu.rt[0] = rtl->Enabled () ? 1.0f : 0.0f;
	fu.rt[1] = (float)RTLighting::SHADOWED_LIGHTS;
	memcpy (frameUbo[fi].mapped, &fu, sizeof(fu));
	UpdateCamAtmSet ();
}

void Scene::UpdateCamAtmSet ()
{
	// atmosphere of the planet the camera is at, for vessels and bases
	camAtmSet = atm->NoAtmosphereSet ();
	if (OBJHANDLE hp = oapiCameraProxyGbody ()) {
		auto it = visuals.find (hp);
		if (it != visuals.end())
			if (Atmosphere *a = it->second->GetAtmosphere ())
				camAtmSet = a->Set (); // also without atmosphere: the planet is reflected
	}
}

void Scene::PlanetAlbedo (OBJHANDLE hp, float alb[3])
{
	// Bond albedo and tint of the bodies most often seen up close
	char name[64];
	oapiGetObjectName (hp, name, 64);
	struct { const char *name; float a[3]; } tab[] = {
		{ "Earth", { 0.27f, 0.30f, 0.36f } }, { "Moon", { 0.12f, 0.12f, 0.11f } },
		{ "Mars", { 0.30f, 0.20f, 0.14f } }, { "Mercury", { 0.09f, 0.09f, 0.085f } },
		{ "Venus", { 0.78f, 0.72f, 0.58f } }, { "Jupiter", { 0.36f, 0.33f, 0.28f } },
		{ "Saturn", { 0.36f, 0.33f, 0.26f } }, { "Titan", { 0.27f, 0.22f, 0.14f } } };
	for (auto &t : tab)
		if (!strcmp (name, t.name)) { for (int k = 0; k < 3; k++) alb[k] = t.a[k]; return; }
	alb[0] = alb[1] = alb[2] = 0.25f;
}

void Scene::FillFrameUniforms (void *pfu)
{
	FrameUniforms &fu = *(FrameUniforms*)pfu;
	fu.jitter[0] = jitter[0], fu.jitter[1] = jitter[1];
	fu.jitter[2] = (float)viewW, fu.jitter[3] = (float)viewH;
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
	for (int k = 0; k < 3; k++) ambientRGB[k] = fu.ambient[k];
	// sunlight reflected by the planet nearby (a Lambertian sphere, by its phase)
	if (OBJHANDLE hp = oapiCameraProxyGbody ())
		if (hp != hSun && hSun) {
			VECTOR3 pp, sp;
			oapiGetGlobalPos (hp, &pp);
			oapiGetGlobalPos (hSun, &sp);
			double R = oapiGetSize (hp);
			VECTOR3 toP = pp - camPos;
			double d = std::max (length (toP), R);
			double alpha = acos (std::clamp (dotp (unit (sp - pp), unit (camPos - pp)), -1.0, 1.0));
			double phase = (sin (alpha) + (PI - alpha) * cos (alpha)) / PI;
			double e = (R / d) * (R / d) * phase;
			float alb[3];
			PlanetAlbedo (hp, alb);
			VECTOR3 dv = view.TransformPoint (toP / length (toP));
			fu.planetDir[0] = (float)dv.x, fu.planetDir[1] = (float)dv.y, fu.planetDir[2] = (float)dv.z;
			fu.planetDir[3] = (float)std::min (1.0, R / d);
			for (int k = 0; k < 3; k++) fu.planetLight[k] = (float)(e * alb[k]);
			fu.planetLight[3] = e > 1e-5 ? 1.0f : 0.0f;
		}
	auto shadowUniforms = [this](const ShadowMap &sm, float *mat, float *prm) {
		sm.ReceiverMatrix (camPos, camRot).ToFloat (mat);
		double texel = sm.TexelSize ();
		prm[0] = 1.0f;
		prm[1] = (float)(1.5 * texel);                       // normal offset [m]
		prm[2] = (float)(texel / (6.0 * sm.Radius()));       // depth bias (one texel in depth units)
		prm[3] = 1.0f / ShadowMap::SIZE;
	};
	for (int i = 0; i < SHADOW_CASCADES; i++)
		if (shadowValid[i]) shadowUniforms (*shadows[i], fu.shadowMat[i], fu.shadowParams[i]);
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
		u.spot[1] = (float)l.owner;
		u.spot[2] = (float)l.shadowOfs;
	}
	Mat4::FromRotTrans (camRot, _V(0, 0, 0)).ToFloat (fu.viewToGlobal);
}


void Scene::RenderPrepass ()
{
	// (planet surfaces were prepared before the shadow maps)
	ao->BeginPrepass ();
	for (auto &it : visuals) {
		objOwner = it.second->Id ();
		it.second->Render (RenderPass::Depth);
	}
	objOwner = 0;
	ctx->EndTarget ();
	ctx->GpuMark ("prepass");
	if (ao->enabled && !rtl->Enabled ()) ao->Compute (proj[0], -proj[5]);
	ctx->GpuMark ("ao");
	if (rt && rt->Enabled ()) {
		rt->Build ();
		ctx->GpuMark ("rt build");
		if (rtl->Enabled ()) {
			double sunRadius = 0.0047;
			if (hSun) {
				VECTOR3 sp;
				oapiGetGlobalPos (hSun, &sp);
				sunRadius = asin (std::min (1.0, oapiGetSize (hSun) / length (sp - camPos)));
			}
			rtl->Compute (frameSet[ctx->FrameIndex()], rt->Set (), proj[0], -proj[5], jitter, (float)sunRadius, cameraCut);
			ctx->GpuMark ("rt lighting");
		}
		ssr->SetRTSets (frameSet[ctx->FrameIndex()], rt->Set (), rt->TextureSet (), camAtmSet);
	}
	ssr->Compute (proj[0], -proj[5], jitter, cameraCut);
	ctx->GpuMark ("ssr");
	{
		VkDescriptorSet rtSets[2] = { rt && rt->Enabled () ? rt->Set () : VK_NULL_HANDLE, rt && rt->Enabled () ? rt->TextureSet () : VK_NULL_HANDLE };
		VECTOR3 sunDir = _V(1, 0, 0);
		if (hSun) {
			VECTOR3 sp;
			oapiGetGlobalPos (hSun, &sp);
			sunDir = unit (sp - camPos);
		}
		particles->Simulate (camPos, sunDir, frameSet[ctx->FrameIndex()], camAtmSet, rtSets[0] ? rtSets : nullptr, oapiGetSimTime ());
		particles->RenderScreenSmoke (frameSet[ctx->FrameIndex()]);
		ctx->GpuMark ("particles");
	}
	RenderVolumetricClouds ();
	ctx->GpuMark ("clouds");
}

void Scene::RenderProbe ()
{
	if (!probeEnabled) return;
	OBJHANDLE hT = oapiCameraTarget ();
	if (!hT || oapiGetObjectType (hT) != OBJTP_VESSEL) { probeFacesDone = 0; return; }
	if (hT != probeTarget) probeTarget = hT, probeFacesDone = 0;
	VECTOR3 pos;
	oapiGetGlobalPos (hT, &pos);

	// camera of the cube face (Vulkan face order +x -x +y -y +z -z): right,
	// up and forward axes (global frame)
	static const VECTOR3 F[6][3] = {
		{ {0,0,-1}, {0,1,0}, {1,0,0} },  { {0,0,1}, {0,1,0}, {-1,0,0} },
		{ {1,0,0}, {0,0,-1}, {0,1,0} },  { {1,0,0}, {0,0,1}, {0,-1,0} },
		{ {1,0,0}, {0,1,0}, {0,0,1} },   { {-1,0,0}, {0,1,0}, {0,0,-1} } };
	int f = probeFace;
	const VECTOR3 &r = F[f][0], &u = F[f][1], &fw = F[f][2];

	// save the main view
	VECTOR3 sCamPos = camPos;
	MATRIX3 sCamRot = camRot;
	Mat4 sView = view;
	float sProj[16];
	memcpy (sProj, proj, sizeof(proj));
	uint32_t sW = viewW, sH = viewH;
	double sTan = tanAperture;
	VkDescriptorSet sAtm = camAtmSet;
	bool sClouds = cloudsActive;

	camPos = pos;
	camRot = { r.x, u.x, fw.x,  r.y, u.y, fw.y,  r.z, u.z, fw.z }; // columns: camera axes
	MATRIX3 vt = { camRot.m11, camRot.m21, camRot.m31, camRot.m12, camRot.m22, camRot.m32, camRot.m13, camRot.m23, camRot.m33 };
	view = Mat4::FromRotTrans (vt, _V(0, 0, 0));
	memset (proj, 0, sizeof(proj));
	proj[0] = 1.0f, proj[5] = -1.0f, proj[11] = 1.0f, proj[14] = (float)nearPlane; // 90 degrees
	viewW = viewH = EnvProbe::SIZE;
	tanAperture = 1.0;
	cloudsActive = false;
	inProbe = true;

	FrameUniforms fu{};
	float sJit[2] = { jitter[0], jitter[1] };
	jitter[0] = jitter[1] = 0.0f;
	FillFrameUniforms (&fu);
	jitter[0] = sJit[0], jitter[1] = sJit[1];
	fu.aoParams[3] = 1.0f; // no ambient occlusion
	fu.probe[0] = 0.0f;    // reflections from the analytic environment
	memcpy (probeFrameUbo[ctx->FrameIndex()].mapped, &fu, sizeof(fu));
	UpdateCamAtmSet ();

	vkc::Target ft = probe->FaceTarget (), fd = probe->FaceDepth ();
	ctx->SetSceneRedirect (&ft, &fd);
	ctx->BindScene (true);
	// (no stars: at the probe's resolution they would cover the sky with
	// speckle, and their reflections are too faint to see)
	for (auto &it : visuals) it.second->RenderSky ();
	for (auto &it : visuals)
		if (it.first != hT) it.second->Render (RenderPass::Opaque);
	ctx->BindScene (true);
	for (auto &it : visuals) // cloud layers (vessel exhaust and glass are left out)
		if (dynamic_cast<VPlanet*>(it.second.get())) it.second->Render (RenderPass::Transparent);
	ctx->SetSceneRedirect (nullptr, nullptr);
	probe->StoreFace (f);
	probe->Prefilter ();

	// restore the main view
	inProbe = false;
	camPos = sCamPos, camRot = sCamRot, view = sView;
	memcpy (proj, sProj, sizeof(proj));
	viewW = sW, viewH = sH, tanAperture = sTan;
	camAtmSet = sAtm, cloudsActive = sClouds;
	probeFace = (probeFace + 1) % 6;
	if (probeFacesDone < 6) probeFacesDone++;
}

bool Scene::SunOnScreen (float uv[2]) const
{
	if (!hSun) return false;
	VECTOR3 sp;
	oapiGetGlobalPos (hSun, &sp);
	VECTOR3 v = ViewDir (unit (sp - camPos));
	if (v.z <= 1e-3) return false;
	uv[0] = (float)(0.5 + 0.5 * proj[0] * v.x / v.z);
	uv[1] = (float)(0.5 + 0.5 * proj[5] * v.y / v.z);
	return uv[0] > -0.2f && uv[0] < 1.2f && uv[1] > -0.2f && uv[1] < 1.2f;
}

float Scene::CloudFadeRadius () const
{
	return cloudsActive ? (float)VolumetricClouds::RANGE : 0.0f;
}

void Scene::RenderVolumetricClouds ()
{
	cloudsActive = false;
	OBJHANDLE hp = oapiCameraProxyGbody ();
	if (!hp || !vclouds->enabled) return;
	auto it = visuals.find (hp);
	if (it == visuals.end()) return;
	VPlanet *vp = dynamic_cast<VPlanet*>(it->second.get());
	Atmosphere *a = vp ? vp->GetAtmosphere () : nullptr;
	if (!vp || !vp->CloudLayer () || !a || !a->Present ()) return;
	VECTOR3 pp;
	MATRIX3 prot;
	oapiGetGlobalPos (hp, &pp);
	oapiGetRotationMatrix (hp, &prot);
	double R = oapiGetSize (hp);
	VECTOR3 cam = tmul (prot, camPos - pp);
	if (length (cam) - R > VolumetricClouds::MAX_CAM_ALT) return;
	VolumetricClouds::Frame f{};
	f.clouds = vp->CloudLayer ();
	f.atmSet = a->Set ();
	f.camPlanet = cam;
	f.planetRadius = R;
	f.cloudAlt = vp->CloudAlt ();
	f.prepass = ao->PrepassView ();
	f.fx = proj[0], f.fy = -proj[5];
	for (int k = 0; k < 3; k++) f.ambient[k] = ambientRGB[k];
	// slow drift of the cloud shapes
	double t = fmod (oapiGetSimTime (), 1e5);
	f.windOffset[0] = (float)(8.0 * t), f.windOffset[1] = 0.0f, f.windOffset[2] = (float)(5.0 * t);
	vclouds->Render (f);
	cloudsActive = true;
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
	VkDescriptorSet fset = FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &fset, 0, nullptr);
	for (auto *vo : list) vo->Render (RenderPass::Opaque);

	// scorch marks on the ground (darkening the lit surface)
	plumes->DrawScorch (camPos, camRot, FrameSet ());

	// volumetric clouds over the opaque scene
	if (cloudsActive) {
		ctx->BindScene (true);
		vclouds->Composite ();
	}

	std::sort (list.begin(), list.end(), [](VObject *a, VObject *b) { return a->CamDist() > b->CamDist(); });
	for (auto *vo : list) vo->Render (RenderPass::Transparent);

	// heat shimmer behind the exhaust (before the plumes and particles)
	ctx->GpuMark ("scene");
	plumes->Shimmer (proj[0], -proj[5], jitter);
	ctx->GpuMark ("shimmer");

	// effects that also write the reactive mask: engine plumes, particles
	vkc::Target rt{ reactive.image, reactive.view, REACTIVE_FORMAT, ctx->RenderExtent () };
	vkc::Target rtMS{ reactiveMS.image, reactiveMS.view, REACTIVE_FORMAT, ctx->RenderExtent () };
	ctx->BindSceneWithMask (rt, reactiveMS.image ? &rtMS : nullptr);
	plumes->Draw (FrameSet (), camAtmSet, gc->TexMgr()->White());
	particles->DrawVolume (FrameSet ());
	particles->DrawScreenSmoke (FrameSet ());
	for (auto *ps : gc->ParticleStreams()) ps->Render (this);
	for (auto *vo : list) vo->DrawParticles ();
}

VkImageView Scene::ResolveTemporal (VkExtent2D &size)
{
	ssr->StoreColor (ctx->SceneImage ()); // reflected in the next frame
	VkImageView out = ctx->SceneView ();
	size = ctx->RenderExtent ();
	if (taaEnabled) {
		// rotation from the view now to the view then, for directions (sky)
		Mat4 rot = prevView * view.RigidInverse ();
		Mat4 skyMotion = Mat4::FromFloat (prevProj) * rot;
		// discard the history after a camera cut
		bool reset = cameraCut;
		VkImageView up = VK_NULL_HANDLE;
		if (useFsr) {
			double t = std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
			FsrUpscaler::Inputs in{};
			in.color = ctx->SceneImage (), in.colorFormat = ctx->sceneFormat;
			in.depth = ctx->DepthImage ();
			in.velocity = ao->VelocityImage (), in.velocityFormat = AmbientOcclusion::VELOCITY_FORMAT;
			in.reactive = reactive.image;
			in.jitter[0] = jitterPixels[0], in.jitter[1] = jitterPixels[1];
			in.frameTime = lastFrameTime < 0.0 ? 1.0f / 60.0f : (float)std::min (t - lastFrameTime, 0.5);
			lastFrameTime = t;
			in.nearPlane = (float)nearPlane;
			in.fovY = (float)(2.0 * oapiCameraAperture ());
			in.reset = reset;
			up = fsr->Dispatch (in);
			if (up) {
				out = up;
				size = ctx->Extent ();
			}
		}
		if (!up)
			out = taa->Resolve (ctx->SceneView (), ao->PrepassView (), ao->VelocityView (), reactive.view, skyMotion,
				proj[0], -proj[5], reset);
	}
	// this frame's camera and object poses, for the next frame's motion
	prevCamPos = camPos;
	prevView = view;
	memcpy (prevProj, proj, sizeof(prevProj));
	prevProj[8] = prevProj[9] = 0.0f;
	havePrev = true;
	for (auto &it : visuals) it.second->StorePrev ();
	return out;
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
	VkDescriptorSet fset = FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &fset, 0, nullptr);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 2, 1, &camAtmSet, 0, nullptr);
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
	vkCmdBindIndexBuffer (cmd, mesh->IndexBuffer(), 0, VK_INDEX_TYPE_UINT16);

	VkPipeline bound = VK_NULL_HANDLE;
	VkDescriptorSet boundTex = VK_NULL_HANDLE, boundMaps = VK_NULL_HANDLE;
	rtParts.clear ();
	for (uint32_t g = 0; g < mesh->nGroup(); g++) {
		const VkMesh::Group &grp = mesh->GetGroup (g);
		if (!grp.nIndex || (grp.usrFlag & VkMesh::GRP_HIDDEN)) continue;
		if (groupVisible && g < groupVisible->size() && !(*groupVisible)[g]) continue;
		const GroupTexOverride *ovr = nullptr;
		if (texOverride)
			for (auto &o : *texOverride)
				if (o.group == g) { ovr = &o; break; }
		bool transparent = ovr ? ovr->additive : grp.transparent;
		if (transparent != (pass == RenderPass::Transparent)) continue;

		VkPipeline pipe = pass == RenderPass::Depth ? meshPrepass
			: !transparent ? meshOpaque : (ovr || (grp.usrFlag & VkMesh::GRP_ADDITIVE)) ? meshAdditive : meshBlend;
		if (pipe != bound) {
			vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
			bound = pipe;
		}
		// the group, and the triangles with another texture and material
		// (material configuration SIDE_TEXTURE)
		bool split = grp.sideTex && grp.nSide && !ovr;
		for (int part = 0; part < (split ? 2 : 1); part++) {
			uint32_t mat = part ? grp.sideMaterial : grp.material;
			uint32_t first = part ? grp.firstIndex + grp.nIndex - grp.nSide : grp.firstIndex;
			uint32_t count = part ? grp.nSide : split ? grp.nIndex - grp.nSide : grp.nIndex;
			VkTexture *tex = part ? grp.sideTex : ovr ? ovr->tex : grp.tex ? grp.tex : gc->TexMgr()->White();
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

			const MATERIAL &m = mesh->Material (mat);
			MeshPushConstants pc;
			Mat4 mv = (groupTransforms && g < groupTransforms->size()) ? modelView * (*groupTransforms)[g] : modelView;
			mv.ToFloat (pc.modelView);
			pc.diffuse[0] = m.diffuse.r, pc.diffuse[1] = m.diffuse.g, pc.diffuse[2] = m.diffuse.b, pc.diffuse[3] = m.diffuse.a;
			pc.emissive[0] = m.emissive.r, pc.emissive[1] = m.emissive.g, pc.emissive[2] = m.emissive.b;
			pc.emissive[3] = (grp.usrFlag & VkMesh::GRP_NOLIGHT) ? 1.0f : 0.0f;
			pc.specular[0] = m.specular.r, pc.specular[1] = m.specular.g, pc.specular[2] = m.specular.b;
			pc.specular[3] = m.power;
			pc.light[0] = pc.light[1] = pc.light[2] = objLight;
			const PBRParams &pp = mesh->PBR (mat);
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
			if (pass == RenderPass::Depth) { // model-view, motion and material (prepass.vert/frag)
				float pp[40];
				memcpy (pp, pc.modelView, sizeof(pc.modelView));
				memcpy (pp + 16, objMotion, sizeof(objMotion));
				memcpy (pp + 32, pc.pbr, sizeof(pc.pbr));
				pp[36] = pc.maps[0];
				pp[37] = pc.emissive[3]; // unlit
				pp[38] = pp[39] = 0.0f;
				vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), pp);
			} else {
				vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
			}
			vkCmdDrawIndexed (cmd, count, 1, first, grp.vertexOffset, 0);
			if (pass == RenderPass::Depth && rt && rt->Enabled ()) { // into the ray tracing scene
				RTScene::Part part;
				part.geo = { mesh->Id(), mesh->GeomVersion(), mesh->VertexBuffer(),
					(VkDeviceSize)grp.vertexOffset * sizeof(NTVERTEX), mesh->GroupVertexCount (g),
					mesh->IndexBuffer(), first, count };
				part.tex = tex;
				RTScene::Material &rm = part.mat;
				memcpy (rm.diffuse, pc.diffuse, sizeof(rm.diffuse));
				memcpy (rm.emissive, pc.emissive, sizeof(rm.emissive));
				rm.light = objLight;
				memcpy (rm.pbr, pc.pbr, sizeof(rm.pbr));
				rm.unlit = pc.emissive[3] > 0.5f;
				rm.owner = objOwner;
				// static groups: one instance for the mesh (below); animated
				// ones on their own
				if (mesh->rtAnimated.size() != mesh->nGroup()) mesh->rtAnimated.assign (mesh->nGroup(), 0);
				if (groupTransforms && g < groupTransforms->size() && !(*groupTransforms)[g].IsIdentity ())
					mesh->rtAnimated[g] = 1;
				if (mesh->rtAnimated[g]) rt->AddInstance (&part, 1, mv);
				else rtParts.push_back (part);
			}
		}
	}
	if (!rtParts.empty()) rt->AddInstance (rtParts.data(), (uint32_t)rtParts.size(), modelView);
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
	VkDescriptorSet fset = FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &fset, 0, nullptr);
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

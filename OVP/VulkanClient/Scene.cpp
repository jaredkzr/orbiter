// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Scene.h"
#include "VObjects.h"
#include "VkMesh.h"
#include "VkTexture.h"
#include "VulkanClient.h"
#include "StarSphere.h"
#include "Particles.h"
#include "PlanetSurface.h"
#include <algorithm>
#include <cstring>

#include "mesh.vert.h"
#include "mesh.frag.h"
#include "stars.vert.h"
#include "stars.frag.h"
#include "particle.vert.h"
#include "particle.frag.h"

namespace {

	// Must match the Frame uniform block in the shaders
	struct FrameUniforms {
		float proj[16];
		float sunDir[4];
		float sunColor[4];
		float ambient[4];
	};

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

	// per-frame uniform buffer (set 0)
	VkDescriptorSetLayoutBinding b{};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 1;
	lci.pBindings = &b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &frameLayout));

	VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, vkc::FRAMES_IN_FLIGHT };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = vkc::FRAMES_IN_FLIGHT;
	pci.poolSizeCount = 1;
	pci.pPoolSizes = &ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &framePool));

	for (uint32_t i = 0; i < vkc::FRAMES_IN_FLIGHT; i++) {
		frameUbo[i] = ctx->CreateBuffer (sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = framePool;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &frameLayout;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &frameSet[i]));
		VkDescriptorBufferInfo dbi{ frameUbo[i].buffer, 0, sizeof(FrameUniforms) };
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = frameSet[i];
		wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		wr.pBufferInfo = &dbi;
		vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	}

	CreatePipelines ();
	BuildSphere ();
	tiles = std::make_unique<TileRenderer>();
	tiles->Init (ctx, gc->TexMgr(), frameLayout);
	exhaustTex = gc->TexMgr()->Load ("Exhaust.dds");

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
	if (tiles) tiles->Shutdown ();
	tiles.reset();
	if (stars) stars->Shutdown ();
	stars.reset();
	ctx->DestroyBuffer (sphereVtx);
	ctx->DestroyBuffer (sphereIdx);
	for (auto &u : frameUbo) ctx->DestroyBuffer (u);
	VkPipeline pipes[] = { meshOpaque, meshBlend, meshAdditive, starPipeline, particleAdditive, particleBlend };
	for (auto p : pipes) if (p) vkDestroyPipeline (ctx->device, p, nullptr);
	vkDestroyPipelineLayout (ctx->device, meshLayout, nullptr);
	vkDestroyPipelineLayout (ctx->device, particleLayout, nullptr);
	gc->TexMgr()->Release (exhaustTex);
	vkDestroyPipelineLayout (ctx->device, starLayout, nullptr);
	vkDestroyDescriptorPool (ctx->device, framePool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, frameLayout, nullptr);
	ctx = nullptr;
}

// ======================================================================
// Pipelines

VkPipeline Scene::CreateMeshPipeline (bool blend, bool additive, bool depthWrite)
{
	VkShaderModule vs = ctx->CreateShaderModule (spv_mesh_vert, spv_mesh_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_mesh_frag, spv_mesh_frag_size);
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
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
	rci.pColorAttachmentFormats = &ctx->colorFormat;
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
	VkDescriptorSetLayout sets[2] = { frameLayout, gc->TexMgr()->Layout() };
	VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 2;
	plci.pSetLayouts = sets;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &meshLayout));

	meshOpaque = CreateMeshPipeline (false, false, true);
	meshBlend = CreateMeshPipeline (true, false, false);
	meshAdditive = CreateMeshPipeline (true, true, false);

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
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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
	rci.pColorAttachmentFormats = &ctx->colorFormat;
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
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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
	rci.pColorAttachmentFormats = &ctx->colorFormat;
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

void Scene::Render ()
{
	VkCommandBuffer cmd = ctx->Cmd();
	uint32_t fi = ctx->FrameIndex();

	// frame uniforms
	FrameUniforms fu{};
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
	fu.ambient[0] = fu.ambient[1] = fu.ambient[2] = 0.03f;
	memcpy (frameUbo[fi].mapped, &fu, sizeof(fu));

	// background stars
	float viewf[16], vp[16];
	view.ToFloat (viewf);
	MulMat4f (proj, viewf, vp);
	stars->Render (cmd, starPipeline, starLayout, vp);

	// scene geometry
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &frameSet[fi], 0, nullptr);

	std::vector<VObject*> list;
	for (auto &it : visuals) list.push_back (it.second.get());
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
	vkCmdBindVertexBuffers (cmd, 0, 1, &vb, &ofs);
	vkCmdBindIndexBuffer (cmd, mesh->IndexBuffer(), 0, VK_INDEX_TYPE_UINT16);

	VkPipeline bound = VK_NULL_HANDLE;
	VkDescriptorSet boundTex = VK_NULL_HANDLE;
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

		VkPipeline pipe = !transparent ? meshOpaque : (ovr || (grp.flags & VkMesh::GRP_ADDITIVE)) ? meshAdditive : meshBlend;
		if (pipe != bound) {
			vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
			bound = pipe;
		}
		VkTexture *tex = ovr ? ovr->tex : grp.tex ? grp.tex : gc->TexMgr()->White();
		if (tex->set != boundTex) {
			vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &tex->set, 0, nullptr);
			boundTex = tex->set;
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
		if (ovr) { // self-illuminated display
			pc.diffuse[0] = pc.diffuse[1] = pc.diffuse[2] = pc.diffuse[3] = 1.0f;
			pc.emissive[3] = 1.0f;
		}
		vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
		vkCmdDrawIndexed (cmd, grp.nIndex, 1, grp.firstIndex, grp.vertexOffset, 0);
	}
}

void Scene::DrawSphere (const Mat4 &modelView, const float colour[4], bool unlit)
{
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshOpaque);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &frameSet[ctx->FrameIndex()], 0, nullptr);
	VkDescriptorSet ws = gc->TexMgr()->White()->set;
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &ws, 0, nullptr);
	VkDeviceSize ofs = 0;
	vkCmdBindVertexBuffers (cmd, 0, 1, &sphereVtx.buffer, &ofs);
	vkCmdBindIndexBuffer (cmd, sphereIdx.buffer, 0, VK_INDEX_TYPE_UINT32);
	MeshPushConstants pc{};
	modelView.ToFloat (pc.modelView);
	memcpy (pc.diffuse, colour, 4 * sizeof(float));
	pc.emissive[3] = unlit ? 1.0f : 0.0f;
	vkCmdPushConstants (cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDrawIndexed (cmd, sphereNIdx, 1, 0, 0, 0);
}

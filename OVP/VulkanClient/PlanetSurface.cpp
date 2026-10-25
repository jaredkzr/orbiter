// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "PlanetSurface.h"
#include "tile_mesh.comp.h"
#include <atomic>
#include <thread>
#include <chrono>
#include "RayTracing.h"
#include "Scene.h"
#include "VkTexture.h"
#include "VulkanClient.h"
#include "Atmosphere.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "tile.vert.h"
#include "tile.frag.h"
#include "prepass.vert.h"
#include "prepass_tile.frag.h"
#include "prepass_tile.vert.h"
#include "AO.h"
#include "Shadows.h"

namespace {

	// Must match the push constant block of tile.vert/frag
	struct TilePushConstants {
		float modelView[16];
		float surfUV[4];  // offset (xy) and scale (zw) into the surface texture
		float maskUV[4];  // same for the mask texture
		float params[4];  // x: mask present, y: night light intensity
		float detail[4];  // terrain detail: data node spacing [m], shortest wavelength of the mesh [m], Hurst exponent, on
		float morph[4];   // morph to the parent's mesh: start distance [m], 1/range [1/m] (0: none); z: sunlight (eclipses); w: pebble cells
		float vessel[4];  // nearest vessel on the ground (view space; w: present), also morphed from
		int32_t refI[4];  // terrain detail: the tile's reference point d0 R [m], integer part
		float refF[4];    // fraction; w: radius
		float refRel[4];  // d0 (R + h0) relative to the camera (planet frame); w: R + h0
	};

	constexpr double SPLIT_PIXELS = 500.0;  // split tiles that cover more than this on screen (at resolution bias 0; GRID quads per edge)
	constexpr size_t MAX_OUTSTANDING = 24;  // tile requests in flight
	constexpr int MAX_UPLOADS = 12;         // tiles accepted per frame
	constexpr uint64_t PRUNE_FRAMES = 120;  // unused subtrees are released after this

	VECTOR3 SurfaceDir (double lat, double lng)
	{
		double cl = cos (lat);
		return _V(cl * cos (lng), sin (lat), cl * sin (lng));
	}

	// ------------------------------------------------------------------
	// Elevation tile files (.elv)

	// nodes without data (-32768, e.g. Vesta's unmapped north) filled from
	// their neighbours pass by pass, exactly as the core does
	// (ElevationManager: FillElevationGaps)
	void FillElevationGaps (int16_t *e, int n)
	{
		const int16_t NODATA = -32768;
		bool any = false;
		for (int i = 0; i < n * n && !any; i++) any = e[i] == NODATA;
		if (!any) return;
		std::vector<int16_t> next (e, e + n * n);
		for (int pass = 0; pass < 2 * n; pass++) {
			bool left = false, changed = false;
			for (int y = 0; y < n; y++)
				for (int x = 0; x < n; x++) {
					int i = y * n + x;
					if (e[i] != NODATA) continue;
					long sum = 0;
					int cnt = 0;
					if (x > 0 && e[i - 1] != NODATA) sum += e[i - 1], cnt++;
					if (x < n - 1 && e[i + 1] != NODATA) sum += e[i + 1], cnt++;
					if (y > 0 && e[i - n] != NODATA) sum += e[i - n], cnt++;
					if (y < n - 1 && e[i + n] != NODATA) sum += e[i + n], cnt++;
					if (cnt) next[i] = (int16_t)std::floor ((double)sum / cnt + 0.5), changed = true;
					else left = true;
				}
			std::copy (next.begin(), next.end(), e);
			if (!left || !changed) break;
		}
		for (int i = 0; i < n * n; i++) if (e[i] == NODATA) e[i] = 0;
	}

	bool DecodeElv (const std::vector<uint8_t> &d, ElevGrid &g, bool modify)
	{
		if (d.size() < 100 || memcmp (d.data(), "ELE\x01", 4)) return false;
		int32_t hdr, dtype, xgrd, ygrd;
		double scale, offset;
		memcpy (&hdr, d.data() + 4, 4);
		memcpy (&dtype, d.data() + 8, 4);
		memcpy (&xgrd, d.data() + 12, 4);
		memcpy (&ygrd, d.data() + 16, 4);
		memcpy (&scale, d.data() + 28, 8);
		memcpy (&offset, d.data() + 36, 8);
		const int N = ElevGrid::N;
		if (xgrd != N || ygrd != N || hdr < 100) return false;
		size_t n = (size_t)N * N;
		if (!modify) g.h.assign (n, (float)offset);
		const uint8_t *p = d.data() + hdr;
		if (dtype == 8) {
			if (d.size() < hdr + n) return false;
			for (size_t i = 0; i < n; i++)
				if (!modify || p[i] != 0xFF) g.h[i] = (float)(p[i] * scale + offset);
		} else if (dtype == -16) {
			if (d.size() < hdr + 2 * n) return false;
			std::vector<int16_t> raw (n);
			memcpy (raw.data(), p, 2 * n);
			if (!modify) FillElevationGaps (raw.data(), N);
			for (size_t i = 0; i < n; i++) {
				int16_t v = raw[i];
				if (!modify || v != 0x7FFF) g.h[i] = (float)(v * scale + offset);
			}
		} else if (dtype == 0) {
			if (modify) g.h.assign (n, (float)offset);
		} else {
			return false;
		}
		auto mm = std::minmax_element (g.h.begin(), g.h.end());
		g.emin = *mm.first, g.emax = *mm.second;
		return true;
	}

} // namespace

// ======================================================================
// TileLoader

TileLoader::TileLoader ()
{
	thread = std::thread ([this]() { Run (); });
}

TileLoader::~TileLoader ()
{
	{
		std::lock_guard<std::mutex> lock (mtx);
		quit = true;
	}
	cv.notify_all ();
	thread.join ();
}

void TileLoader::Push (Request &&req)
{
	outstanding++;
	{
		std::lock_guard<std::mutex> lock (mtx);
		queue.push_back (std::move (req));
	}
	cv.notify_one ();
}

bool TileLoader::Pop (Result &res, const PlanetLayers *owner)
{
	std::lock_guard<std::mutex> lock (mtx);
	for (auto it = done.begin(); it != done.end(); ++it) {
		if (it->req.planet.get() != owner) continue;
		res = std::move (*it);
		done.erase (it);
		outstanding--;
		return true;
	}
	return false;
}

void TileLoader::Drop (const PlanetLayers *owner)
{
	std::lock_guard<std::mutex> lock (mtx);
	for (auto it = queue.begin(); it != queue.end(); ) {
		if (it->planet.get() == owner) { it = queue.erase (it); outstanding--; }
		else ++it;
	}
	for (auto it = done.begin(); it != done.end(); ) {
		if (it->req.planet.get() == owner) { it = done.erase (it); outstanding--; }
		else ++it;
	}
}

void TileLoader::Run ()
{
	while (true) {
		Request req;
		{
			std::unique_lock<std::mutex> lock (mtx);
			cv.wait (lock, [this]() { return quit || !queue.empty(); });
			if (quit) return;
			req = std::move (queue.front());
			queue.pop_front ();
		}
		Result res;
		res.req = req;
		if (req.planet->alive) Process (req, res);
		std::lock_guard<std::mutex> lock (mtx);
		done.push_back (std::move (res));
	}
}

void TileLoader::Process (const Request &req, Result &res)
{
	PlanetLayers &pl = *req.planet;
	switch (req.kind) {
	case SURF:
		res.ok = pl.surf && pl.surf->Read (req.lvl, req.ilat, req.ilng, res.data);
		break;
	case MASK:
		res.ok = pl.mask && pl.mask->Read (req.lvl, req.ilat, req.ilng, res.data);
		break;
	case ELEV: {
		std::vector<uint8_t> d;
		res.ok = pl.elev && pl.elev->Read (req.lvl, req.ilat, req.ilng, d) && DecodeElv (d, res.elev, false);
		if (res.ok && pl.elevMod && pl.elevMod->HasTile (req.lvl, req.ilat, req.ilng) &&
			pl.elevMod->Read (req.lvl, req.ilat, req.ilng, d))
			DecodeElv (d, res.elev, true);
		break;
	}
	}
}

// ======================================================================
// TileRenderer

bool TileRenderer::Init (vkc::Context *c, TextureManager *texmgr, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout atmLayout)
{
	ctx = c;
	// meshes with the terrain detail made on the GPU (ORBITER_VK_GPU_MESH=0: on the CPU)
	bool gpuMesh = ctx->deviceAddress; // (single precision: no double precision needed)
	if (const char *e = getenv ("ORBITER_VK_GPU_MESH")) gpuMesh = gpuMesh && atoi (e) != 0;
	if (gpuMesh) {
		VkPushConstantRange mpr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 48 };
		VkPipelineLayoutCreateInfo mli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		mli.pushConstantRangeCount = 1;
		mli.pPushConstantRanges = &mpr;
		VK_CHECK(vkCreatePipelineLayout (ctx->device, &mli, nullptr, &meshLayout));
		VkShaderModule cs = ctx->CreateShaderModule (spv_tile_mesh_comp, spv_tile_mesh_comp_size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = meshLayout;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &meshPipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
		oapiWriteLog ("VulkanClient: tile meshes with the terrain detail made on the GPU");
	}
	VkDescriptorSetLayout sets[4] = { frameLayout, texmgr->Layout(), texmgr->Layout(), atmLayout };
	// (the prepass pushes two matrices and the water mask's coordinates)
	VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)std::max (sizeof(TilePushConstants), 48 * sizeof(float)) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 4;
	plci.pSetLayouts = sets;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));

	VkShaderModule vs = ctx->CreateShaderModule (spv_tile_vert, spv_tile_vert_size);
	// (with double precision: the terrain detail finer than the meshes)
	VkShaderModule fs = ctx->CreateShaderModule (spv_tile_frag, spv_tile_frag_size);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};
	// (binding 1: the amplitude of the terrain detail, after the vertices)
	VkVertexInputBindingDescription bind[2] = {
		{ 0, sizeof(NTVERTEX), VK_VERTEX_INPUT_RATE_VERTEX },
		{ 1, 5 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX }
	};
	VkVertexInputAttributeDescription attr[5] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(NTVERTEX, x) },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(NTVERTEX, nx) },
		{ 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NTVERTEX, tu) },
		{ 3, 1, VK_FORMAT_R32G32_SFLOAT, 0 },
		{ 4, 1, VK_FORMAT_R32G32B32_SFLOAT, 2 * sizeof(float) }
	};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 2;
	vi.pVertexBindingDescriptions = bind;
	vi.vertexAttributeDescriptionCount = 5;
	vi.pVertexAttributeDescriptions = attr;
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE; // terrain skirts face both ways
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = ctx->sceneSamples;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed z
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
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
	gpi.layout = layout;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline));

	// cloud layers: alpha blended, no depth writes
	ds.depthWriteEnable = VK_FALSE;
	cba.blendEnable = VK_TRUE;
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	cba.colorBlendOp = VK_BLEND_OP_ADD;
	cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.alphaBlendOp = VK_BLEND_OP_ADD;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &cloudPipeline));

	// prepass: single-sampled normal and depth, and screen motion
	VkShaderModule pvs = ctx->CreateShaderModule (spv_prepass_tile_vert, spv_prepass_tile_vert_size);
	VkShaderModule pfs = ctx->CreateShaderModule (spv_prepass_tile_frag, spv_prepass_tile_frag_size);
	stages[0].module = pvs;
	stages[1].module = pfs;
	ds.depthWriteEnable = VK_TRUE;
	cba.blendEnable = VK_FALSE;
	VkPipelineColorBlendAttachmentState cbas[AmbientOcclusion::PREPASS_TARGETS] = { cba, cba, cba };
	cb.attachmentCount = AmbientOcclusion::PREPASS_TARGETS;
	cb.pAttachments = cbas;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	rci.colorAttachmentCount = AmbientOcclusion::PREPASS_TARGETS;
	rci.pColorAttachmentFormats = AmbientOcclusion::PrepassFormats ();
	attr[3] = attr[4]; // (the morph target, not the terrain detail)
	vi.vertexAttributeDescriptionCount = 4;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &depthPipeline));
	vkDestroyShaderModule (ctx->device, pvs, nullptr);
	vkDestroyShaderModule (ctx->device, pfs, nullptr);
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyShaderModule (ctx->device, fs, nullptr);

	// shared index buffer: (G+1)^2 grid vertices, then the skirt vertices of
	// the north, south, west and east edges, (G+1) each
	const int G = GRID, V = G + 1;
	std::vector<uint16_t> idx;
	for (int i = 0; i < G; i++)
		for (int j = 0; j < G; j++) {
			uint16_t a = i*V + j, b = a + 1, c = a + V, d = c + 1;
			idx.insert (idx.end(), { a, b, c, b, d, c });
		}
	auto skirt = [&](int base, auto edgeIdx) {
		for (int k = 0; k < G; k++) {
			uint16_t e0 = edgeIdx (k), e1 = edgeIdx (k + 1);
			uint16_t s0 = base + k, s1 = base + k + 1;
			idx.insert (idx.end(), { e0, e1, s0, e1, s1, s0 });
		}
	};
	int sk = V * V;
	skirt (sk + 0*V, [&](int k) { return (uint16_t)k; });               // north row
	skirt (sk + 1*V, [&](int k) { return (uint16_t)(G*V + k); });       // south row
	skirt (sk + 2*V, [&](int k) { return (uint16_t)(k*V); });           // west column
	skirt (sk + 3*V, [&](int k) { return (uint16_t)(k*V + G); });       // east column
	nIdx = (uint32_t)idx.size();
	if (idx.size() & 1) idx.push_back (0); // (whole 32-bit words for ray tracing shaders; not drawn)
	ibuf = ctx->CreateDeviceBuffer (idx.data(), idx.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		(ctx->rayTracing ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0));

	loader = std::make_unique<TileLoader>();
	return true;
}

void TileRenderer::Shutdown ()
{
	if (!ctx) return;
	loader.reset ();
	if (meshPipe) vkDestroyPipeline (ctx->device, meshPipe, nullptr);
	if (meshLayout) vkDestroyPipelineLayout (ctx->device, meshLayout, nullptr);
	ctx->DestroyBuffer (ibuf);
	vkDestroyPipeline (ctx->device, pipeline, nullptr);
	vkDestroyPipeline (ctx->device, cloudPipeline, nullptr);
	vkDestroyPipeline (ctx->device, depthPipeline, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	ctx = nullptr;
}

// ======================================================================
// PlanetSurface

std::unique_ptr<PlanetSurface> PlanetSurface::Create (OBJHANDLE hPlanet, Scene *scene, bool clouds)
{
	const int *engine = (const int*)oapiGetObjectParam (hPlanet, clouds ? OBJPRM_PLANET_CLOUDTILEENGINE : OBJPRM_PLANET_TILEENGINE);
	// (legacy texture format, engine 1: tiles made from the patch textures,
	// for bodies with a surface; gas giants keep their textured sphere)
	if (!engine || *engine < 1 || (*engine < 2 && oapiGetMass (hPlanet) > 1e25)) return nullptr;
	if (clouds) {
		const bool *has = (const bool*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_HASCLOUDS);
		if (!has || !*has) return nullptr;
	}
	char name[256], path[512];
	oapiGetObjectName (hPlanet, name, 256);
	if (!scene->Client()->PlanetTexturePath (name, path)) return nullptr;
	std::string dir (path);
	std::replace (dir.begin(), dir.end(), '\\', '/');
	while (!dir.empty() && dir.back() == '/') dir.pop_back();

	auto layers = std::make_shared<PlanetLayers>();
	layers->surf = std::make_unique<TileLayer> (dir, clouds ? TileLayer::CLOUD : TileLayer::SURF);
	if (!layers->surf->Valid() || !layers->surf->HasTile (4, 0, 0)) return nullptr;
	if (!clouds) {
		layers->mask = std::make_unique<TileLayer> (dir, TileLayer::MASK);
		if (!layers->mask->Valid()) layers->mask.reset();
		layers->elev = std::make_unique<TileLayer> (dir, TileLayer::ELEV);
		if (!layers->elev->Valid()) layers->elev.reset();
		layers->elevMod = std::make_unique<TileLayer> (dir, TileLayer::ELEVMOD);
		if (!layers->elevMod->Valid()) layers->elevMod.reset();
	}

	std::unique_ptr<PlanetSurface> ps (new PlanetSurface (hPlanet, scene, layers, clouds));
	oapiWriteLogV ("VulkanClient: tiled %s for %s from %s%s%s", clouds ? "cloud layer" : "surface", name, dir.c_str(),
		layers->mask ? " (mask)" : "", layers->elev ? " (elevation)" : "");
	return ps;
}

PlanetSurface::PlanetSurface (OBJHANDLE h, Scene *s, std::shared_ptr<PlanetLayers> l, bool c)
	: hPlanet(h), scene(s), layers(std::move (l)), clouds(c)
{
	ctx = scene->Client()->Ctx();
	texmgr = scene->Client()->TexMgr();
	tr = scene->Tiles();
	radius = oapiGetSize (hPlanet);
	if (clouds) {
		if (const double *alt = (const double*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_CLOUDALT))
			radius += *alt;
		if (const int *ml = (const int*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_CLOUDMAXLEVEL))
			maxLevel = std::clamp (*ml, 4, 21);
	} else {
		if (const DWORD *ml = (const DWORD*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_SURFACEMAXLEVEL))
			maxLevel = std::clamp ((int)*ml, 4, 21);
		if (const double *me = (const double*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_MAXELEVATION))
			maxElev = std::max (0.0, *me);
		detail = oapiSurfaceDetailParams (hPlanet, &detailPrm);
		// (finer meshes where the detail goes)
		if (detail) maxLevel = std::clamp (std::max (maxLevel, detailPrm.level), 4, 28);
		if (const int *em = (const int*)scene->Client()->GetConfigParam (CFGPRM_ELEVATIONMODE)) elevMode = *em;
		// (PlanetResolutionBias: each step of +1 halves the tiles' screen size)
		if (const double *rb = (const double*)scene->Client()->GetConfigParam (CFGPRM_RESOLUTIONBIAS))
			splitPixels = SPLIT_PIXELS * std::exp2 (-*rb);
	}

	// The quadtree roots are loaded right away, so that the planet is never
	// rendered without textures.
	for (int i = 0; i < 2; i++) {
		root[i].reset (NewNode (4, 0, i, nullptr));
		Node *n = root[i].get();
		std::vector<uint8_t> d;
		if (layers->surf->Read (4, 0, i, d) && (n->surf = texmgr->CreateFromDDS (d.data(), d.size(), true)))
			n->surfState = 2;
		else
			n->surfState = 3;
		if (layers->mask && layers->mask->Read (4, 0, i, d) && (n->mask = texmgr->CreateFromDDS (d.data(), d.size(), true)))
			n->maskState = 2;
		ElevGrid g;
		if (layers->elev && layers->elev->Read (4, 0, i, d) && DecodeElv (d, g, false)) {
			if (layers->elevMod && layers->elevMod->Read (4, 0, i, d)) DecodeElv (d, g, true);
			n->elev = std::make_unique<ElevGrid> (std::move (g));
			n->elevState = 2;
		}
	}
}

PlanetSurface::~PlanetSurface ()
{
	layers->alive = false;
	tr->Loader().Drop (layers.get());
	for (auto &r : root) if (r) FreeNode (r.get());
}

PlanetSurface::Node *PlanetSurface::NewNode (int lvl, int ilat, int ilng, Node *parent)
{
	Node *n = new Node;
	n->lvl = lvl, n->ilat = ilat, n->ilng = ilng;
	n->parent = parent;
	int nlat = 1 << (lvl - 4), nlng = 2 << (lvl - 4);
	n->latmax = PI05 - PI * ilat / nlat;
	n->latmin = PI05 - PI * (ilat + 1) / nlat;
	n->lngmin = -PI + PI2 * ilng / nlng;
	n->lngmax = -PI + PI2 * (ilng + 1) / nlng;
	double latc = 0.5 * (n->latmin + n->latmax), lngc = 0.5 * (n->lngmin + n->lngmax);
	n->centre = SurfaceDir (latc, lngc) * radius;
	// angular radius: largest angle between the centre and the tile corners
	double a = 0.0;
	VECTOR3 c = SurfaceDir (latc, lngc);
	for (double lat : { n->latmin, n->latmax })
		for (double lng : { n->lngmin, n->lngmax })
			a = std::max (a, acos (std::clamp (dotp (c, SurfaceDir (lat, lng)), -1.0, 1.0)));
	n->angRadius = a;
	n->lastUsed = frame;
	return n;
}

void PlanetSurface::FreeNode (Node *n)
{
	for (auto &c : n->child)
		if (c) { FreeNode (c.get()); c.reset(); }
	texmgr->Release (n->surf);
	texmgr->Release (n->mask);
	n->surf = n->mask = nullptr;
	if (n->vbuf.buffer) {
		vkc::Context *c = ctx;
		vkc::Buffer b = n->vbuf;
		ctx->DeferDestroy ([c, b]() mutable { c->DestroyBuffer (b); });
		n->vbuf = vkc::Buffer();
		RTForgetGeometry (n->rtId);
		n->rtId = 0;
	}
}

void PlanetSurface::PruneChildren (Node *n)
{
	bool any = false, used = false;
	for (auto &c : n->child)
		if (c) {
			any = true;
			if (c->lastUsed + PRUNE_FRAMES > frame) used = true;
		}
	if (!any) return;
	if (!used) {
		for (auto &c : n->child)
			if (c) { FreeNode (c.get()); c.reset(); }
		return;
	}
	for (auto &c : n->child)
		if (c) PruneChildren (c.get());
}

PlanetSurface::Node *PlanetSurface::Find (int lvl, int ilat, int ilng) const
{
	if (lvl < 4) return nullptr;
	if (lvl == 4) return (ilat == 0 && ilng >= 0 && ilng < 2) ? root[ilng].get() : nullptr;
	Node *p = Find (lvl - 1, ilat / 2, ilng / 2);
	return p ? p->child[((ilat & 1) << 1) | (ilng & 1)].get() : nullptr;
}

const PlanetSurface::Node *PlanetSurface::ElevSource (const Node *n) const
{
	for (; n; n = n->parent)
		if (n->elev) return n;
	return nullptr;
}

double PlanetSurface::SampleElev (const Node *src, double lat, double lng) const
{
	const int N = ElevGrid::N;
	double fx = (lng - src->lngmin) / (src->lngmax - src->lngmin) * (N - 3) + 1.0;
	double fy = (lat - src->latmin) / (src->latmax - src->latmin) * (N - 3) + 1.0;
	fx = std::clamp (fx, 0.0, N - 1.000001);
	fy = std::clamp (fy, 0.0, N - 1.000001);
	int x0 = (int)fx, y0 = (int)fy;
	double wx = fx - x0, wy = fy - y0;
	if (elevMode == 2) {
		// cubic spline (Catmull-Rom), as the core interpolates (ElevationManager)
		x0 = std::clamp (x0, 1, N - 3), y0 = std::clamp (y0, 1, N - 3);
		wx = std::clamp (fx - x0, 0.0, 1.0), wy = std::clamp (fy - y0, 0.0, 1.0);
		auto cr = [](double t, double am1, double a0, double ap1, double ap2) {
			return 0.5 * (2.0*a0 + t*(-am1+ap1) + t*t*(2.0*am1-5.0*a0+4.0*ap1-ap2) + t*t*t*(-am1+3.0*a0-3.0*ap1+ap2));
		};
		const float *h = src->elev->h.data() + (size_t)y0 * N + x0;
		double b[4];
		for (int r = -1; r <= 2; r++) {
			const float *row = h + r * N;
			b[r + 1] = cr (wx, row[-1], row[0], row[1], row[2]);
		}
		return cr (wy, b[0], b[1], b[2], b[3]);
	}
	const float *h = src->elev->h.data() + (size_t)y0 * N + x0;
	double e0 = h[0] * (1.0 - wx) + h[1] * wx;
	double e1 = h[N] * (1.0 - wx) + h[N + 1] * wx;
	return e0 * (1.0 - wy) + e1 * wy;
}

double PlanetSurface::SurfaceElev (const Node *src, double lat, double lng, double lmin, double *amp, double *fade) const
{
	if (amp) *amp = 0.0;
	if (fade) *fade = 0.0;
	if (!src) { // (no elevation data: flat, with the detail as the core has it)
		if (clouds || !detail || layers->elev) return 0.0;
		return oapiSurfaceDetail (hPlanet, lng, lat, 0.0, detailPrm.flatSpacing, 0.0, lmin, amp, fade);
	}
	double h = SampleElev (src, lat, lng);
	if (clouds || !detail) return h;
	// the relief of the data cell (as the core computes it for its elevation)
	const int N = ElevGrid::N;
	double fx = (lng - src->lngmin) / (src->lngmax - src->lngmin) * (N - 3) + 1.0;
	double fy = (lat - src->latmin) / (src->latmax - src->latmin) * (N - 3) + 1.0;
	int x0 = std::clamp ((int)fx, 1, N - 3), y0 = std::clamp ((int)fy, 1, N - 3);
	double wx = std::clamp (fx - x0, 0.0, 1.0), wy = std::clamp (fy - y0, 0.0, 1.0);
	const float *e = src->elev->h.data() + (size_t)y0 * N + x0;
	auto node = [&](const float *p) {
		float r = 0.0f;
		for (const float *q : { p - 1, p + 1, p - N, p + N }) r = std::max (r, std::fabs (*p - *q));
		return (double)r;
	};
	double r0 = node (e) * (1.0 - wx) + node (e + 1) * wx;
	double r1 = node (e + N) * (1.0 - wx) + node (e + N + 1) * wx;
	double relief = r0 * (1.0 - wy) + r1 * wy;
	double spacing = radius * (src->latmax - src->latmin) / (N - 3);
	return h + oapiSurfaceDetail (hPlanet, lng, lat, h, spacing, relief, lmin, amp, fade);
}

void PlanetSurface::BuildMesh (Node *n, const Node *src, bool detailed)
{
	MeshData md;
	ComputeMesh (n, src, detailed, md);
	ApplyMesh (n, md);
}

void PlanetSurface::ComputeMesh (const Node *n, const Node *src, bool detailed, MeshData &md) const
{
	const int G = TileRenderer::GRID, V = G + 1, W = G + 3; // W: grid with a one-node border
	double dlat = (n->latmax - n->latmin) / G, dlng = (n->lngmax - n->lngmin) / G;
	std::vector<VECTOR3> P ((size_t)W * W), D ((size_t)W * W);
	std::vector<float> A ((size_t)W * W * 2, 0.0f); // noise amplitude and crater fade of the detail (the shader adds the finer octaves)
	// detail down to a few node spacings of this mesh (or none: the shader
	// shades all of it)
	double lmin = detailed ? 4.0 * radius * (n->latmax - n->latmin) / G : 1e30;
	for (int i = 0; i < W; i++) {
		double lat = std::clamp (n->latmax - (i - 1) * dlat, -PI05, PI05);
		for (int j = 0; j < W; j++) {
			double lng = n->lngmin + (j - 1) * dlng;
			VECTOR3 d = SurfaceDir (lat, lng);
			double a = 0.0, f = 0.0;
			double h = SurfaceElev (src, lat, lng, lmin, &a, &f);
			A[2 * (i*W + j)] = (float)a, A[2 * (i*W + j) + 1] = (float)f;
			D[i*W + j] = d;
			P[i*W + j] = d * (radius + h);
		}
	}
	std::vector<NTVERTEX> vtx ((size_t)V * V + 4 * V);
	auto at = [&](int i, int j) -> const VECTOR3 & { return P[(i + 1) * W + (j + 1)]; };
	for (int i = 0; i < V; i++)
		for (int j = 0; j < V; j++) {
			VECTOR3 dir = D[(i + 1) * W + (j + 1)];
			VECTOR3 nm = crossp (at (i + 1, j) - at (i - 1, j), at (i, j + 1) - at (i, j - 1));
			double len = length (nm);
			nm = len > 1e-9 ? nm / len : dir;
			if (dotp (nm, dir) < 0.0) nm = -nm;
			VECTOR3 p = at (i, j) - n->centre;
			NTVERTEX &v = vtx[i*V + j];
			v.x = (float)p.x, v.y = (float)p.y, v.z = (float)p.z;
			v.nx = (float)nm.x, v.ny = (float)nm.y, v.nz = (float)nm.z;
			v.tu = (float)j / G, v.tv = (float)i / G;
		}
	// Morph targets (continuous level of detail): where each vertex is on
	// the parent tile's mesh (its elevation source and detail band; the
	// vertices between the parent's on its triangles' edges). The vertex
	// shader blends to them with the distance, so that a tile matches its
	// coarser neighbours and changes smoothly to its parent's shape.
	std::vector<VECTOR3> T ((size_t)V * V);
	const Node *psrc = n->parent ? ElevSource (n->parent) : nullptr;
	if (n->parent && !clouds) {
		double lminP = detailed ? 2.0 * lmin : 1e30;
		for (int i = 0; i <= G; i += 2)
			for (int j = 0; j <= G; j += 2) {
				double lat = std::clamp (n->latmax - i * dlat, -PI05, PI05), lng = n->lngmin + j * dlng;
				T[i*V + j] = D[(i + 1) * W + (j + 1)] * (radius + SurfaceElev (psrc, lat, lng, lminP));
			}
		for (int i = 0; i <= G; i++)
			for (int j = 0; j <= G; j++) {
				if (!(i & 1) && !(j & 1)) continue;
				if ((i & 1) && (j & 1))  // (the parent's quads are split from top right to bottom left)
					T[i*V + j] = (T[(i - 1)*V + j + 1] + T[(i + 1)*V + j - 1]) * 0.5;
				else if (i & 1) T[i*V + j] = (T[(i - 1)*V + j] + T[(i + 1)*V + j]) * 0.5;
				else            T[i*V + j] = (T[i*V + j - 1] + T[i*V + j + 1]) * 0.5;
			}
	} else {
		for (int i = 0; i <= G; i++)
			for (int j = 0; j <= G; j++) T[i*V + j] = at (i, j);
	}
	// skirts hang below the tile edges to hide cracks between resolution levels
	double depth = radius * (n->latmax - n->latmin) * 0.02;
	auto skirt = [&](int base, int k, int i, int j) {
		NTVERTEX v = vtx[i*V + j];
		VECTOR3 d = D[(i + 1) * W + (j + 1)];
		v.x -= (float)(d.x * depth), v.y -= (float)(d.y * depth), v.z -= (float)(d.z * depth);
		vtx[V*V + base + k] = v;
	};
	for (int k = 0; k < V; k++) {
		skirt (0*V, k, 0, k);
		skirt (1*V, k, G, k);
		skirt (2*V, k, k, 0);
		skirt (3*V, k, k, G);
	}

	// per-vertex data after the vertices (vertex binding 1): noise
	// amplitude and crater fade of the terrain detail, morph target
	// (relative to the tile centre)
	const int NE = 5;
	std::vector<float> amp (vtx.size() * NE);
	for (int i = 0; i < V; i++)
		for (int j = 0; j < V; j++) {
			size_t a = (size_t)(i + 1) * W + (j + 1);
			float *e = &amp[NE * (i*V + j)];
			VECTOR3 t = T[i*V + j] - n->centre;
			e[0] = A[2*a], e[1] = A[2*a + 1];
			e[2] = (float)t.x, e[3] = (float)t.y, e[4] = (float)t.z;
		}
	auto copy = [&](int base, int k, int i, int j) {
		float *e = &amp[NE * (V*V + base + k)];
		memcpy (e, &amp[NE * (i*V + j)], NE * sizeof(float));
		VECTOR3 d = D[(i + 1) * W + (j + 1)];
		e[2] -= (float)(d.x * depth), e[3] -= (float)(d.y * depth), e[4] -= (float)(d.z * depth);
	};
	for (int k = 0; k < V; k++) {
		copy (0*V, k, 0, k);
		copy (1*V, k, G, k);
		copy (2*V, k, k, 0);
		copy (3*V, k, k, G);
	}
	md.detailSpacing = src ? (float)(radius * (src->latmax - src->latmin) / (ElevGrid::N - 3))
		: (detail && !layers->elev ? (float)detailPrm.flatSpacing : 0.0f);
	double lmesh = std::max (lmin, detailPrm.lambdaMin);
	md.detailLmesh = (lmesh >= md.detailSpacing) ? 1e30f : (float)lmesh; // (none in the mesh)
	md.elevLvl = src ? src->lvl : 0;
	md.detailed = detailed;
	md.vtx = std::move (vtx);
	md.amp = std::move (amp);
}

void PlanetSurface::ApplyMesh (Node *n, MeshData &md)
{
	std::vector<NTVERTEX> &vtx = md.vtx;
	std::vector<float> &amp = md.amp;
	n->detailSpacing = md.detailSpacing;
	n->detailLmesh = md.detailLmesh;
	VkDeviceSize vsize = vtx.size() * sizeof(NTVERTEX);
	EnsureVertexBuffer (n);
	ctx->UpdateBuffer (n->vbuf.buffer, 0, vtx.data(), vsize);
	ctx->UpdateBuffer (n->vbuf.buffer, vsize, amp.data(), amp.size() * sizeof(float));
	n->nVtx = (uint32_t)vtx.size();
	n->rtVersion++;
	n->meshElevLvl = md.elevLvl;
	n->meshDetailed = md.detailed;
}

void PlanetSurface::EnsureVertexBuffer (Node *n)
{
	if (n->vbuf.buffer) return;
	const int V = TileRenderer::GRID + 1;
	const size_t nv = (size_t)V * V + 4 * V;
	VkDeviceSize size = nv * (sizeof(NTVERTEX) + 5 * sizeof(float));
	n->vbuf = ctx->CreateBuffer (size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		(ctx->rayTracing ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0) |
		(ctx->rayTracing || ctx->deviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0), false);
	n->rtId = RTNewGeometryId ();
}

void PlanetSurface::SplitPoint (const VECTOR3 &D, int32_t I[3], float F[3]) const
{
	for (int k = 0; k < 3; k++) {
		double fl = std::floor (D.data[k]);
		I[k] = (int32_t)fl;
		F[k] = (float)(D.data[k] - fl);
	}
}

void PlanetSurface::PrepareGpuMesh (const Node *n, const Node *src, GpuJob &job, std::vector<float> &pts, MeshData &md) const
{
	const int G = TileRenderer::GRID, V = G + 1, W = G + 3, M = G / 2 + 1;
	double dlat = (n->latmax - n->latmin) / G, dlng = (n->lngmax - n->lngmin) / G;
	// (as ComputeMesh: the data's elevation, the detail's amplitude and
	// fade, and its coarse layers, in double precision: wavelengths from
	// lsplit, 40000 node spacings; the GPU adds the finer ones)
	double lmin = 4.0 * radius * (n->latmax - n->latmin) / G;
	double lsplit = std::max ({ 2.0 * lmin, detailPrm.lambdaMin, 1e4 * lmin });
	pts.resize ((size_t)(W * W + M * M) * 3);
	// (elevations relative to the tile's reference elevation href, so that
	// their single precision rounding is far below the node spacing)
	const float href = (float)SurfaceElev (src, 0.5 * (n->latmin + n->latmax), 0.5 * (n->lngmin + n->lngmax), lsplit);
	for (int i = 0; i < W; i++) {
		double lat = std::clamp (n->latmax - (i - 1) * dlat, -PI05, PI05);
		for (int j = 0; j < W; j++) {
			double lng = n->lngmin + (j - 1) * dlng;
			double a = 0.0, f = 0.0;
			float *p = &pts[3 * (i * W + j)];
			p[0] = (float)(SurfaceElev (src, lat, lng, lsplit, &a, &f) - (double)href);
			p[1] = (float)a, p[2] = (float)f;
		}
	}
	const Node *psrc = n->parent ? ElevSource (n->parent) : nullptr;
	if (n->parent)
		for (int i = 0; i < M; i++) {
			double lat = std::clamp (n->latmax - 2 * i * dlat, -PI05, PI05);
			for (int j = 0; j < M; j++) {
				double a = 0.0, f = 0.0;
				float *p = &pts[3 * (W * W + i * M + j)];
				p[0] = (float)(SurfaceElev (psrc, lat, n->lngmin + 2 * j * dlng, lsplit, &a, &f) - (double)href);
				p[1] = (float)a, p[2] = (float)f;
			}
		}
	auto spacingOf = [&](const Node *s) {
		return s ? radius * (s->latmax - s->latmin) / (ElevGrid::N - 3) : (detail && !layers->elev ? detailPrm.flatSpacing : 0.0);
	};
	// (the centre: d0 R; offsets from it in single precision)
	double latc = 0.5 * (n->latmin + n->latmax), lngc = 0.5 * (n->lngmin + n->lngmax);
	SplitPoint (n->centre, job.refI, job.refF);
	job.refI[3] = 0, job.refF[3] = (float)radius;
	job.ll[0] = (float)latc, job.ll[1] = (float)lngc, job.ll[2] = (float)dlat, job.ll[3] = (float)dlng;
	job.off[0] = (float)(n->latmax - latc), job.off[1] = (float)(n->lngmin - lngc);
	job.off[2] = (float)(PI05 - latc), job.off[3] = (float)(-PI05 - latc);
	VECTOR3 d0 = n->centre / radius;
	job.d0[0] = (float)d0.x, job.d0[1] = (float)d0.y, job.d0[2] = (float)d0.z, job.d0[3] = 0.0f;
	job.sp[0] = (float)std::max (lmin, detailPrm.lambdaMin);
	job.sp[1] = (float)std::max (2.0 * lmin, detailPrm.lambdaMin);
	job.sp[2] = (float)spacingOf (src), job.sp[3] = (float)spacingOf (psrc);
	job.misc[0] = (float)(radius * (n->latmax - n->latmin) * 0.02);
	job.misc[1] = n->parent ? 1.0f : 0.0f;
	job.misc[2] = (float)lsplit, job.misc[3] = href;
	job.outAddr = job.ptsAddr = 0;
	md.detailSpacing = (float)spacingOf (src);
	double lmesh = std::max (lmin, detailPrm.lambdaMin);
	md.detailLmesh = (lmesh >= md.detailSpacing) ? 1e30f : (float)lmesh;
	md.elevLvl = src ? src->lvl : 0;
	md.detailed = true;
}

void PlanetSurface::Request (Node *n, TileLoader::Kind kind)
{
	uint8_t &state = kind == TileLoader::SURF ? n->surfState : kind == TileLoader::MASK ? n->maskState : n->elevState;
	if (state) return;
	TileLayer *layer = kind == TileLoader::SURF ? layers->surf.get() : kind == TileLoader::MASK ? layers->mask.get() : layers->elev.get();
	if (!layer || !layer->HasTile (n->lvl, n->ilat, n->ilng)) { state = 3; return; }
	if (tr->Loader().Outstanding() >= MAX_OUTSTANDING) return;
	state = 1;
	tr->Loader().Push ({ layers, kind, n->lvl, n->ilat, n->ilng });
}

void PlanetSurface::Update ()
{
	TileLoader::Result res;
	int n = 0;
	while (n < MAX_UPLOADS && tr->Loader().Pop (res, layers.get())) {
		Node *node = Find (res.req.lvl, res.req.ilat, res.req.ilng);
		if (!node) continue; // released in the meantime
		switch (res.req.kind) {
		case TileLoader::SURF:
			node->surf = res.ok ? texmgr->CreateFromDDS (res.data.data(), res.data.size(), true) : nullptr;
			node->surfState = node->surf ? 2 : 3;
			n++;
			break;
		case TileLoader::MASK:
			node->mask = res.ok ? texmgr->CreateFromDDS (res.data.data(), res.data.size(), true) : nullptr;
			node->maskState = node->mask ? 2 : 3;
			n++;
			break;
		case TileLoader::ELEV:
			if (res.ok) node->elev = std::make_unique<ElevGrid> (std::move (res.elev));
			node->elevState = res.ok ? 2 : 3;
			break;
		}
	}
}

void PlanetSurface::Select (Node *n)
{
	n->lastUsed = frame;

	// horizon culling: compare the angle between the tile and the camera
	// direction with the horizon angle (extended by the highest mountains)
	if (camDist > radius) {
		double horizon = acos (radius / camDist) + acos (radius / (radius + maxElev + 1.0));
		double ang = acos (std::clamp (dotp (n->centre / radius, camLocal / camDist), -1.0, 1.0));
		if (ang - n->angRadius > horizon) return;
	}

	// Distance: a lower bound for the points of the tile, between the
	// lowest and highest elevation of its data (the closest point of a
	// spherical shell segment, the tile approximated by a cap). The morph of
	// the continuous level of detail relies on it being a lower bound.
	double emin = 0.0, emax = 0.0;
	if (!clouds) {
		if (const Node *src = ElevSource (n)) emin = src->elev->emin, emax = src->elev->emax;
		else emin = -maxElev, emax = maxElev;
	}
	auto lowerDist = [&](const VECTOR3 &p) {
		double pr = length (p);
		double cang = dotp (n->centre / radius, p / pr);
		double gap = std::max (0.0, acos (std::clamp (cang, -1.0, 1.0)) - n->angRadius);
		double r = std::clamp (pr * cos (gap), radius + emin, radius + emax);
		return std::max (sqrt (std::max (0.0, pr * pr + r * r - 2.0 * pr * r * cos (gap))), 1.0);
	};
	double dist = lowerDist (camLocal);
	double size = radius * (n->latmax - n->latmin);
	bool split = size / dist * pixPerRad > splitPixels;
	// Vessels on or near the ground stand on the finest mesh, which has the
	// terrain detail of the ground contact, wherever the camera is: tiles
	// are also split as if seen from the nearest one (graded, so the morph
	// joins the levels)
	n->vessel = -1;
	double dv = 1e30;
	for (size_t i = 0; i < groundVessels.size(); i++) {
		double d = lowerDist (groundVessels[i]);
		if (d < dv) dv = d, n->vessel = (int)i;
	}
	if (n->vessel >= 0 && size / dv * pixPerRad > splitPixels) split = true;
	if (n->lvl < maxLevel && split) {
		for (int c = 0; c < 4; c++) {
			if (!n->child[c])
				n->child[c].reset (NewNode (n->lvl + 1, 2 * n->ilat + (c >> 1), 2 * n->ilng + (c & 1), n));
			Select (n->child[c].get());
		}
	} else {
		drawList.push_back (n);
	}
}

void PlanetSurface::Prepare ()
{
	if (preparedFrame == scene->FrameNo ()) return;
	preparedFrame = scene->FrameNo ();
	frame++;
	oapiGetGlobalPos (hPlanet, &ppos);
	oapiGetRotationMatrix (hPlanet, &prot);
	if (clouds) {
		// the cloud layer rotates about the planet axis relative to the surface
		const double *rot = (const double*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_CLOUDROTATION);
		double a = rot ? *rot : 0.0, ca = cos (a), sa = sin (a);
		MATRIX3 Ry = { ca, 0, -sa,  0, 1, 0,  sa, 0, ca };
		prot = mul (prot, Ry);
	}
	camLocal = tmul (prot, scene->CameraPos() - ppos);
	camDist = length (camLocal);
	if (!std::isfinite (camDist)) return; // (no camera: keep the last selection)
	pixPerRad = scene->PixPerRad ();


	// vessels near the ground (directions from the planet centre), close
	// enough to the camera that a mismatch of a few metres would show
	// (farther away, the finest meshes would only alias)
	groundVessels.clear ();
	if (!clouds) {
		for (DWORD i = 0; i < oapiGetVesselCount (); i++) {
			OBJHANDLE hv = oapiGetVesselByIndex (i);
			VECTOR3 gv;
			oapiGetGlobalPos (hv, &gv);
			VECTOR3 lv = tmul (prot, gv - ppos);
			double r = length (lv);
			if (r < radius + maxElev + 1000.0 && length (lv - camLocal) < 3000.0)
				groundVessels.push_back (lv);
		}
	}

	drawList.clear ();
	for (auto &r : root) Select (r.get());
	for (auto &r : root) PruneChildren (r.get());

	// nearest tiles first, so that they get their data first
	std::sort (drawList.begin(), drawList.end(), [this](Node *a, Node *b) {
		return length (camLocal - a->centre) < length (camLocal - b->centre);
	});

	// data requests and mesh updates (transfers, outside the rendering scope)
	// Tiles without data of their own use their nearest ancestor's, so the
	// request goes to the deepest tile up the tree that has data.
	auto requestUp = [this](Node *n, TileLoader::Kind kind) {
		for (Node *a = n; a; a = a->parent) {
			uint8_t &st = kind == TileLoader::SURF ? a->surfState : a->maskState;
			if (st == 0) Request (a, kind);
			if (st != 3) break; // loaded, pending, or deferred (too many requests)
		}
	};
	static const unsigned nThreads = std::max (1u, std::thread::hardware_concurrency () - 1);
	const size_t maxJobs = 2 * nThreads;
	const bool gpu = tr->MeshPipeline () != VK_NULL_HANDLE;
	const size_t maxGpuJobs = 96;
	std::vector<std::pair<Node*, const Node*>> jobs;
	for (Node *n : drawList) {
		requestUp (n, TileLoader::SURF);
		if (layers->mask) requestUp (n, TileLoader::MASK);
		if (layers->elev) {
			// the mesh grid is sampled from a tile some levels up, which has
			// the same node spacing as the mesh (256 vs GRID intervals)
			const int up = (int)std::lround (std::log2 (256.0 / TileRenderer::GRID));
			Node *e = n;
			while (e->lvl > 4 && e->lvl > n->lvl - up) e = e->parent;
			// (where the data ends higher up, the deepest tile that has it, as
			// the core uses: otherwise the mesh falls back to the root)
			while (e->lvl > 4 && !layers->elev->HasTile (e->lvl, e->ilat, e->ilng)) e = e->parent;
			Request (e, TileLoader::ELEV);
		}
		const Node *src = ElevSource (n);
		int srcLvl = src ? src->lvl : 0;
		// Meshes with the terrain detail take long to build: a number per
		// frame (the nearest first) on worker threads; others get a plain mesh
		// at once and the detail later
		bool rebuild = !n->vbuf.buffer || n->meshElevLvl != srcLvl;
		bool withDetail = detail && !clouds && (src || !layers->elev);
		if (withDetail && gpu && (rebuild || !n->meshDetailed) && jobs.size() < maxGpuJobs) {
			jobs.push_back ({ n, src });
		} else if (withDetail && !gpu && (rebuild || !n->meshDetailed) && jobs.size() < maxJobs) {
			jobs.push_back ({ n, src });
		} else if (rebuild) {
			auto t0 = std::chrono::steady_clock::now ();
			BuildMesh (n, src, !withDetail);
			meshTime += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
			meshCount++;
		}
	}
	if (!jobs.empty() && gpu) {
		// the inputs per grid point (worker threads), then one dispatch
		oapiSurfaceDetail (hPlanet, 0.0, 0.0, 0.0, 100.0, 0.0, 0.0);
		auto t0 = std::chrono::steady_clock::now();
		std::vector<GpuJob> gj (jobs.size());
		std::vector<std::vector<float>> pts (jobs.size());
		std::vector<MeshData> md (jobs.size());
		std::atomic<size_t> next{ 0 };
		auto work = [&]() {
			for (size_t i; (i = next++) < jobs.size(); )
				PrepareGpuMesh (jobs[i].first, jobs[i].second, gj[i], pts[i], md[i]);
		};
		std::vector<std::thread> pool;
		for (unsigned t = 1; t < nThreads && t < jobs.size(); t++) pool.emplace_back (work);
		work ();
		for (auto &t : pool) t.join ();
		size_t nf = 0;
		for (auto &p : pts) nf += p.size();
		vkc::Context::Transient tp = ctx->AllocTransient (nf * sizeof(float));
		vkc::Context::Transient tj = ctx->AllocTransient (jobs.size() * sizeof(GpuJob) + 32);
		VkDeviceAddress pa = ctx->BufferAddress (tp.buffer) + tp.offset;
		VkDeviceAddress ja = ctx->BufferAddress (tj.buffer) + tj.offset;
		size_t jalign = (size_t)((16 - (ja & 15)) & 15); // (the records: 16-byte aligned)
		ja += jalign;
		size_t at = 0;
		for (size_t i = 0; i < jobs.size(); i++) {
			Node *n = jobs[i].first;
			EnsureVertexBuffer (n);
			memcpy ((float*)tp.ptr + at, pts[i].data(), pts[i].size() * sizeof(float));
			gj[i].ptsAddr = pa + at * sizeof(float);
			gj[i].outAddr = ctx->BufferAddress (n->vbuf.buffer);
			at += pts[i].size();
			n->detailSpacing = md[i].detailSpacing, n->detailLmesh = md[i].detailLmesh;
			n->nVtx = (uint32_t)((TileRenderer::GRID + 1) * (TileRenderer::GRID + 1) + 4 * (TileRenderer::GRID + 1));
			n->rtVersion++;
			n->meshElevLvl = md[i].elevLvl;
			n->meshDetailed = true;
		}
		memcpy ((uint8_t*)tj.ptr + jalign, gj.data(), gj.size() * sizeof(GpuJob));
		// (ORBITER_VK_MESH_CHECK: the first meshes also made at once into
		// host memory and compared with the CPU's)
		static const bool check = getenv ("ORBITER_VK_MESH_CHECK") && atoi (getenv ("ORBITER_VK_MESH_CHECK"));
		static int nChecked = 0;
		for (size_t i = 0; check && i < jobs.size() && nChecked < 24; i++) {
			if (jobs[i].first->lvl < 14 && nChecked % 3) continue;
			nChecked++;
			const int V = TileRenderer::GRID + 1, NV = V * V + 4 * V;
			vkc::Buffer hb = ctx->CreateBuffer ((VkDeviceSize)NV * 13 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
			vkc::Context::Transient t1 = ctx->AllocTransient (sizeof(GpuJob) + 32);
			VkDeviceAddress a1 = ctx->BufferAddress (t1.buffer) + t1.offset;
			size_t al = (size_t)((16 - (a1 & 15)) & 15);
			GpuJob one = gj[i];
			one.outAddr = ctx->BufferAddress (hb.buffer);
			memcpy ((uint8_t*)t1.ptr + al, &one, sizeof(one));
			ctx->ImmediateSubmit ([&](VkCommandBuffer c) {
				vkCmdBindPipeline (c, VK_PIPELINE_BIND_POINT_COMPUTE, tr->MeshPipeline ());
				struct { uint64_t jobs, pad; float prm[4]; float prm2[4]; } q{};
				q.jobs = a1 + al;
				q.prm[0] = (float)detailPrm.hurst, q.prm[1] = (float)detailPrm.ridge;
				q.prm[2] = (float)detailPrm.craters, q.prm[3] = (float)detailPrm.craterDepth;
				q.prm2[0] = (float)detailPrm.pebbles;
				vkCmdPushConstants (c, tr->MeshLayout (), VK_SHADER_STAGE_COMPUTE_BIT, 0, 48, &q);
				vkCmdDispatch (c, 1, 1, 1);
			});
			MeshData cpu;
			ComputeMesh (jobs[i].first, jobs[i].second, true, cpu);
			const float *g = (const float*)hb.mapped;
			double dp = 0.0, dn = 0.0, dt = 0.0, da = 0.0, relief = 0.0;
			for (int k = 0; k < NV; k++) {
				const NTVERTEX &v = cpu.vtx[k];
				const float *w = g + 8 * k;
				dp = std::max (dp, (double)std::max ({ std::fabs (v.x - w[0]), std::fabs (v.y - w[1]), std::fabs (v.z - w[2]) }));
				dn = std::max (dn, (double)std::max ({ std::fabs (v.nx - w[3]), std::fabs (v.ny - w[4]), std::fabs (v.nz - w[5]) }));
				const float *e = g + 8 * NV + 5 * k, *c = &cpu.amp[5 * k];
				da = std::max (da, (double)std::max (std::fabs (e[0] - c[0]), std::fabs (e[1] - c[1])));
				dt = std::max (dt, (double)std::max ({ std::fabs (e[2] - c[2]), std::fabs (e[3] - c[3]), std::fabs (e[4] - c[4]) }));
			}
			oapiWriteLogV ("VulkanClient: mesh check level %d: position %.4f m, normal %.5f, morph target %.4f m, detail stream %.5f",
				jobs[i].first->lvl, dp, dn, dt, da);
			ctx->DestroyBuffer (hb);
		}
		VkCommandBuffer cmd = ctx->Cmd ();
		ctx->EndTarget ();
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; // (earlier draws read the buffers: write after read)
		mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		mb.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.memoryBarrierCount = 1;
		dep.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2 (cmd, &dep);
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr->MeshPipeline ());
		struct { uint64_t jobs, pad; float prm[4]; float prm2[4]; } pc{};
		pc.jobs = ja;
		pc.prm[0] = (float)detailPrm.hurst, pc.prm[1] = (float)detailPrm.ridge;
		pc.prm[2] = (float)detailPrm.craters, pc.prm[3] = (float)detailPrm.craterDepth;
		pc.prm2[0] = (float)detailPrm.pebbles;
		vkCmdPushConstants (cmd, tr->MeshLayout (), VK_SHADER_STAGE_COMPUTE_BIT, 0, 48, &pc);
		vkCmdDispatch (cmd, (uint32_t)jobs.size(), 1, 1);
		mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT |
			(ctx->rayTracing ? VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR : 0);
		vkCmdPipelineBarrier2 (cmd, &dep);
		meshTime += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
		meshCount += (int)jobs.size();
	} else if (!jobs.empty()) {
		// (the core's terrain detail initialises itself on first use: here)
		oapiSurfaceDetail (hPlanet, 0.0, 0.0, 0.0, 100.0, 0.0, 0.0);
		std::vector<MeshData> out (jobs.size());
		std::atomic<size_t> next{ 0 };
		auto work = [&]() {
			for (size_t i; (i = next++) < jobs.size(); )
				ComputeMesh (jobs[i].first, jobs[i].second, true, out[i]);
		};
		auto t0 = std::chrono::steady_clock::now ();
		std::vector<std::thread> pool;
		for (unsigned t = 1; t < nThreads && t < jobs.size(); t++) pool.emplace_back (work);
		work ();
		for (auto &t : pool) t.join ();
		for (size_t i = 0; i < jobs.size(); i++) ApplyMesh (jobs[i].first, out[i]);
		meshTime += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
		meshCount += (int)jobs.size();
	}
	// (ORBITER_VK_TILE_DEBUG: the deepest tiles drawn and the deepest surface
	// and elevation data among them)
	static const bool debug = getenv ("ORBITER_VK_TILE_DEBUG") && atoi (getenv ("ORBITER_VK_TILE_DEBUG"));
	if (debug && frame % 120 == 0 && !drawList.empty()) {
		int maxLvl = 0, maxSurf = 0, maxElev = 0;
		for (Node *n : drawList) {
			maxLvl = std::max (maxLvl, n->lvl);
			for (const Node *a = n; a; a = a->parent) if (a->surf) { maxSurf = std::max (maxSurf, a->lvl); break; }
			if (const Node *e = ElevSource (n)) maxElev = std::max (maxElev, e->lvl);
		}
		char name[64];
		oapiGetObjectName (hPlanet, name, 64);
		oapiWriteLogV ("VulkanClient: tiles %s%s: %zu drawn, deepest level %d, surface data %d, elevation %d",
			name, clouds ? " (clouds)" : "", drawList.size(), maxLvl, maxSurf, maxElev);
		oapiWriteLogV ("VulkanClient: meshes %s: %d built in the last 120 frames, %.2f ms each", name, meshCount, meshCount ? meshTime / meshCount : 0.0);
		meshTime = 0.0, meshCount = 0;
		for (const VECTOR3 &v : groundVessels) {
			// the mesh under a vessel near the ground against the core's elevation
			double lat = asin (v.y / length (v)), lng = atan2 (v.z, v.x);
			for (Node *n : drawList) {
				if (lat < n->latmin || lat > n->latmax || lng < n->lngmin || lng > n->lngmax) continue;
				if (const Node *src = ElevSource (n)) {
					double lmin = 4.0 * radius * (n->latmax - n->latmin) / TileRenderer::GRID;
					oapiWriteLogV ("VulkanClient: elevation under a vessel: mesh %.2f m (tile level %d, data %d), core %.2f m",
						SurfaceElev (src, lat, lng, lmin), n->lvl, src->lvl, oapiSurfaceElevation (hPlanet, lng, lat));
				}
				break;
			}
		}
		if (!clouds) {
			// elevation under the camera: the mesh's against the core's
			double lat = asin (camLocal.y / camDist), lng = atan2 (camLocal.z, camLocal.x);
			for (Node *n : drawList) {
				if (lat < n->latmin || lat > n->latmax || lng < n->lngmin || lng > n->lngmax) continue;
				const Node *src = ElevSource (n);
				if (src && detail) {
					double amp;
					SurfaceElev (src, lat, lng, 0.0, &amp);
					oapiWriteLogV ("VulkanClient: terrain detail at the camera: amplitude %.1f m, data spacing %.1f m, mesh down to %.1f m",
						amp, n->detailSpacing, n->detailLmesh);
				}
				oapiWriteLogV ("VulkanClient: elevation at the camera (%s): mesh %.1f m (level %d, tile %d/%d/%d, range %.0f..%.0f), core %.1f m",
					name, SurfaceElev (src, lat, lng, 0.0), src ? src->lvl : 0, n->lvl, n->ilat, n->ilng,
					src ? src->elev->emin : 0.0, src ? src->elev->emax : 0.0, oapiSurfaceElevation (hPlanet, lng, lat));
				break;
			}
		}
	}
}

void PlanetSurface::RenderDepth ()
{
	// into the ambient occlusion prepass (bound by the caller)
	Prepare ();
	if (drawList.empty() || clouds) return;
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tr->DepthPipeline());
	VkDescriptorSet fs = scene->FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tr->Layout(), 0, 1, &fs, 0, nullptr);
	vkCmdBindIndexBuffer (cmd, tr->IndexBuffer(), 0, VK_INDEX_TYPE_UINT16);
	for (Node *n : drawList) DrawNode (n, true);
}

void PlanetSurface::RenderShadow (ShadowMap &sm)
{
	Prepare ();
	if (clouds) return;
	double R = oapiGetSize (hPlanet);
	for (Node *n : drawList) {
		if (!n->vbuf.buffer) continue;
		VECTOR3 gc = ppos + mul (prot, n->centre);
		// tile extent, with a margin for its relief
		double rad = n->angRadius * R * 1.5 + 10e3;
		if (!sm.MayCast (gc, rad)) continue;
		sm.DrawGeometry (n->vbuf.buffer, tr->IndexBuffer(), tr->IndexCount(), Mat4::FromRotTrans (prot, gc - sm.Centre()));
	}
}

void PlanetSurface::RenderCoverage (const Mat4 &toMap, VkPipelineLayout layout, const VECTOR3 &mapCentre, double mapRadius)
{
	Prepare ();
	// the tiles are in the cloud layer's frame (rotated about the axis
	// relative to the planet)
	MATRIX3 protPlanet;
	oapiGetRotationMatrix (hPlanet, &protPlanet);
	MATRIX3 R = mul (transp (protPlanet), prot);
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindIndexBuffer (cmd, tr->IndexBuffer(), 0, VK_INDEX_TYPE_UINT16);
	for (Node *n : drawList) {
		if (!n->vbuf.buffer) continue;
		// only tiles reaching the map (the projection is orthographic: tiles
		// elsewhere on the globe would land on it too)
		if (length (mul (R, n->centre) - mapCentre) > mapRadius + n->angRadius * radius * 1.5) continue;
		float uv[4];
		VkTexture *surf = nullptr;
		for (const Node *a = n; a; a = a->parent) {
			if (!a->surf) continue;
			int k = n->lvl - a->lvl;
			float sc = 1.0f / (1 << k);
			uv[0] = (n->ilng - (a->ilng << k)) * sc, uv[1] = (n->ilat - (a->ilat << k)) * sc;
			uv[2] = uv[3] = sc;
			surf = a->surf;
			break;
		}
		if (!surf) continue;
		float pc[20];
		(toMap * Mat4::FromRotTrans (R, mul (R, n->centre))).ToFloat (pc);
		memcpy (pc + 16, uv, sizeof(uv));
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &surf->set, 0, nullptr);
		vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
		VkDeviceSize ofs = 0;
		vkCmdBindVertexBuffers (cmd, 0, 1, &n->vbuf.buffer, &ofs);
		vkCmdDrawIndexed (cmd, tr->GridIndexCount(), 1, 0, 0, 0);
	}
}

void PlanetSurface::Render (Atmosphere *atm)
{
	Prepare ();
	if (drawList.empty()) return;

	ctx->BindScene (true);
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, clouds ? tr->CloudPipeline() : tr->Pipeline());
	VkDescriptorSet fs = scene->FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tr->Layout(), 0, 1, &fs, 0, nullptr);
	VkDescriptorSet as = atm->Set ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tr->Layout(), 3, 1, &as, 0, nullptr);
	vkCmdBindIndexBuffer (cmd, tr->IndexBuffer(), 0, VK_INDEX_TYPE_UINT16);
	for (Node *n : drawList) DrawNode (n);
}

void PlanetSurface::BindVertices (const Node *n)
{
	// the vertices, and the terrain detail amplitudes after them
	VkBuffer b[2] = { n->vbuf.buffer, n->vbuf.buffer };
	VkDeviceSize ofs[2] = { 0, n->nVtx * sizeof(NTVERTEX) };
	vkCmdBindVertexBuffers (ctx->Cmd(), 0, 2, b, ofs);
}

void PlanetSurface::DrawNode (Node *n, bool prepass)
{
	// texture: the tile's own, or the matching part of the nearest ancestor's
	auto texFor = [n](VkTexture *Node::*member, float uv[4]) -> VkTexture * {
		for (const Node *a = n; a; a = a->parent) {
			if (!(a->*member)) continue;
			int k = n->lvl - a->lvl;
			float s = 1.0f / (1 << k);
			uv[0] = (n->ilng - (a->ilng << k)) * s;
			uv[1] = (n->ilat - (a->ilat << k)) * s;
			uv[2] = uv[3] = s;
			return a->*member;
		}
		return nullptr;
	};
	TilePushConstants pc{};
	VkTexture *surf = texFor (&Node::surf, pc.surfUV);
	if (!surf) return;
	VkTexture *mask = layers->mask ? texFor (&Node::mask, pc.maskUV) : nullptr;
	pc.params[0] = mask ? 1.0f : 0.0f;
	pc.params[1] = 1.0f;
	pc.params[2] = clouds ? 1.0f : 0.0f;
	pc.params[3] = clouds ? scene->CloudFadeRadius () : 0.0f; // volumetric clouds take over within
	if (n->parent && !clouds) {
		// morph to the parent's shape between 0.7 and 0.9 times the distance
		// at which the parent is no longer split (where coarser neighbours
		// begin; finer ones end within 0.7), from the camera or the nearest
		// vessel on the ground (whichever is closer)
		double D = radius * (n->parent->latmax - n->parent->latmin) * pixPerRad / splitPixels;
		pc.morph[0] = (float)(0.7 * D);
		pc.morph[1] = (float)(1.0 / (0.2 * D));
	}
	if (!mask) mask = texmgr->White ();

	Mat4 mv = scene->ModelView (ppos + mul (prot, n->centre), prot);
	mv.ToFloat (pc.modelView);
	// (a moon in its planet's shadow: the share of the sun's disc seen from the tile)
	pc.morph[2] = prepass ? 1.0f : (float)scene->SunVisibility (ppos + mul (prot, n->centre), hPlanet);
	pc.morph[3] = detail && !clouds ? (float)detailPrm.pebbles : 0.0f; // (the pebbles shaded per pixel)
	if (detail && !clouds) { // (the detail's reference point, single precision: terraindetail.glsl)
		SplitPoint (n->centre, pc.refI, pc.refF);
		pc.refF[3] = (float)radius;
		const Node *es = ElevSource (n);
		double h0 = es ? 0.5 * (es->elev->emin + es->elev->emax) : 0.0;
		VECTOR3 rel = n->centre * ((radius + h0) / radius) - camLocal;
		pc.refRel[0] = (float)rel.x, pc.refRel[1] = (float)rel.y, pc.refRel[2] = (float)rel.z;
		pc.refRel[3] = (float)(radius + h0);
	}
	if (pc.morph[1] > 0.0f && n->vessel >= 0 && n->vessel < (int)groundVessels.size()) {
		VECTOR3 v = groundVessels[n->vessel] - n->centre; // (tile frame)
		pc.vessel[0] = (float)(mv.m[0] * v.x + mv.m[4] * v.y + mv.m[8] * v.z + mv.m[12]);
		pc.vessel[1] = (float)(mv.m[1] * v.x + mv.m[5] * v.y + mv.m[9] * v.z + mv.m[13]);
		pc.vessel[2] = (float)(mv.m[2] * v.x + mv.m[6] * v.y + mv.m[10] * v.z + mv.m[14]);
		pc.vessel[3] = 1.0f;
	}
	VkCommandBuffer cmd = ctx->Cmd();
	if (prepass) {
		// view-space motion of the planet frame (set by the planet's visual),
		// and the water mask
		float pp[48];
		mv.ToFloat (pp);
		memcpy (pp + 16, scene->ObjectMotion (), 16 * sizeof(float));
		memcpy (pp + 32, pc.maskUV, sizeof(pc.maskUV));
		memcpy (pp + 36, pc.params, sizeof(pc.params));
		memcpy (pp + 40, pc.morph, sizeof(pc.morph));
		memcpy (pp + 44, pc.vessel, sizeof(pc.vessel));
		vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tr->Layout(), 2, 1, &mask->set, 0, nullptr);
		if (RTScene *rt = scene->RT ()) { // into the ray tracing scene (without the skirts)
			RTScene::Material rm{};
			rm.diffuse[0] = rm.diffuse[1] = rm.diffuse[2] = rm.diffuse[3] = 1.0f;
			rm.light = 1.0f;
			rm.pbr[1] = 1.0f, rm.pbr[3] = 1.0f; // matte
			memcpy (rm.uv, pc.surfUV, sizeof(rm.uv));
			rm.unlit = false;
			RTScene::Geometry geo{ n->rtId, n->rtVersion, n->vbuf.buffer, 0, n->nVtx, tr->IndexBuffer(), 0, tr->GridIndexCount() };
			rt->AddInstance (geo, mv, surf, rm);
		}
		vkCmdPushConstants (cmd, tr->Layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), pp);
		BindVertices (n);
		vkCmdDrawIndexed (cmd, tr->IndexCount(), 1, 0, 0, 0);
		return;
	}
	VkDescriptorSet sets[2] = { surf->set, mask->set };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tr->Layout(), 1, 2, sets, 0, nullptr);
	if (detail && !clouds && n->detailSpacing > 0.0f) {
		pc.detail[0] = n->detailSpacing;
		pc.detail[1] = n->detailLmesh;
		pc.detail[2] = (float)detailPrm.hurst;
		pc.detail[3] = 1.0f;
	}
	vkCmdPushConstants (cmd, tr->Layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	BindVertices (n);
	// skirts would show as bright seams in the blended cloud layer
	vkCmdDrawIndexed (cmd, clouds ? tr->GridIndexCount() : tr->IndexCount(), 1, 0, 0, 0);
}

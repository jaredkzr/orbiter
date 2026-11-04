// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "PlanetSurface.h"
#include "ProcSurface.h"
#include "PolarElevation.h"
#include <functional>
#include "GroundTracks.h"
#include "tile_mesh.comp.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "RayTracing.h"
#include "Scene.h"
#include "VkTexture.h"
#include "VulkanClient.h"
#include "Atmosphere.h"
#include <algorithm>
#include <array>
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
		float tracks[4];  // ground tracks on the tile: first entry, count (GroundTracks)
	};

	constexpr double SPLIT_PIXELS = 500.0;  // split tiles that cover more than this on screen (at resolution bias 0; GRID quads per edge)
	constexpr size_t MAX_OUTSTANDING = 24;  // tile requests in flight
	constexpr int MAX_UPLOADS = 12;         // tiles accepted per frame
	constexpr uint64_t PRUNE_FRAMES = 120;  // unused subtrees are released after this
	constexpr size_t MESH_SPARE = 1024;     // meshes kept beyond the selection (at least; else half its count)
	constexpr double FOV_OUT = 40.0 * 3.14159265358979 / 180.0; // out of view, tiles split at most as in a view of this (vertical) aperture
	constexpr double MOTION_TIME = 0.05;    // [s] tiles the camera crosses faster than this (at the time warp) not split
	constexpr double VESSEL_TILE = 10.0;    // [m] the finest tiles under a vessel on the ground,
	constexpr double VESSEL_CAM = 0.1;      // at least this part of the camera's distance,
	constexpr double VESSEL_ALT = 1.0;      // and of its height above the ground

	// Worker threads kept for the meshes (they were made and joined twice
	// a frame)
	class Workers {
	public:
		static Workers &Get () { static Workers w; return w; }
		/// fn (i) for each i < n, on the workers and the calling thread;
		/// returns when all are done
		void Run (size_t n, const std::function<void (size_t)> &fn)
		{
			if (n < 2 || threads.empty ()) { for (size_t i = 0; i < n; i++) fn (i); return; }
			std::lock_guard<std::mutex> one (runMtx);
			{
				std::lock_guard<std::mutex> lk (m);
				job = &fn, count = n, next = 0, busy = threads.size (), gen++;
			}
			cv.notify_all ();
			for (size_t i; (i = next++) < n; ) fn (i);
			std::unique_lock<std::mutex> lk (m);
			done.wait (lk, [this] { return busy == 0; });
			job = nullptr;
		}
		~Workers ()
		{
			{ std::lock_guard<std::mutex> lk (m); quit = true, gen++; }
			cv.notify_all ();
			for (auto &t : threads) t.join ();
		}
	private:
		Workers ()
		{
			unsigned n = std::max (2u, std::thread::hardware_concurrency ()) - 2; // (and the caller's)
			for (unsigned i = 0; i < n; i++) threads.emplace_back ([this] { Loop (); });
		}
		void Loop ()
		{
			uint64_t seen = 0;
			for (;;) {
				const std::function<void (size_t)> *fn;
				size_t n;
				{
					std::unique_lock<std::mutex> lk (m);
					cv.wait (lk, [&] { return gen != seen; });
					seen = gen;
					if (quit) return;
					fn = job, n = count;
				}
				for (size_t i; (i = next++) < n; ) (*fn) (i);
				std::lock_guard<std::mutex> lk (m);
				if (--busy == 0) done.notify_one ();
			}
		}
		std::vector<std::thread> threads;
		std::mutex m, runMtx;
		std::condition_variable cv, done;
		const std::function<void (size_t)> *job = nullptr;
		size_t count = 0, busy = 0;
		std::atomic<size_t> next{ 0 };
		uint64_t gen = 0;
		bool quit = false;
	};

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
		if (res.ok) oapi::PolarElevationRows (res.elev.h.data(), ElevGrid::N, req.lvl, req.ilat); // (as the core: towards the poles, rows resampled)
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
	// the north, south, west and east edges, (G+1) each. Variants take every
	// 2^c-th column (tiles narrowed by a pole: their columns would be far
	// closer together than their rows), each the grid, then its skirts; their
	// edges keep all columns (fans to the next row), so that they meet
	// their neighbours' (and their children's morph) whatever these take.
	const int G = GRID, V = G + 1;
	std::vector<uint16_t> idx;
	auto skirt = [&](int base, auto edgeIdx, int step) {
		for (int k = 0; k < G; k += step) {
			uint16_t e0 = edgeIdx (k), e1 = edgeIdx (k + step);
			uint16_t s0 = base + k, s1 = base + k + step;
			idx.insert (idx.end(), { e0, e1, s0, e1, s1, s0 });
		}
	};
	const int sk = V * V;
	auto north = [&](int k) { return (uint16_t)k; };
	auto south = [&](int k) { return (uint16_t)(G*V + k); };
	auto west = [&](int k) { return (uint16_t)(k*V); };
	auto east = [&](int k) { return (uint16_t)(k*V + G); };
	for (int v = 0; v < NCOLS; v++) {
		const int K = 1 << v;
		Grid &g = grids[v];
		g.first = (uint32_t)idx.size();
		for (int i = 0; i < G; i++)
			for (int j = 0; j < G; j += K) {
				uint16_t a = i*V + j, b = a + K, c = a + V, d = c + K;
				if (K == 1 || (i > 0 && i < G - 1)) { idx.insert (idx.end(), { a, b, c, b, d, c }); continue; }
				const int h = K / 2;
				if (i == 0) { // (all columns above)
					for (int t = j; t < j + K; t++) idx.insert (idx.end(), { (uint16_t)t, (uint16_t)(t + 1), t < j + h ? c : d });
					idx.insert (idx.end(), { (uint16_t)(j + h), d, c });
				} else {      // (all columns below)
					for (int t = j; t < j + K; t++) idx.insert (idx.end(), { t < j + h ? a : b, (uint16_t)(c + t - j + 1), (uint16_t)(c + t - j) });
					idx.insert (idx.end(), { a, b, (uint16_t)(c + h) });
				}
			}
		g.gridCount = (uint32_t)idx.size() - g.first;
		skirt (sk + 0*V, north, 1);
		skirt (sk + 1*V, south, 1);
		skirt (sk + 2*V, west, 1);
		skirt (sk + 3*V, east, 1);
		g.count = (uint32_t)idx.size() - g.first;
	}
	// thinned for the view: cells of Kr rows and Kc columns; a cell on the
	// tile's edge takes all the edge's nodes, a fan from a corner of its
	// that is on none of those edges (cells are at most a quarter of the
	// tile: there is one), the polygon counter-clockwise in (j, i) as the
	// quads' triangles
	for (int v = 0; v < NCOLS; v++)
		for (int r = 0; r < NDEC; r++)
			for (int q = 0; q < NDEC; q++) {
				if (!r && !q) continue;
				const int Kr = 1 << r, Kc = 1 << (v + q);
				if (Kc > G / 4) continue;
				Grid &g = thinned[v][r][q];
				g.first = (uint32_t)idx.size();
				for (int i0 = 0; i0 < G; i0 += Kr)
					for (int j0 = 0; j0 < G; j0 += Kc) {
						const int i1 = i0 + Kr, j1 = j0 + Kc;
						bool top = i0 == 0 && Kc > 1, bottom = i1 == G && Kc > 1, left = j0 == 0 && Kr > 1, right = j1 == G && Kr > 1;
						auto at = [&](int i, int j) { return (uint16_t)(i*V + j); };
						if (!top && !bottom && !left && !right) {
							uint16_t a = at (i0, j0), b = at (i0, j1), c = at (i1, j0), d = at (i1, j1);
							idx.insert (idx.end(), { a, b, c, b, d, c });
							continue;
						}
						std::vector<uint16_t> poly; // (corners: TL 0, TR, BR, BL; their positions in poly)
						int corner[4];
						corner[0] = (int)poly.size(); poly.push_back (at (i0, j0));
						if (top) for (int j = j0 + 1; j < j1; j++) poly.push_back (at (i0, j));
						corner[1] = (int)poly.size(); poly.push_back (at (i0, j1));
						if (right) for (int i = i0 + 1; i < i1; i++) poly.push_back (at (i, j1));
						corner[2] = (int)poly.size(); poly.push_back (at (i1, j1));
						if (bottom) for (int j = j1 - 1; j > j0; j--) poly.push_back (at (i1, j));
						corner[3] = (int)poly.size(); poly.push_back (at (i1, j0));
						if (left) for (int i = i1 - 1; i > i0; i--) poly.push_back (at (i, j0));
						// (corner k is on the edges before and after it)
						const bool onEdge[4] = { top || left, top || right, right || bottom, bottom || left };
						int s = 0;
						while (s < 4 && onEdge[s]) s++;
						const int n = (int)poly.size(), p0 = corner[s];
						for (int k = 1; k < n - 1; k++)
							idx.insert (idx.end(), { poly[p0], poly[(p0 + k) % n], poly[(p0 + k + 1) % n] });
					}
				g.gridCount = (uint32_t)idx.size() - g.first;
				skirt (sk + 0*V, north, 1);
				skirt (sk + 1*V, south, 1);
				skirt (sk + 2*V, west, 1);
				skirt (sk + 3*V, east, 1);
				g.count = (uint32_t)idx.size() - g.first;
			}
	// a coarse version (every 4th node and skirt vertex) for the environment
	// probe's small faces
	const int C = 4;
	coarseFirst = (uint32_t)idx.size();
	for (int i = 0; i < G; i += C)
		for (int j = 0; j < G; j += C) {
			uint16_t a = i*V + j, b = a + C, c = a + C*V, d = c + C;
			idx.insert (idx.end(), { a, b, c, b, d, c });
		}
	skirt (sk + 0*V, north, C);
	skirt (sk + 1*V, south, C);
	skirt (sk + 2*V, west, C);
	skirt (sk + 3*V, east, C);
	coarseCount = (uint32_t)idx.size() - coarseFirst;
	// the ray tracing scene's grids: every 2nd row and column (of the
	// variant: at most every 64th), no skirts (a quarter of the triangles)
	for (int v = 0; v < NCOLS; v++) {
		const int K = std::min (G, 2 << v);
		Grid &g = grids[v];
		if (idx.size() & 1) idx.push_back (0); // (whole 32-bit words for ray tracing shaders; not drawn)
		g.rtFirst = (uint32_t)idx.size();
		for (int i = 0; i < G; i += 2)
			for (int j = 0; j < G; j += K) {
				uint16_t a = i*V + j, b = a + K, c = a + 2*V, d = c + K;
				idx.insert (idx.end(), { a, b, c, b, d, c });
			}
		g.rtCount = (uint32_t)idx.size() - g.rtFirst;
	}
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
	// a procedural surface (the exoplanets): its tiles made on demand
	if (clouds) {
		auto proc = ProcPlanet::Create (hPlanet);
		if (proc && !proc->Gas ()) {
			auto layers = std::make_shared<PlanetLayers>();
			layers->surf = std::make_unique<TileLayer> (proc, TileLayer::CLOUD);
			oapiWriteLogV ("VulkanClient: procedural cloud layer for %s", name);
			return std::unique_ptr<PlanetSurface> (new PlanetSurface (hPlanet, scene, layers, clouds));
		}
	} else {
		if (auto proc = ProcPlanet::Create (hPlanet)) {
			if (proc->Gas ()) return nullptr;
			auto layers = std::make_shared<PlanetLayers>();
			layers->surf = std::make_unique<TileLayer> (proc, TileLayer::SURF);
			if (proc->HasMask ()) layers->mask = std::make_unique<TileLayer> (proc, TileLayer::MASK);
			layers->elev = std::make_unique<TileLayer> (proc, TileLayer::ELEV);
			static const char *cls[] = { "rock", "ocean", "ice", "desert", "lava", "gas" };
			oapiWriteLogV ("VulkanClient: procedural surface for %s (%s world, tiles to level %d, elevation to %d)", name,
				cls[std::clamp (proc->Prm ().surfClass, 0, 5)], proc->MaxTexLevel (), proc->MaxElevLevel ());
			return std::unique_ptr<PlanetSurface> (new PlanetSurface (hPlanet, scene, layers, clouds));
		}
	}
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
		if (layers->surf->Read (4, 0, i, d) && (n->surf = texmgr->CreateFromDDS (d.data(), d.size(), true, true)))
			n->surfState = 2;
		else
			n->surfState = 3;
		if (layers->mask && layers->mask->Read (4, 0, i, d) && (n->mask = texmgr->CreateFromDDS (d.data(), d.size(), true, true)))
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
	for (auto &b : errBuf)
		if (b.buffer) {
			vkc::Context *c = ctx;
			vkc::Buffer x = b;
			ctx->DeferDestroy ([c, x]() mutable { c->DestroyBuffer (x); });
		}
}

PlanetSurface::Node *PlanetSurface::NewNode (int lvl, int ilat, int ilng, Node *parent)
{
	Node *n = new Node;
	dbgNew++;
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
	n->cosLat = cos (std::min (PI05, std::max (fabs (n->latmin), fabs (n->latmax))));
	{ // (SplitSize)
		double hgt = radius * (n->latmax - n->latmin);
		double wid = radius * cos (std::min (fabs (n->latmin), fabs (n->latmax))) * (n->lngmax - n->lngmin);
		if (n->latmin < 0.0 && n->latmax > 0.0) wid = radius * (n->lngmax - n->lngmin);
		n->size = sqrt (hgt * std::min (wid, hgt));
	}
	// columns taken (TileRenderer::Grid): no closer together than the rows
	// (at the wider edge; latitude and longitude steps are the same angle)
	double ratio = n->latmin < 0.0 && n->latmax > 0.0 ? 1.0 : cos (std::min (fabs (n->latmin), fabs (n->latmax)));
	while (n->cols + 1 < TileRenderer::NCOLS && ratio * (2 << n->cols) <= 1.0) n->cols++;
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
		nMeshes--;
		dbgFreed++;
		RTForgetGeometry (n->rtId);
		n->rtId = 0;
	}
	if (n->rtCluster) RTForgetGeometry (n->rtCluster);
	n->rtCluster = 0;
}

void PlanetSurface::PruneChildren (Node *n, uint64_t age)
{
	bool any = false, used = false;
	for (auto &c : n->child)
		if (c) {
			any = true;
			if (c->lastUsed + age > frame) used = true;
		}
	if (!any) return;
	if (!used) {
		for (auto &c : n->child)
			if (c) { FreeNode (c.get()); c.reset(); }
		return;
	}
	for (auto &c : n->child)
		if (c) PruneChildren (c.get(), age);
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
	// (beyond the tile's edge nodes, e.g. the border of a mesh at the edge
	// of its source tile, from which its normals are taken: linear, from the
	// border nodes, which the spline lacks the neighbours for. Clamped to the
	// edge, the slope there was halved: a line of shading along every edge.)
	if (elevMode == 2 && fx >= 1.0 && fx <= N - 2.0 && fy >= 1.0 && fy <= N - 2.0) {
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
			// (across the columns drawn: one-sided at the edges of a tile taking
			// every K-th; finer, their noise would differ from a neighbour's)
			const int K = 1 << n->cols;
			VECTOR3 nm = K > 1 ? crossp (at (i + 1, j) - at (i - 1, j), at (i, std::min (j + K, G)) - at (i, std::max (j - K, 0)))
				: crossp (at (i + 1, j) - at (i - 1, j), at (i, j + 1) - at (i, j - 1));
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
		// (from the same data, and no detail in the mesh at either's spacing:
		// the tile's own nodes, the same values)
		double sp = src ? radius * (src->latmax - src->latmin) / (ElevGrid::N - 3) : detailPrm.flatSpacing;
		bool same = psrc == src && std::max (lmin, detailPrm.lambdaMin) >= sp;
		for (int i = 0; i <= G; i += 2)
			for (int j = 0; j <= G; j += 2) {
				double lat = std::clamp (n->latmax - i * dlat, -PI05, PI05), lng = n->lngmin + j * dlng;
				T[i*V + j] = same ? at (i, j) : D[(i + 1) * W + (j + 1)] * (radius + SurfaceElev (psrc, lat, lng, lminP));
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
	DecimationErrors (vtx.data(), n->cols, md.decErr);
	md.vtx = std::move (vtx);
	md.amp = std::move (amp);
}

void PlanetSurface::DecimationErrors (const NTVERTEX *vtx, int cols, float err[6])
{
	const int G = TileRenderer::GRID, V = G + 1, Kb = 1 << cols;
	auto p = [&](int i, int j) { const NTVERTEX &v = vtx[i*V + j]; return VECTOR3{ v.x, v.y, v.z }; };
	for (int r = 1; r <= 3; r++) {
		const int K = 1 << r;
		double e = 0.0;
		for (int i = 0; i <= G; i++) {
			if (i % K == 0) continue;
			int i0 = i - i % K;
			double u = (double)(i - i0) / K;
			for (int j = 0; j <= G; j += Kb)
				e = std::max (e, length (p (i, j) - (p (i0, j) * (1.0 - u) + p (i0 + K, j) * u)));
		}
		err[r - 1] = (float)e;
	}
	for (int q = 1; q <= 3; q++) {
		const int K = Kb << q;
		if (K > G / 4) { err[2 + q] = 1e30f; continue; }
		double e = 0.0;
		for (int j = 0; j <= G; j += Kb) {
			if (j % K == 0) continue;
			int j0 = j - j % K;
			double u = (double)(j - j0) / K;
			for (int i = 0; i <= G; i++)
				e = std::max (e, length (p (i, j) - (p (i, j0) * (1.0 - u) + p (i, j0 + K) * u)));
		}
		err[2 + q] = (float)e;
	}
}

void PlanetSurface::ChooseThinning (Node *n, double dist, double ppr) const
{
	// rows and columns foreshortened on the screen (seen at a grazing angle,
	// a tile's 64 rows fall within a few pixels: thousands of tiles towards
	// the horizon from a low camera) are thinned while their spacing stays
	// within that of a tile seen face on just before it splits (S_MAX) and
	// the mesh's deviation from the thinned one within a pixel (E_MAX;
	// relief, silhouettes). (Most of the rows are the relief's: the
	// deviations of real terrain are metres at the mesh's spacing; the
	// Atmos takeoff's ground camera: terrain 4.7 -> 3.8 ms, prepass 3.3 -> 2.7)
	constexpr double S_MAX = 8.0, E_MAX = 1.0;
	n->decR = n->decQ = 0;
	if (clouds || dist <= 0.0) return;
	const int G = TileRenderer::GRID;
	VECTOR3 c = n->centre / radius;
	VECTOR3 north = _V(0, 1, 0) - c * c.y;
	double ln = length (north);
	if (ln < 1e-6) return; // (at a pole)
	north = north / ln;
	VECTOR3 east = crossp (north, c);
	VECTOR3 v = n->centre - camLocal;
	double lv = length (v);
	if (lv <= 0.0) return;
	v = v / lv;
	double latc = 0.5 * (n->latmin + n->latmax);
	double rowLen = radius * (n->latmax - n->latmin) / G;
	double colLen = radius * cos (latc) * (n->lngmax - n->lngmin) / G * (1 << n->cols);
	double rowPx = rowLen * sqrt (std::max (0.0, 1.0 - dotp (north, v) * dotp (north, v))) / dist * ppr;
	double colPx = colLen * sqrt (std::max (0.0, 1.0 - dotp (east, v) * dotp (east, v))) / dist * ppr;
	double errPx = ppr / dist;
	int best = 0;
	for (int r = 0; r < TileRenderer::NDEC; r++)
		for (int q = 0; q < TileRenderer::NDEC; q++) {
			if (r + q <= best || (1 << (n->cols + q)) > G / 4) continue;
			if (r && rowPx * (1 << r) > S_MAX) continue;
			if (q && colPx * (1 << q) > S_MAX) continue;
			double e = (r ? n->decErr[r - 1] : 0.0) + (q ? n->decErr[2 + q] : 0.0);
			if (e * errPx > E_MAX) continue;
			best = r + q, n->decR = r, n->decQ = q;
		}
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
	memcpy (n->decErr, md.decErr, sizeof(n->decErr));
}

void PlanetSurface::EnsureVertexBuffer (Node *n)
{
	if (n->vbuf.buffer) return;
	const int V = TileRenderer::GRID + 1;
	const size_t nv = (size_t)V * V + 4 * V;
	VkDeviceSize size = nv * (sizeof(NTVERTEX) + 5 * sizeof(float));
	nMeshes++;
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
	// (from the same data, the parent's nodes are every other one of the
	// tile's, the same values: copied, a fifth of the core's evaluations)
	if (n->parent && psrc == src)
		for (int i = 0; i < M; i++)
			for (int j = 0; j < M; j++)
				memcpy (&pts[3 * (W * W + i * M + j)], &pts[3 * ((2 * i + 1) * W + 2 * j + 1)], 3 * sizeof(float));
	else if (n->parent)
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
	job.refI[3] = 1 << n->cols, job.refF[3] = (float)radius; // (the columns drawn: the normals' stencil)
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
			node->surf = res.ok ? texmgr->CreateFromDDS (res.data.data(), res.data.size(), true, true) : nullptr;
			node->surfState = node->surf ? 2 : 3;
			n++;
			break;
		case TileLoader::MASK:
			node->mask = res.ok ? texmgr->CreateFromDDS (res.data.data(), res.data.size(), true, true) : nullptr;
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

double PlanetSurface::SplitSize (const Node *n) const
{
	// From the tile's area (the mean of its sides). Towards the poles tiles
	// narrow to slivers: split by their north-south side alone, ever more of
	// them would be needed for the same ground (at 89.5 deg, 50 times as
	// many: gigabytes of meshes)
	// (made with the node: NewNode)
	return n->size;
}

PlanetSurface::From PlanetSurface::MakeFrom (const VECTOR3 &p) const
{
	From f;
	f.p = p;
	f.r = length (p);
	f.u = p / f.r;
	f.lat = asin (std::clamp (p.y / f.r, -1.0, 1.0)), f.lng = atan2 (p.z, p.x);
	f.cosLat = cos (std::min (PI05, fabs (f.lat)));
	return f;
}

double PlanetSurface::LowerDist (const Node *n, const From &f, double cang, double emin, double emax) const
{
	// A lower bound for the points of the tile, between the lowest and
	// highest elevation of its data (the closest point of a spherical shell
	// segment, the tile approximated by a cap)
	double gap = std::max (0.0, acos (std::clamp (cang, -1.0, 1.0)) - n->angRadius);
	// (and from the tile's latitude-longitude box: the cap about a tile
	// narrowed to a sliver by a pole takes in its neighbours on either
	// side, so that all of them would be split)
	double glat = std::max (0.0, std::max (n->latmin - f.lat, f.lat - n->latmax));
	double dl = f.lng - 0.5 * (n->lngmin + n->lngmax);
	dl = fabs (dl - PI2 * std::floor ((dl + PI) / PI2));
	double glng = std::max (0.0, dl - 0.5 * (n->lngmax - n->lngmin));
	double cm = std::min (n->cosLat, f.cosLat); // (cos of the larger latitude of the two)
	gap = std::max (gap, sqrt (glat * glat + glng * cm * glng * cm));
	double cg = cos (gap);
	double r = std::clamp (f.r * cg, radius + emin, radius + emax);
	return std::max (sqrt (std::max (0.0, f.r * f.r + r * r - 2.0 * f.r * r * cg)), 1.0);
}

uint32_t PlanetSurface::Select (Node *n)
{
	n->lastUsed = frame;
	n->drawn = false;
	n->hidden = true;
	n->rtTris = 0;

	// horizon culling: compare the angle between the tile and the camera
	// direction with the horizon angle (extended by the highest mountains)
	double cang = dotp (n->centre, fromCam.u) / radius;
	if (horizon >= 0.0 && acos (std::clamp (cang, -1.0, 1.0)) - n->angRadius > horizon) return 0;
	n->hidden = false;

	// Distance: a lower bound for the points of the tile, between the
	// lowest and highest elevation of its data (LowerDist). The morph of
	// the continuous level of detail relies on it being a lower bound.
	double emin = 0.0, emax = 0.0;
	if (!clouds) {
		if (const Node *src = ElevSource (n)) emin = src->elev->emin, emax = src->elev->emax;
		else emin = -maxElev, emax = maxElev;
	}
	double dist = LowerDist (n, fromCam, cang, emin, emax);
	double size = SplitSize (n);
	// (out of view, at most a FOV_OUT view's scale: the tiles around a
	// zoomed view (no frustum culling: the ray tracing's) would be split
	// as finely as those seen, all round: a 10 deg view took 26000 tiles,
	// the video memory. A tile in view has its parent in view: graded)
	double ppr = n->parent ? n->parent->pixPerRad : pixPerRad;
	if (ppr > pixPerRadOut) {
		VECTOR3 c = n->centre * ((radius + 0.5 * (emin + emax)) / radius);
		double b = (radius + emax) * n->angRadius + 0.5 * (emax - emin);
		ppr = scene->SphereInView (ppos + mul (prot, c) - scene->CameraPos (), b) ? pixPerRad : pixPerRadOut;
	}
	n->pixPerRad = ppr;
	bool split = size / dist * ppr > splitPixels && size > minSplit;
	// Vessels on or near the ground stand on the finest mesh, which has the
	// terrain detail of the ground contact, wherever the camera is: tiles
	// are also split as if seen from the nearest one (graded, so the morph
	// joins the levels)
	n->vessel = -1;
	double dv = 1e30;
	for (size_t i = 0; i < fromVessels.size(); i++) {
		double d = LowerDist (n, fromVessels[i], dotp (n->centre, fromVessels[i].u) / radius, emin, emax);
		if (d < dv) dv = d, n->vessel = (int)i;
	}
	// (down to tiles of 10 m (groundTile; coarser for a camera farther
	// away, or moving fast): mesh nodes 0.15 m apart, half the shortest
	// wavelength of the ground contact's detail, elevmgr PHYSICS_LMIN;
	// finer ones only for the camera: thousands of them, each in the ray
	// tracing scene)
	if (n->vessel >= 0 && size > groundTile[n->vessel] && size / dv * ppr > splitPixels) split = true;
	if (n->lvl < maxLevel && split) {
		for (int c = 0; c < 4; c++) {
			if (!n->child[c])
				n->child[c].reset (NewNode (n->lvl + 1, 2 * n->ilat + (c >> 1), 2 * n->ilng + (c & 1), n));
			n->rtTris += Select (n->child[c].get());
		}
	} else {
		drawList.push_back (n);
		n->drawn = true;
		n->rtTris = tr->Columns (n->cols).rtCount / 3;
		if (thinning) ChooseThinning (n, dist, ppr);
	}
	return n->rtTris;
}

void PlanetSurface::Prepare ()
{
	if (preparedFrame == scene->FrameNo ()) return;
	preparedFrame = scene->FrameNo ();
	frame++;
	scene->ObservedPos (hPlanet, ppos); // (light-time: where its light left it)
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
	pixPerRadOut = std::min (pixPerRad, 0.5 * scene->ViewHeight () / tan (0.5 * FOV_OUT));
	// (low and fast over the ground, the finest tiles about the camera
	// were all replaced every frame: a thousand new meshes a frame on a
	// takeoff run with the chase camera a metre up. No finer than the
	// camera crosses in MOTION_TIME at the time warp: 4 m tiles at 80 m/s
	// (nodes 12 cm apart; the shader adds the detail). Rises at most
	// twofold a frame: a camera's jump is no speed)
	double dt = oapiGetSimStep ();
	if (dt > 0.0) {
		double v = length (camLocal - camLast) / dt;
		camSpeed = std::min (v, 2.0 * camSpeed + 10.0);
	}
	camLast = camLocal;
	minSplit = clouds ? 0.0 : MOTION_TIME * camSpeed * oapiGetTimeAcceleration ();

	// vessels near the ground (directions from the planet centre), close
	// enough to the camera that a mismatch of a few metres would show
	// (farther away, the finest meshes would only alias)
	groundVessels.clear (), groundTile.clear ();
	if (!clouds) {
		for (DWORD i = 0; i < oapiGetVesselCount (); i++) {
			OBJHANDLE hv = oapiGetVesselByIndex (i);
			VECTOR3 gv;
			oapiGetGlobalPos (hv, &gv);
			VECTOR3 lv = tmul (prot, gv - ppos);
			double r = length (lv);
			if (r < radius + maxElev + 1000.0 && length (lv - camLocal) < 3000.0) {
				// (as fine as the ground contact's detail only for a camera
				// close by: seen from hundreds of metres, a vessel rolling
				// down the runway replaced its 10 m tiles every frame; and
				// no finer than its height above the ground: an aircraft a
				// kilometre up at 300 m/s, under the planet's highest
				// mountains, had them made beneath it, ten tiles a frame)
				double alt = 0.0;
				if (!oapiGetAltitude (hv, ALTMODE_GROUND, &alt)) alt = 0.0;
				groundVessels.push_back (lv);
				groundTile.push_back (std::max ({ VESSEL_TILE, VESSEL_CAM * length (lv - camLocal), minSplit, VESSEL_ALT * alt }));
			}
		}
	}

	fromCam = MakeFrom (camLocal);
	horizon = camDist > radius ? acos (radius / camDist) + acos (radius / (radius + maxElev + 1.0)) : -1.0;
	fromVessels.clear ();
	for (const VECTOR3 &v : groundVessels) fromVessels.push_back (MakeFrom (v));
	drawList.clear ();
	for (auto &r : root) Select (r.get());
	// (unused subtrees kept a while, for a camera coming back; but low and
	// fast over the ground the finest tiles change every frame, ~100 new
	// meshes a frame at a low frame rate: over a budget, only this frame's
	// selection is kept; it had filled the video memory within a minute)
	size_t budget = drawList.size () + std::max<size_t> (MESH_SPARE, drawList.size () / 2);
	for (auto &r : root) PruneChildren (r.get(), nMeshes > budget ? 1 : PRUNE_FRAMES);

	static const bool profileTree = getenv ("ORBITER_VK_PROFILE") && atoi (getenv ("ORBITER_VK_PROFILE"));
	if (profileTree && frame % 100 == 0) { // (what the tile tree holds)
		size_t nn = 0, nb = 0, ns = 0, nm = 0, ne = 0;
		std::function<void (const Node *)> walk = [&](const Node *n) {
			nn++; nb += n->vbuf.buffer ? 1 : 0; ns += n->surf ? 1 : 0; nm += n->mask ? 1 : 0; ne += n->elev ? 1 : 0;
			for (auto &c : n->child) if (c) walk (c.get());
		};
		for (auto &r : root) if (r) walk (r.get());
		oapiWriteLogV ("VulkanClient: %s tiles: %zu drawn, %zu nodes, %zu meshes, %zu surface, %zu mask textures, %zu elevation grids",
			clouds ? "cloud" : "surface", drawList.size(), nn, nb, ns, nm, ne);
	}
	// nearest tiles first, so that they get their data first
	{
		std::vector<std::pair<double, Node*>> by (drawList.size ());
		for (size_t i = 0; i < drawList.size (); i++) by[i] = { length (camLocal - drawList[i]->centre), drawList[i] };
		std::sort (by.begin(), by.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
		for (size_t i = 0; i < by.size (); i++) drawList[i] = by[i].second;
	}

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
	// (the decimation errors of the meshes made in this slot's last use)
	const uint32_t slot = ctx->FrameIndex ();
	if (gpu && !errBuf[slot].buffer)
		errBuf[slot] = ctx->CreateBuffer (maxGpuJobs * 8 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
	for (size_t k = 0; k < errPending[slot].size(); k++) {
		const ErrPending &e = errPending[slot][k];
		Node *n = Find (e.lvl, e.ilat, e.ilng);
		if (n && n->rtVersion == e.ver) memcpy (n->decErr, (const float*)errBuf[slot].mapped + 8 * k, sizeof(n->decErr));
	}
	errPending[slot].clear ();
	std::vector<std::pair<Node*, const Node*>> jobs;
	// (meshes made at once: a number per frame, the nearest first; each is
	// staged in memory of the frame, which a jump to a new place would
	// otherwise fill with thousands of them)
	constexpr int MAX_PLAIN_MESHES = 256;
	struct PlainJob { Node *n; const Node *src; bool detailed; };
	std::vector<PlainJob> plain;
	for (Node *n : drawList) {
		requestUp (n, TileLoader::SURF);
		if (layers->mask) requestUp (n, TileLoader::MASK);
		if (layers->elev) {
			// the mesh grid is sampled from a tile some levels up, which has
			// the same node spacing as the mesh (256 vs GRID intervals)
			if (!n->elevNode) {
				const int up = (int)std::lround (std::log2 (256.0 / TileRenderer::GRID));
				Node *e = n;
				while (e->lvl > 4 && e->lvl > n->lvl - up) e = e->parent;
				// (where the data ends higher up, the deepest tile that has it, as
				// the core uses: otherwise the mesh falls back to the root)
				while (e->lvl > 4 && !layers->elev->HasTile (e->lvl, e->ilat, e->ilng)) e = e->parent;
				n->elevNode = e; // (an ancestor: freed after n, if at all)
			}
			Request (n->elevNode, TileLoader::ELEV);
		}
		const Node *src = ElevSource (n);
		int srcLvl = src ? src->lvl : 0;
		// Meshes with the terrain detail take long to build: a number per
		// frame (the nearest first) on worker threads; others get a plain mesh
		// at once and the detail later
		bool rebuild = !n->vbuf.buffer || n->meshElevLvl != srcLvl;
		bool withDetail = detail && !clouds && (src || !layers->elev);
		if (!n->vbuf.buffer) dbgRbNew++; else if (n->meshElevLvl != srcLvl) dbgRbElev++; else if (withDetail && !n->meshDetailed) dbgDetail++;
		if (withDetail && gpu && (rebuild || !n->meshDetailed) && jobs.size() < maxGpuJobs) {
			jobs.push_back ({ n, src });
		} else if (withDetail && !gpu && (rebuild || !n->meshDetailed) && jobs.size() < maxJobs) {
			jobs.push_back ({ n, src });
		} else if (rebuild && plain.size() < MAX_PLAIN_MESHES) {
			plain.push_back ({ n, src, !withDetail });
		}
	}
	// (the tiles the probe draws, without a mesh: plain ones, a few a frame)
	for (const NodeKey &k : probeNeed) {
		if (plain.size () >= MAX_PLAIN_MESHES || plain.size () >= 4) break;
		Node *n = Find (k.lvl, k.ilat, k.ilng);
		if (!n || n->vbuf.buffer || n->drawn) continue;
		plain.push_back ({ n, ElevSource (n), false });
	}
	probeNeed.clear ();
	if (!plain.empty()) {
		// (on the worker threads too: low and fast over the ground, a hundred
		// or more a frame, ~1 ms each with the core's elevation and detail;
		// one after another, most of the frame)
		auto t0 = std::chrono::steady_clock::now ();
		if (detail && !clouds) oapiSurfaceDetail (hPlanet, 0.0, 0.0, 0.0, 100.0, 0.0, 0.0); // (initialised before the threads)
		std::vector<MeshData> out (plain.size());
		Workers::Get ().Run (plain.size(), [&](size_t i) { ComputeMesh (plain[i].n, plain[i].src, plain[i].detailed, out[i]); });
		for (size_t i = 0; i < plain.size(); i++) ApplyMesh (plain[i].n, out[i]);
		meshTime += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
		meshCount += (int)plain.size();
	}
	if (!jobs.empty() && gpu) {
		// the inputs per grid point (worker threads), then one dispatch
		oapiSurfaceDetail (hPlanet, 0.0, 0.0, 0.0, 100.0, 0.0, 0.0);
		auto t0 = std::chrono::steady_clock::now();
		std::vector<GpuJob> gj (jobs.size());
		std::vector<std::vector<float>> pts (jobs.size());
		std::vector<MeshData> md (jobs.size());
		Workers::Get ().Run (jobs.size(), [&](size_t i) { PrepareGpuMesh (jobs[i].first, jobs[i].second, gj[i], pts[i], md[i]); });
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
			gj[i].errAddr = ctx->BufferAddress (errBuf[slot].buffer) + i * 8 * sizeof(float);
			at += pts[i].size();
			n->detailSpacing = md[i].detailSpacing, n->detailLmesh = md[i].detailLmesh;
			n->nVtx = (uint32_t)((TileRenderer::GRID + 1) * (TileRenderer::GRID + 1) + 4 * (TileRenderer::GRID + 1));
			n->rtVersion++;
			n->meshElevLvl = md[i].elevLvl;
			n->meshDetailed = true;
			for (float &e : n->decErr) e = 1e30f; // (until read back)
			errPending[slot].push_back ({ n->lvl, n->ilat, n->ilng, n->rtVersion });
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
		auto t0 = std::chrono::steady_clock::now ();
		Workers::Get ().Run (jobs.size(), [&](size_t i) { ComputeMesh (jobs[i].first, jobs[i].second, true, out[i]); });
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
		oapiWriteLogV ("VulkanClient: tiles %s: %d nodes new, %d meshes freed; needing a mesh: %d new, %d elevation changed, %d detail (frame sums)", name, dbgNew, dbgFreed, dbgRbNew, dbgRbElev, dbgDetail);
		dbgNew = dbgFreed = dbgRbNew = dbgRbElev = dbgDetail = 0;
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
	size_t nv = 0;
	for (Node *n : drawList) if (InView (n)) DrawNode (n, true), nv++;
	static const bool profile = getenv ("ORBITER_VK_PROFILE") && atoi (getenv ("ORBITER_VK_PROFILE"));
	if (profile && frame % 100 == 0) oapiWriteLogV ("VulkanClient: surface tiles in view: %zu of %zu", nv, drawList.size());
	if (RTScene *rt = scene->RT ())
		for (auto &r : root) if (r) AddRTClusters (r.get(), rt);
}

void PlanetSurface::GatherDrawn (Node *n, std::vector<Node*> &out) const
{
	if (n->lastUsed != frame || !n->rtTris) return;
	if (n->drawn) { out.push_back (n); return; }
	for (auto &c : n->child) if (c) GatherDrawn (c.get(), out);
}

void PlanetSurface::AddRTClusters (Node *n, RTScene *rt)
{
	// Each tile an instance of its own: their boxes overlap (most of all the
	// slivers about a pole, all reaching it), and long rays (the sun's, low
	// over the ground) descend into hundreds of them. Instead, the tiles under
	// a node with up to RT_CLUSTER triangles between them (64 tiles of all
	// columns; more of those narrowed by a pole) are one structure, moved
	// from the node's centre (their meshes are about their own). A tile that
	// changes rebuilds its cluster's (ORBITER_VK_RT_CLUSTER: the triangles).
	// Where tiles changed within RT_CHURN frames, smaller clusters (down to
	// a sixteenth): low and fast over the ground, a few new tiles a frame
	// rebuilt clusters of a million triangles in all, 20-30 ms a frame.
	static const uint32_t RT_CLUSTER = getenv ("ORBITER_VK_RT_CLUSTER") ? std::max (1, atoi (getenv ("ORBITER_VK_RT_CLUSTER"))) : 131072;
	constexpr uint64_t RT_CHURN = 60;
	if (n->lastUsed != frame || !n->rtTris) return;
	if (n->rtTris > RT_CLUSTER && !n->drawn) {
		for (auto &c : n->child) if (c) AddRTClusters (c.get(), rt);
		return;
	}
	if (!n->drawn && n->rtTris > RT_CLUSTER / 16) {
		std::vector<Node*> under;
		GatherDrawn (n, under);
		uint64_t sig = 1469598103934665603ull;
		for (const Node *t : under)
			sig = (sig ^ (t->vbuf.buffer ? t->rtId * 0x9E3779B97F4A7C15ull + t->rtVersion : 0x5bd1e995ull)) * 1099511628211ull;
		if (sig != n->rtSig) n->rtSig = sig, n->rtChanged = frame;
		if (frame - n->rtChanged < RT_CHURN) {
			for (auto &c : n->child) if (c) AddRTClusters (c.get(), rt);
			return;
		}
	}
	std::vector<Node*> tiles;
	GatherDrawn (n, tiles);
	std::vector<RTScene::Part> parts;
	std::vector<std::array<float, 3>> ofs;
	parts.reserve (tiles.size());
	ofs.reserve (tiles.size());
	for (Node *t : tiles) {
		if (!t->vbuf.buffer) continue;
		RTScene::Part p{};
		p.tex = TexFor (t, &Node::surf, p.mat.uv);
		if (!p.tex) continue;
		p.geo = { t->rtId, t->rtVersion, t->vbuf.buffer, 0, t->nVtx, tr->IndexBuffer(), tr->Columns (t->cols).rtFirst, tr->Columns (t->cols).rtCount }; // (without the skirts)
		p.mat.diffuse[0] = p.mat.diffuse[1] = p.mat.diffuse[2] = p.mat.diffuse[3] = 1.0f;
		p.mat.light = 1.0f;
		p.mat.pbr[1] = 1.0f, p.mat.pbr[3] = 1.0f; // matte
		p.mat.unlit = false;
		parts.push_back (p);
		VECTOR3 d = t->centre - n->centre;
		ofs.push_back ({ (float)d.x, (float)d.y, (float)d.z });
	}
	if (parts.empty()) return;
	if (!n->rtCluster) n->rtCluster = RTNewGeometryId ();
	rt->AddCluster (n->rtCluster, parts.data(), reinterpret_cast<const float (*)[3]> (ofs.data()), (uint32_t)parts.size(),
		scene->ModelView (ppos + mul (prot, n->centre), prot));
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
		const TileRenderer::Grid &g = tr->Thinned (n->cols, n->decR, n->decQ);
		sm.DrawGeometry (n->vbuf.buffer, tr->IndexBuffer(), g.count, Mat4::FromRotTrans (prot, gc - sm.Centre()), g.first);
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
		vkCmdDrawIndexed (cmd, tr->Columns (n->cols).gridCount, 1, tr->Columns (n->cols).first, 0, 0);
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
	if (scene->InProbe ()) {
		// (the probe's faces, 128 pixels across 90 degrees: tiles thirty
		// times as large serve; the drawn ones, thousands in a low view,
		// took the probe's draws a tenth of the frame's time)
		probeList.clear ();
		VECTOR3 pc = tmul (prot, scene->CameraPos () - ppos);
		for (auto &r : root) ProbeCut (r.get(), pc, probeList);
		for (Node *n : probeList) if (InView (n)) DrawNode (n);
		return;
	}
	for (Node *n : drawList) if (InView (n)) DrawNode (n);
}

void PlanetSurface::ProbeCut (Node *n, const VECTOR3 &pc, std::vector<Node*> &out)
{
	if (!n || n->lastUsed != frame || n->hidden) return; // (not in this frame's tree; beyond the horizon)
	if (!n->drawn) {
		constexpr double PROBE_PPR = 64.0; // [pixels/rad] (EnvProbe::SIZE / 2: a face's 90 degrees)
		double emin = 0.0, emax = 0.0; // (a sphere about the tile, as InView)
		if (!clouds) {
			if (const Node *src = ElevSource (n)) emin = src->elev->emin, emax = src->elev->emax;
			else emin = -maxElev, emax = maxElev;
		}
		VECTOR3 c = n->centre * ((radius + 0.5 * (emin + emax)) / radius);
		double d = std::max (length (c - pc) - (radius + emax) * n->angRadius - 0.5 * (emax - emin), 1.0);
		if (SplitSize (n) / d * PROBE_PPR <= splitPixels) {
			if (n->vbuf.buffer) { out.push_back (n); return; }
			if (probeNeed.size () < 64) probeNeed.push_back ({ n->lvl, n->ilat, n->ilng });
		}
		for (auto &c : n->child) ProbeCut (c.get(), pc, out);
		return;
	}
	out.push_back (n);
}

bool PlanetSurface::InView (const Node *n) const
{
	// a sphere about the tile: its cap, raised to the highest and lowered to
	// the lowest elevation of its data (with a margin for the terrain detail
	// and the skirts)
	double emin = 0.0, emax = 0.0;
	if (!clouds) {
		if (const Node *src = ElevSource (n)) emin = src->elev->emin, emax = src->elev->emax;
		else emin = -maxElev, emax = maxElev;
	}
	double rad = n->angRadius * (radius + std::max (emax, 0.0)) + std::max (-emin, emax) + 100.0;
	return scene->SphereInView (ppos + mul (prot, n->centre) - scene->CameraPos (), rad * 1.01);
}

void PlanetSurface::BindVertices (const Node *n)
{
	// the vertices, and the terrain detail amplitudes after them
	VkBuffer b[2] = { n->vbuf.buffer, n->vbuf.buffer };
	VkDeviceSize ofs[2] = { 0, n->nVtx * sizeof(NTVERTEX) };
	vkCmdBindVertexBuffers (ctx->Cmd(), 0, 2, b, ofs);
}

VkTexture *PlanetSurface::TexFor (const Node *n, VkTexture *Node::*member, float uv[4])
{
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
}

void PlanetSurface::DrawNode (Node *n, bool prepass)
{
	if (!n->vbuf.buffer) return; // (its mesh not made yet: a number per frame)
	// texture: the tile's own, or the matching part of the nearest ancestor's
	auto texFor = [n](VkTexture *Node::*member, float uv[4]) { return TexFor (n, member, uv); };
	TilePushConstants pc{};
	VkTexture *surf = texFor (&Node::surf, pc.surfUV);
	if (!surf) return;
	VkTexture *mask = layers->mask ? texFor (&Node::mask, pc.maskUV) : nullptr;
	pc.params[0] = mask ? 1.0f : 0.0f;
	pc.params[1] = clouds ? layerAlpha : 1.0f; // (night lights' scale; a cloud layer's opacity)
	pc.params[2] = clouds ? 1.0f : 0.0f;
	pc.params[3] = clouds ? scene->CloudFadeRadius () : 0.0f; // volumetric clouds take over within
	if (n->parent && !clouds) {
		// morph to the parent's shape between 0.7 and 0.9 times the distance
		// at which the parent is no longer split (where coarser neighbours
		// begin; finer ones end within 0.7), from the camera or the nearest
		// vessel on the ground (whichever is closer)
		// (by the size the split goes by: near a pole, a sliver's length would
		// put the morph far beyond its coarser neighbours, leaving cracks)
		double D = SplitSize (n->parent) * n->parent->pixPerRad / splitPixels;
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
	if (!prepass && !clouds && !scene->InProbe ())
		if (GroundTracks *gt = scene->Tracks ()) gt->ForTile (hPlanet, ppos, prot, n->centre / radius, n->angRadius, pc.tracks);
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
		vkCmdPushConstants (cmd, tr->Layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), pp);
		BindVertices (n);
		const TileRenderer::Grid &g = tr->Thinned (n->cols, n->decR, n->decQ);
		vkCmdDrawIndexed (cmd, g.count, 1, g.first, 0, 0);
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
	if (scene->InProbe () && !clouds) vkCmdDrawIndexed (cmd, tr->CoarseIndexCount(), 1, tr->CoarseFirstIndex(), 0, 0);
	else {
		const TileRenderer::Grid &g = tr->Thinned (n->cols, n->decR, n->decQ);
		vkCmdDrawIndexed (cmd, clouds ? g.gridCount : g.count, 1, g.first, 0, 0);
	}
}

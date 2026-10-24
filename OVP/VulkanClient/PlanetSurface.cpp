// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "PlanetSurface.h"
#include "Scene.h"
#include "VkTexture.h"
#include "VulkanClient.h"
#include "Atmosphere.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "tile.vert.h"
#include "tile.frag.h"

namespace {

	// Must match the push constant block of tile.vert/frag
	struct TilePushConstants {
		float modelView[16];
		float surfUV[4];  // offset (xy) and scale (zw) into the surface texture
		float maskUV[4];  // same for the mask texture
		float params[4];  // x: mask present, y: night light intensity
	};

	constexpr double SPLIT_PIXELS = 450.0;  // split tiles that cover more than this on screen
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
			for (size_t i = 0; i < n; i++) {
				int16_t v;
				memcpy (&v, p + 2 * i, 2);
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
	VkDescriptorSetLayout sets[4] = { frameLayout, texmgr->Layout(), texmgr->Layout(), atmLayout };
	VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TilePushConstants) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 4;
	plci.pSetLayouts = sets;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));

	VkShaderModule vs = ctx->CreateShaderModule (spv_tile_vert, spv_tile_vert_size);
	VkShaderModule fs = ctx->CreateShaderModule (spv_tile_frag, spv_tile_frag_size);
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
	rs.cullMode = VK_CULL_MODE_NONE; // terrain skirts face both ways
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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
	gpi.layout = layout;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline));
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
	ibuf = ctx->CreateDeviceBuffer (idx.data(), idx.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

	loader = std::make_unique<TileLoader>();
	return true;
}

void TileRenderer::Shutdown ()
{
	if (!ctx) return;
	loader.reset ();
	ctx->DestroyBuffer (ibuf);
	vkDestroyPipeline (ctx->device, pipeline, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	ctx = nullptr;
}

// ======================================================================
// PlanetSurface

std::unique_ptr<PlanetSurface> PlanetSurface::Create (OBJHANDLE hPlanet, Scene *scene)
{
	const int *engine = (const int*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_TILEENGINE);
	if (!engine || *engine < 2) return nullptr;
	char name[256], path[512];
	oapiGetObjectName (hPlanet, name, 256);
	if (!scene->Client()->PlanetTexturePath (name, path)) return nullptr;
	std::string dir (path);
	std::replace (dir.begin(), dir.end(), '\\', '/');
	while (!dir.empty() && dir.back() == '/') dir.pop_back();

	auto layers = std::make_shared<PlanetLayers>();
	layers->surf = std::make_unique<TileLayer> (dir, TileLayer::SURF);
	if (!layers->surf->Valid() || !layers->surf->HasTile (4, 0, 0)) return nullptr;
	layers->mask = std::make_unique<TileLayer> (dir, TileLayer::MASK);
	if (!layers->mask->Valid()) layers->mask.reset();
	layers->elev = std::make_unique<TileLayer> (dir, TileLayer::ELEV);
	if (!layers->elev->Valid()) layers->elev.reset();
	layers->elevMod = std::make_unique<TileLayer> (dir, TileLayer::ELEVMOD);
	if (!layers->elevMod->Valid()) layers->elevMod.reset();

	std::unique_ptr<PlanetSurface> ps (new PlanetSurface (hPlanet, scene, layers));
	oapiWriteLogV ("VulkanClient: tiled surface for %s from %s%s%s", name, dir.c_str(),
		layers->mask ? " (mask)" : "", layers->elev ? " (elevation)" : "");
	return ps;
}

PlanetSurface::PlanetSurface (OBJHANDLE h, Scene *s, std::shared_ptr<PlanetLayers> l)
	: hPlanet(h), scene(s), layers(std::move (l))
{
	ctx = scene->Client()->Ctx();
	texmgr = scene->Client()->TexMgr();
	tr = scene->Tiles();
	radius = oapiGetSize (hPlanet);
	if (const DWORD *ml = (const DWORD*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_SURFACEMAXLEVEL))
		maxLevel = std::clamp ((int)*ml, 4, 21);
	if (const double *me = (const double*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_MAXELEVATION))
		maxElev = std::max (0.0, *me);

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
	const float *h = src->elev->h.data() + (size_t)y0 * N + x0;
	double e0 = h[0] * (1.0 - wx) + h[1] * wx;
	double e1 = h[N] * (1.0 - wx) + h[N + 1] * wx;
	return e0 * (1.0 - wy) + e1 * wy;
}

void PlanetSurface::BuildMesh (Node *n, const Node *src)
{
	const int G = TileRenderer::GRID, V = G + 1, W = G + 3; // W: grid with a one-node border
	double dlat = (n->latmax - n->latmin) / G, dlng = (n->lngmax - n->lngmin) / G;
	std::vector<VECTOR3> P ((size_t)W * W), D ((size_t)W * W);
	for (int i = 0; i < W; i++) {
		double lat = std::clamp (n->latmax - (i - 1) * dlat, -PI05, PI05);
		for (int j = 0; j < W; j++) {
			double lng = n->lngmin + (j - 1) * dlng;
			VECTOR3 d = SurfaceDir (lat, lng);
			double h = src ? SampleElev (src, lat, lng) : 0.0;
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

	VkDeviceSize size = vtx.size() * sizeof(NTVERTEX);
	if (!n->vbuf.buffer)
		n->vbuf = ctx->CreateBuffer (size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	ctx->UpdateBuffer (n->vbuf.buffer, 0, vtx.data(), size);
	n->meshElevLvl = src ? src->lvl : 0;
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

	double dist = std::max (length (camLocal - n->centre) - radius * n->angRadius, 1.0);
	double size = radius * (n->latmax - n->latmin);
	if (n->lvl < maxLevel && size / dist * pixPerRad > SPLIT_PIXELS) {
		for (int c = 0; c < 4; c++) {
			if (!n->child[c])
				n->child[c].reset (NewNode (n->lvl + 1, 2 * n->ilat + (c >> 1), 2 * n->ilng + (c & 1), n));
			Select (n->child[c].get());
		}
	} else {
		drawList.push_back (n);
	}
}

void PlanetSurface::Render (Atmosphere *atm)
{
	frame++;
	oapiGetGlobalPos (hPlanet, &ppos);
	oapiGetRotationMatrix (hPlanet, &prot);
	camLocal = tmul (prot, scene->CameraPos() - ppos);
	camDist = length (camLocal);
	pixPerRad = scene->PixPerRad ();

	drawList.clear ();
	for (auto &r : root) Select (r.get());
	for (auto &r : root) PruneChildren (r.get());

	// nearest tiles first, so that they get their data first
	std::sort (drawList.begin(), drawList.end(), [this](Node *a, Node *b) {
		return length (camLocal - a->centre) < length (camLocal - b->centre);
	});

	// data requests and mesh updates (transfers, outside the rendering scope)
	for (Node *n : drawList) {
		Request (n, TileLoader::SURF);
		if (layers->mask) Request (n, TileLoader::MASK);
		if (layers->elev) {
			// the mesh grid is sampled from a tile three levels up, which has
			// the same node spacing as the mesh (256 vs 32 intervals)
			Node *e = n;
			while (e->lvl > 4 && e->lvl > n->lvl - 3) e = e->parent;
			Request (e, TileLoader::ELEV);
		}
		const Node *src = ElevSource (n);
		int srcLvl = src ? src->lvl : 0;
		if (!n->vbuf.buffer || n->meshElevLvl != srcLvl) BuildMesh (n, src);
	}
	if (drawList.empty()) return;

	ctx->BindBackbuffer (true);
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tr->Pipeline());
	VkDescriptorSet fs = scene->FrameSet ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tr->Layout(), 0, 1, &fs, 0, nullptr);
	VkDescriptorSet as = atm->Set ();
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tr->Layout(), 3, 1, &as, 0, nullptr);
	vkCmdBindIndexBuffer (cmd, tr->IndexBuffer(), 0, VK_INDEX_TYPE_UINT16);
	for (Node *n : drawList) DrawNode (n);
}

void PlanetSurface::DrawNode (Node *n)
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
	if (!mask) mask = texmgr->White ();

	Mat4 mv = scene->ModelView (ppos + mul (prot, n->centre), prot);
	mv.ToFloat (pc.modelView);
	VkCommandBuffer cmd = ctx->Cmd();
	VkDescriptorSet sets[2] = { surf->set, mask->set };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tr->Layout(), 1, 2, sets, 0, nullptr);
	vkCmdPushConstants (cmd, tr->Layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	VkDeviceSize ofs = 0;
	vkCmdBindVertexBuffers (cmd, 0, 1, &n->vbuf.buffer, &ofs);
	vkCmdDrawIndexed (cmd, tr->IndexCount(), 1, 0, 0, 0);
}

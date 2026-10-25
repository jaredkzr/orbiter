// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ScatterField.h"
#include "Scene.h"
#include "Shadows.h"
#include "VulkanClient.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace vkscatter {

namespace {

	const double SHADOW_RANGE = 300.0; // [m] rocks casting into the shadow maps
	const double SHADOW_RANGE_PLANT = 150.0; // [m] plants (many, alpha-tested)
	const double RT_RANGE = 150.0;     // [m] rocks in the ray tracing scene
	const double RT_RANGE_PLANT = 60.0; // [m] plants (alpha-tested foliage: slow to trace)
	const size_t RT_MAX = 2000;
	// levels of detail: by size on screen [pixels]
	inline int Lod (double px, size_t nLods)
	{
		int l = px > 400.0 ? 0 : px > 120.0 ? 1 : px > 30.0 ? 2 : 3;
		return std::min (l, (int)nLods);
	}
	const double FETCH_BUDGET = 0.002; // [s] reading cells per frame (more while nothing is there yet)

	inline uint64_t Key (int layer, int row, int col)
	{
		return ((uint64_t)layer << 56) | ((uint64_t)(uint32_t)row << 28) | (uint64_t)(uint32_t)col;
	}

} // namespace

Field::Field (Scene *s, OBJHANDLE hp): scene (s), hPlanet (hp)
{
	R = oapiGetSize (hPlanet);
	int na = oapiScatterAssetCount (hPlanet);
	if (!na) return;
	char name[64];
	oapiGetObjectName (hPlanet, name, 64);

	bool plants = false;
	std::vector<SCATTERASSET> ca (na);
	for (int i = 0; i < na; i++) {
		oapiScatterAsset (hPlanet, i, &ca[i]);
		if (ca[i].kind != SCATTER_ROCK) plants = true;
	}
	RockStyle style = !strcmp (name, "Mars") ? RockStyle::MARS : !strcmp (name, "Earth") ? RockStyle::GRANITE : RockStyle::REGOLITH;
	lib = std::make_unique<Library> (scene->Client()->Ctx(), scene->Client()->TexMgr());
	lib->Generate (name, style, plants);

	// the core's assets: meshes by name (rocks: mesh stems; plants: names)
	kinds.resize (na);
	int drawn = 0;
	for (int i = 0; i < na; i++) {
		Kind &k = kinds[i];
		k.kind = ca[i].kind;
		k.size = std::max (2.0 * ca[i].radius, ca[i].height);
		for (const Asset &a : lib->Assets())
			if (a.name == ca[i].name) { k.asset = &a; break; }
		if (!k.asset) {
			oapiWriteLogV ("VulkanClient: ground scatter of %s: no mesh for %s", name, ca[i].name);
			continue;
		}
		// (plants: the height the core gives them, which their trunks have)
		if (k.kind != SCATTER_ROCK && k.asset->height > 0.0) k.scale = ca[i].height / k.asset->height;
		k.size = std::max (2.0 * k.asset->radius, k.asset->height) * k.scale; // (as drawn: crowns may be wider)
		drawn++;
	}
	int nl = oapiScatterLayerCount (hPlanet);
	layers.resize (nl);
	for (int l = 0; l < nl; l++) {
		oapiScatterLayer (hPlanet, l, &layers[l]);
		maxRange = std::max (maxRange, layers[l].range);
	}
	active = drawn > 0 && nl > 0;
	oapiWriteLogV ("VulkanClient: ground scatter of %s: %d of %d assets, %d layers", name, drawn, na, nl);
}

Field::~Field ()
{
	vkc::Context *ctx = scene->Client()->Ctx();
	for (auto &b : buf)
		if (b.buffer) {
			vkc::Buffer bb = b;
			ctx->DeferDestroy ([ctx, bb]() mutable { ctx->DestroyBuffer (bb); });
		}
}

VECTOR3 Field::CellCentre (int layer, int row, int col) const
{
	// (the core's grid, GroundScatter.h)
	double cell = layers[layer].cell;
	int nr = std::max (1, (int)(PI * R / cell));
	double lat = -PI05 + (row + 0.5) * PI / nr;
	int nc = std::max (1, (int)(PI2 * R * cos (lat) / cell));
	double lng = -PI + (col + 0.5) * PI2 / nc;
	return _V(cos (lat) * cos (lng), sin (lat), cos (lat) * sin (lng)) * R;
}

void Field::Fetch (int layer, int row, int col, Cell &c)
{
	c.layer = layer;
	int n = oapiScatterCell (hPlanet, layer, row, col, nullptr, 0);
	std::vector<SCATTERINSTANCE> in (n);
	if (n) oapiScatterCell (hPlanet, layer, row, col, in.data(), n);
	c.inst.clear();
	VECTOR3 lo = _V(1e30, 1e30, 1e30), hi = _V(-1e30, -1e30, -1e30);
	for (const SCATTERINSTANCE &s : in) {
		if (s.asset < 0 || s.asset >= (int)kinds.size() || !kinds[s.asset].asset) continue;
		const Kind &k = kinds[s.asset];
		Inst x;
		x.pos = s.pos;
		double f = s.scale * k.scale;
		const double *m = s.rot.data;
		for (int i = 0; i < 9; i++) x.R[i] = (float)(m[i] * f);
		x.size = (float)(k.size * s.scale);
		x.kind = (uint16_t)s.asset;
		for (int i = 0; i < 3; i++) lo.data[i] = std::min (lo.data[i], s.pos.data[i]), hi.data[i] = std::max (hi.data[i], s.pos.data[i]);
		c.inst.push_back (x);
	}
	if (c.inst.empty()) {
		c.centre = CellCentre (layer, row, col);
		c.radius = 0.0;
		return;
	}
	c.centre = (lo + hi) * 0.5;
	c.radius = 0.0;
	for (const Inst &x : c.inst) c.radius = std::max (c.radius, length (x.pos - c.centre) + x.size);
}

void Field::Prepare ()
{
	batches.clear(); shadowBatches.clear(); rtInst.clear();
	if (!active) return;
	frame++;
	VECTOR3 cam = scene->CameraPos (), P;
	oapiGlobalToLocal (hPlanet, &cam, &P);
	double alt = length (P) - R;
	if (alt > maxRange + 10e3) { // (far above: nothing to draw; cells kept a while)
		if (!cells.empty() && alt > maxRange + 100e3) cells.clear();
		return;
	}
	unsigned gen = oapiScatterGeneration (hPlanet);
	if (gen != generation) cells.clear(), generation = gen;

	// cells needed: kept ones marked, missing ones read nearest first
	struct Missing { int layer, row, col; double d; };
	std::vector<Missing> missing;
	std::vector<int> cl;
	for (int l = 0; l < (int)layers.size(); l++) {
		int n = oapiScatterCells (hPlanet, l, P, layers[l].range, nullptr, 0);
		cl.resize (2 * n);
		oapiScatterCells (hPlanet, l, P, layers[l].range, cl.data(), n);
		for (int i = 0; i < n; i++) {
			auto it = cells.find (Key (l, cl[2*i], cl[2*i+1]));
			if (it != cells.end()) it->second.used = frame;
			else missing.push_back ({ l, cl[2*i], cl[2*i+1], length (CellCentre (l, cl[2*i], cl[2*i+1]) - P) / layers[l].range });
		}
	}
	std::sort (missing.begin(), missing.end(), [](const Missing &a, const Missing &b) { return a.d < b.d; });
	auto t0 = std::chrono::steady_clock::now();
	double budget = cells.empty() ? 0.05 : FETCH_BUDGET;
	for (const Missing &m : missing) {
		Cell &c = cells[Key (m.layer, m.row, m.col)];
		Fetch (m.layer, m.row, m.col, c);
		c.used = frame;
		if (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() > budget) break;
	}
	for (auto it = cells.begin(); it != cells.end(); )
		if (frame - it->second.used > 120) it = cells.erase (it); else ++it;

	// the instances: in view (drawn), near (shadows), nearer (ray tracing)
	MATRIX3 Rp;
	oapiGetRotationMatrix (hPlanet, &Rp);
	VECTOR3 gp;
	oapiGetGlobalPos (hPlanet, &gp);
	origin = cam;
	VECTOR3 d0 = gp - cam;
	size_t nk = kinds.size();
	std::vector<std::vector<float>> bucket (nk * 4), sbucket (nk * 4);
	for (auto &kv : cells) {
		Cell &c = kv.second;
		if (c.used != frame || c.inst.empty()) continue;
		const SCATTERLAYER &L = layers[c.layer];
		VECTOR3 rc = mul (Rp, c.centre) + d0;
		double dc = length (rc);
		if (dc - c.radius > L.range) continue;
		bool inView = scene->SphereInView (rc, c.radius);
		bool shadow = L.kind != SCATTER_GRASS && dc - c.radius < SHADOW_RANGE;
		if (!inView && !shadow) continue;
		for (const Inst &x : c.inst) {
			VECTOR3 rel = mul (Rp, x.pos) + d0;
			double d = length (rel);
			if (d > L.range) continue;
			const Kind &k = kinds[x.kind];
			// (shrunk away over the last 15% of the range)
			double f = std::min (1.0, (L.range - d) / (0.15 * L.range));
			double px = scene->PixelSize (x.size * f, d);
			if (px < 1.0) continue;
			int lod = Lod (px, k.asset->lods.size());
			// object -> global frame relative to the camera (rows)
			float row[12];
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++)
					row[i*4+j] = (float)((Rp.data[i*3] * x.R[j] + Rp.data[i*3+1] * x.R[3+j] + Rp.data[i*3+2] * x.R[6+j]) * f);
				row[i*4+3] = (float)rel.data[i];
			}
			if (inView) bucket[x.kind * 4 + lod].insert (bucket[x.kind * 4 + lod].end(), row, row + 12);
			if (shadow && d < (L.kind == SCATTER_ROCK ? SHADOW_RANGE : SHADOW_RANGE_PLANT)) { // (a level coarser)
				int sl = std::min (lod + 1, (int)k.asset->lods.size());
				sbucket[x.kind * 4 + sl].insert (sbucket[x.kind * 4 + sl].end(), row, row + 12);
			}
			if (inView && d < (L.kind == SCATTER_ROCK ? RT_RANGE : RT_RANGE_PLANT) && L.kind != SCATTER_GRASS && rtInst.size() < RT_MAX) {
				// (the mesh drawn: rays start on its surface)
				const VkMesh *m = lod ? k.asset->lods[lod - 1].get() : k.asset->mesh.get();
				Mat4 M;
				for (int i = 0; i < 3; i++) {
					for (int j = 0; j < 3; j++) M.m[j*4+i] = row[i*4+j];
					M.m[12+i] = row[i*4+3];
				}
				M.m[3] = M.m[7] = M.m[11] = 0.0, M.m[15] = 1.0;
				rtInst.push_back ({ m, M });
			}
		}
	}

	// upload: the batches of this frame
	size_t total = 0;
	for (auto &b : bucket) total += b.size();
	for (auto &b : sbucket) total += b.size();
	if (!total) return;
	vkc::Context *ctx = scene->Client()->Ctx();
	vkc::Buffer &B = buf[ctx->FrameIndex() % vkc::FRAMES_IN_FLIGHT];
	VkDeviceSize bytes = total * sizeof(float);
	if (B.size < bytes) {
		if (B.buffer) {
			vkc::Buffer old = B;
			ctx->DeferDestroy ([ctx, old]() mutable { ctx->DestroyBuffer (old); });
		}
		B = ctx->CreateBuffer (std::max<VkDeviceSize> (bytes * 3 / 2, 1 << 20), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
	}
	float *dst = (float*)B.mapped;
	uint32_t at = 0; // (instances)
	auto emit = [&](std::vector<std::vector<float>> &bk, std::vector<Batch> &out) {
		for (size_t i = 0; i < bk.size(); i++) {
			if (bk[i].empty()) continue;
			const Kind &k = kinds[i / 4];
			int lod = (int)(i % 4);
			const VkMesh *m = lod ? k.asset->lods[lod - 1].get() : k.asset->mesh.get();
			uint32_t n = (uint32_t)(bk[i].size() / 12);
			memcpy (dst + (size_t)at * 12, bk[i].data(), bk[i].size() * sizeof(float));
			out.push_back ({ m, at, n });
			at += n;
		}
	};
	emit (bucket, batches);
	emit (sbucket, shadowBatches);

	// (ORBITER_VK_PROFILE: instances and triangles drawn)
	static const bool profile = getenv ("ORBITER_VK_PROFILE") && atoi (getenv ("ORBITER_VK_PROFILE"));
	if (profile && frame % 240 == 0) {
		uint64_t tri[4] = {}, ninst[4] = {}, stri = 0;
		for (const Batch &b : batches) {
			uint64_t t = 0;
			for (uint32_t g = 0; g < b.mesh->nGroup(); g++) t += b.mesh->GetGroup (g).nIndex / 3;
			int kk = 0;
			for (size_t i = 0; i < kinds.size(); i++) {
				const Asset *a = kinds[i].asset;
				if (!a) continue;
				bool mine = a->mesh.get() == b.mesh;
				for (auto &l : a->lods) mine |= l.get() == b.mesh;
				if (mine) { kk = kinds[i].kind; break; }
			}
			tri[kk] += t * b.count, ninst[kk] += b.count;
		}
		for (const Batch &b : shadowBatches)
			for (uint32_t g = 0; g < b.mesh->nGroup(); g++) stri += (uint64_t)b.mesh->GetGroup (g).nIndex / 3 * b.count;
		oapiWriteLogV ("VulkanClient: ground scatter: %zu cells; drawn rocks %llu (%.2fM triangles), grass %llu (%.2fM), shrubs %llu (%.2fM), trees %llu (%.2fM); shadows %.2fM; ray traced %zu",
			cells.size(), (unsigned long long)ninst[0], tri[0] * 1e-6, (unsigned long long)ninst[1], tri[1] * 1e-6,
			(unsigned long long)ninst[2], tri[2] * 1e-6, (unsigned long long)ninst[3], tri[3] * 1e-6, stri * 1e-6, rtInst.size());
	}
}

void Field::Render (RenderPass pass)
{
	if (!active || pass == RenderPass::Transparent || scene->InProbe ()) return;
	vkc::Context *ctx = scene->Client()->Ctx();
	const vkc::Buffer &B = buf[ctx->FrameIndex() % vkc::FRAMES_IN_FLIGHT];
	for (const Batch &b : batches)
		scene->DrawMeshInstanced (b.mesh, B.buffer, (VkDeviceSize)b.first * Scene::INSTANCE_STRIDE, b.count, pass);
	if (pass == RenderPass::Depth)
		for (const RTInst &r : rtInst) scene->AddMeshRT (r.mesh, scene->View () * r.M); // (the view of this pass)
}

void Field::RenderShadow (ShadowMap &sm)
{
	if (!active || shadowBatches.empty()) return;
	if (length (sm.Centre() - origin) > sm.Radius() + SHADOW_RANGE) return;
	vkc::Context *ctx = scene->Client()->Ctx();
	const vkc::Buffer &B = buf[ctx->FrameIndex() % vkc::FRAMES_IN_FLIGHT];
	for (const Batch &b : shadowBatches)
		sm.DrawMeshInstanced (b.mesh, B.buffer, (VkDeviceSize)b.first * Scene::INSTANCE_STRIDE, b.count, origin);
}

} // namespace vkscatter

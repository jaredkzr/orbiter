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
	const double RT_RANGE_PLANT = 260.0; // [m] plants (alpha-tested foliage: slow to trace)
	const size_t RT_MAX = 4000;
	// levels of detail: by size on screen [pixels]
	const int SLOTS = 5; // per asset: levels of detail 0 to 3, impostor
	const double FAR_DIST = 700.0;     // [m] far set: instances beyond this distance of its anchor
	const double FAR_MOVE = 150.0;     // [m] new far set when the camera has moved this far from the anchor
	const size_t FAR_NEWCELLS = 64;    // ... or this many candidate cells have come since,
	const uint64_t FAR_MIN_FRAMES = 180; // ... and this many frames have passed since the last one
	const double FAR_BUDGET = 0.0015;  // [s] building the far set per frame
	const double RT_PROXY_MIN = 100.0;  // [m] plants with impostors from this distance of the far set's anchor ...
	const double RT_PROXY_MAX = 1500.0; // ... to this: stand-ins in the ray tracing scene (the near ones are meshes)
	const double FAR_THIN = 1500.0;    // [m] beyond: a share FAR_THIN/d of the far plants, wider (canopy kept, less overdraw)
	inline int Lod (double px, size_t nLods)
	{
		int l = px > 400.0 ? 0 : px > 120.0 ? 1 : px > 30.0 ? 2 : 3;
		return std::min (l, (int)nLods);
	}
	const double FETCH_BUDGET = 0.003; // [s] reading cells per frame (more while nothing is there yet)

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
	if (far.buf.buffer) {
		vkc::Buffer bb = far.buf;
		ctx->DeferDestroy ([ctx, bb]() mutable { ctx->DestroyBuffer (bb); });
	}
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
	c.allImp = true;
	c.farBuild = c.farPending = 0;
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
		if (!k.asset->impostor) c.allImp = false;
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
			else missing.push_back ({ l, cl[2*i], cl[2*i+1], length (CellCentre (l, cl[2*i], cl[2*i+1]) - P) }); // (nearest first, any layer)
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

	// the far set: started, continued
	BuildFar (P);

	// the instances: in view (drawn), near (shadows), nearer (ray tracing)
	MATRIX3 Rp;
	oapiGetRotationMatrix (hPlanet, &Rp);
	VECTOR3 gp;
	oapiGetGlobalPos (hPlanet, &gp);
	origin = cam;
	VECTOR3 d0 = gp - cam;
	size_t nk = kinds.size();
	std::vector<std::vector<float>> bucket (nk * SLOTS), sbucket (nk * SLOTS);
	for (auto &kv : cells) {
		Cell &c = kv.second;
		if (c.used != frame || c.inst.empty()) continue;
		const SCATTERLAYER &L = layers[c.layer];
		bool inFar = far.id && c.farBuild == far.id; // (its far instances: drawn by the far set)
		if (inFar && c.allImp && length (c.centre - far.anchor) - c.radius > FAR_DIST) continue;
		VECTOR3 rc = mul (Rp, c.centre) + d0;
		double dc = length (rc);
		if (dc - c.radius > L.range) continue;
		bool inView = scene->SphereInView (rc, c.radius);
		// (trees: impostors cast into the wide maps beyond the meshes)
		bool shadow = L.kind != SCATTER_GRASS && (dc - c.radius < SHADOW_RANGE || L.kind == SCATTER_TREE);
		if (!inView && !shadow) continue;
		for (const Inst &x : c.inst) {
			const Kind &k = kinds[x.kind];
			if (inFar && k.asset->impostor && length (x.pos - far.anchor) > FAR_DIST) continue;
			VECTOR3 rel = mul (Rp, x.pos) + d0;
			double d = length (rel);
			if (d > L.range) continue;
			// (shrunk away over the last 15% of the range)
			double f = std::min (1.0, (L.range - d) / (0.15 * L.range));
			double px = scene->PixelSize (x.size * f, d);
			if (px < 1.0) continue;
			int lod = Lod (px, k.asset->lods.size());
			int slot = (lod == 3 && k.asset->impostor) ? 4 : lod;
			// object -> global frame relative to the camera (rows)
			float row[12];
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++)
					row[i*4+j] = (float)((Rp.data[i*3] * x.R[j] + Rp.data[i*3+1] * x.R[3+j] + Rp.data[i*3+2] * x.R[6+j]) * f);
				row[i*4+3] = (float)rel.data[i];
			}
			if (inView) bucket[x.kind * SLOTS + slot].insert (bucket[x.kind * SLOTS + slot].end(), row, row + 12);
			if (shadow && d < (L.kind == SCATTER_ROCK ? SHADOW_RANGE : SHADOW_RANGE_PLANT)) { // (a level coarser)
				int sl = std::min (lod + 1, (int)k.asset->lods.size());
				sbucket[x.kind * SLOTS + sl].insert (sbucket[x.kind * SLOTS + sl].end(), row, row + 12);
			} else if (shadow && L.kind == SCATTER_TREE && k.asset->impostor)
				sbucket[x.kind * SLOTS + 4].insert (sbucket[x.kind * SLOTS + 4].end(), row, row + 12);
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
			const Kind &k = kinds[i / SLOTS];
			int slot = (int)(i % SLOTS);
			const VkMesh *m = slot == 4 ? k.asset->impostor.get() : slot ? k.asset->lods[slot - 1].get() : k.asset->mesh.get();
			uint32_t n = (uint32_t)(bk[i].size() / 12);
			memcpy (dst + (size_t)at * 12, bk[i].data(), bk[i].size() * sizeof(float));
			out.push_back ({ m, at, n, k.kind, slot == 4 });
			at += n;
		}
	};
	emit (bucket, batches);
	emit (sbucket, shadowBatches);

	// (ORBITER_VK_PROFILE: instances and triangles drawn)
	static const bool profile = getenv ("ORBITER_VK_PROFILE") && atoi (getenv ("ORBITER_VK_PROFILE"));
	if (profile && frame % 240 == 0) {
		uint64_t tri[4] = {}, ninst[4] = {}, stri = 0, nfar = 0;
		for (const Batch &b : batches) {
			uint64_t t = 0;
			for (uint32_t g = 0; g < b.mesh->nGroup(); g++) t += b.mesh->GetGroup (g).nIndex / 3;
			tri[b.kind] += t * b.count, ninst[b.kind] += b.count;
		}
		for (const Batch &b : far.batches) nfar += b.count;
		for (const Batch &b : shadowBatches)
			for (uint32_t g = 0; g < b.mesh->nGroup(); g++) stri += (uint64_t)b.mesh->GetGroup (g).nIndex / 3 * b.count;
		oapiWriteLogV ("VulkanClient: ground scatter: %zu cells; drawn rocks %llu (%.2fM triangles), grass %llu (%.2fM), shrubs %llu (%.2fM), trees %llu (%.2fM); shadows %.2fM; ray traced %zu; far set %llu",
			cells.size(), (unsigned long long)ninst[0], tri[0] * 1e-6, (unsigned long long)ninst[1], tri[1] * 1e-6,
			(unsigned long long)ninst[2], tri[2] * 1e-6, (unsigned long long)ninst[3], tri[3] * 1e-6, stri * 1e-6, rtInst.size(),
			(unsigned long long)nfar);
	}
}

void Field::Render (RenderPass pass)
{
	if (!active || pass == RenderPass::Transparent || scene->InProbe ()) return;
	vkc::Context *ctx = scene->Client()->Ctx();
	const vkc::Buffer &B = buf[ctx->FrameIndex() % vkc::FRAMES_IN_FLIGHT];
	for (const Batch &b : batches)
		scene->DrawMeshInstanced (b.mesh, B.buffer, (VkDeviceSize)b.first * Scene::INSTANCE_STRIDE, b.count, pass, nullptr, b.impostor);
	// the far set: transforms in the planet frame relative to its anchor
	if (far.id && !far.batches.empty()) {
		MATRIX3 Rp;
		oapiGetRotationMatrix (hPlanet, &Rp);
		VECTOR3 gp;
		oapiGetGlobalPos (hPlanet, &gp);
		Mat4 fv = scene->View () * Mat4::FromRotTrans (Rp, gp + mul (Rp, far.anchor) - scene->CameraPos ());
		for (const Batch &b : far.batches)
			scene->DrawMeshInstanced (b.mesh, far.buf.buffer, (VkDeviceSize)b.first * Scene::INSTANCE_STRIDE, b.count, pass, &fv, true, b.range);
		if (pass == RenderPass::Depth && far.rtProxy) scene->AddMeshRT (far.rtProxy.get(), fv, true);
	}
	if (pass == RenderPass::Depth)
		for (const RTInst &r : rtInst) scene->AddMeshRT (r.mesh, scene->View () * r.M); // (the view of this pass)
}

void Field::BuildFar (const VECTOR3 &P)
{
	// start: the camera has moved from the anchor, or candidate cells have
	// come that the far set does not have
	if (!fb.active) {
		size_t fresh = 0;
		for (auto &kv : cells)
			if (kv.second.allImp && !kv.second.inst.empty() && (!far.id || kv.second.farBuild != far.id)) fresh++;
		bool moved = !far.id || length (P - far.anchor) > FAR_MOVE;
		if (!fresh || (!moved && (fresh < FAR_NEWCELLS || frame - farFrame < FAR_MIN_FRAMES))) return;
		farFrame = frame;
		fb.active = true;
		fb.id = std::max (far.id, fb.id) + 1;
		fb.anchor = P;
		fb.keys.clear();
		for (auto &kv : cells)
			if (kv.second.allImp && !kv.second.inst.empty()) fb.keys.push_back (kv.first);
		fb.next = 0;
		fb.bucket.assign (kinds.size(), {});
		fb.range.assign (kinds.size(), 0.0f);
		fb.proxy.clear();
	}
	// continue: the far instances of some cells
	auto t0 = std::chrono::steady_clock::now();
	while (fb.next < fb.keys.size()) {
		auto it = cells.find (fb.keys[fb.next++]);
		if (it == cells.end()) continue;
		Cell &c = it->second;
		const SCATTERLAYER &L = layers[c.layer];
		c.farPending = fb.id;
		bool rt = scene->Client()->Ctx()->rayTracing;
		if (length (c.centre - fb.anchor) + c.radius < (rt ? RT_PROXY_MIN : FAR_DIST)) continue; // (nothing for the far set)
		for (size_t n = 0; n < c.inst.size(); n++) {
			const Inst &x = c.inst[n];
			VECTOR3 rel = x.pos - fb.anchor;
			double d = length (rel);
			if (rt && d > RT_PROXY_MIN && d < RT_PROXY_MAX) {
				// (a stand-in for the crown: a double cone, six-sided, 12 triangles)
				const Asset *a = kinds[x.kind].asset;
				if (fb.proxy.empty() || fb.proxy.back().vtx.size() + 8 > 65535) {
					fb.proxy.emplace_back ();
					fb.proxy.back().mat.diffuse = { 0.05f, 0.08f, 0.03f, 1.0f };
				}
				VkMesh::Part &pt = fb.proxy.back();
				uint16_t b = (uint16_t)pt.vtx.size();
				double lo = a->crownLo, hi = a->crownHi, mid = lo + 0.4 * (hi - lo), r = 0.7 * a->crownR;
				auto put = [&](double ox, double oy, double oz, double nx, double ny, double nz) {
					NTVERTEX v{};
					for (int i = 0; i < 3; i++) {
						(&v.x)[i] = (float)(x.R[i*3] * ox + x.R[i*3+1] * oy + x.R[i*3+2] * oz + rel.data[i]);
						(&v.nx)[i] = (float)(x.R[i*3] * nx + x.R[i*3+1] * ny + x.R[i*3+2] * nz);
					}
					pt.vtx.push_back (v);
				};
				put (0, hi, 0, 0, 1, 0);
				put (0, lo, 0, 0, -1, 0);
				for (int k = 0; k < 6; k++) {
					double ang = k * PI2 / 6.0;
					put (r * cos (ang), mid, r * sin (ang), cos (ang), 0, sin (ang));
				}
				for (int k = 0; k < 6; k++) {
					uint16_t r0 = b + 2 + k, r1 = b + 2 + (k + 1) % 6;
					// (front faces outward as ray tracing sees them: shadow rays cull back faces)
					pt.idx.insert (pt.idx.end(), { b, r1, r0, (uint16_t)(b + 1), r0, r1 });
				}
			}
			if (d <= FAR_DIST) continue;
			// (thinned out with distance: kept by a hash of the instance)
			double keep = std::min (1.0, FAR_THIN / d);
			uint64_t h = (it->first + n * 0x9e3779b97f4a7c15ull) * 0xbf58476d1ce4e5b9ull;
			if ((double)(h >> 40) / (double)(1ull << 24) >= keep) continue;
			float wide = (float)(1.0 / std::sqrt (keep));
			float row[12];
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++) row[i*4+j] = x.R[i*3+j] * (j == 1 ? 1.0f : wide);
				row[i*4+3] = (float)rel.data[i];
			}
			fb.bucket[x.kind].insert (fb.bucket[x.kind].end(), row, row + 12);
			fb.range[x.kind] = (float)L.range;
		}
		if (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() > FAR_BUDGET) return;
	}
	// done: upload, and the new set replaces the old one
	vkc::Context *ctx = scene->Client()->Ctx();
	size_t total = 0;
	for (auto &b : fb.bucket) total += b.size();
	Far nf;
	nf.id = fb.id;
	nf.anchor = fb.anchor;
	if (total) {
		nf.buf = ctx->CreateBuffer (total * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		float *dst = (float*)nf.buf.mapped;
		uint32_t at = 0;
		for (size_t k = 0; k < fb.bucket.size(); k++) {
			if (fb.bucket[k].empty()) continue;
			uint32_t n = (uint32_t)(fb.bucket[k].size() / 12);
			memcpy (dst + (size_t)at * 12, fb.bucket[k].data(), fb.bucket[k].size() * sizeof(float));
			nf.batches.push_back ({ kinds[k].asset->impostor.get(), at, n, kinds[k].kind, true, fb.range[k] });
			at += n;
		}
	}
	if (far.buf.buffer) {
		vkc::Buffer old = far.buf;
		ctx->DeferDestroy ([ctx, old]() mutable { ctx->DestroyBuffer (old); });
	}
	if (!fb.proxy.empty()) nf.rtProxy = std::make_unique<VkMesh> (ctx, scene->Client()->TexMgr(), fb.proxy);
	{
		static const bool profile = getenv ("ORBITER_VK_PROFILE") && atoi (getenv ("ORBITER_VK_PROFILE"));
		size_t ntri = 0;
		for (auto &p : fb.proxy) ntri += p.idx.size() / 3;
		if (profile) oapiWriteLogV ("VulkanClient: ground scatter far set %u: %zu cells, ray tracing stand-ins %zu triangles in %zu parts", fb.id, fb.keys.size(), ntri, fb.proxy.size());
	}
	fb.proxy.clear();
	far = std::move (nf);
	for (auto &kv : cells)
		if (kv.second.farPending == far.id) kv.second.farBuild = far.id;
	fb.active = false;
	fb.bucket.clear();
}

void Field::RenderShadow (ShadowMap &sm)
{
	if (!active || length (sm.Centre() - origin) > sm.Radius() + maxRange) return;
	vkc::Context *ctx = scene->Client()->Ctx();
	const vkc::Buffer &B = buf[ctx->FrameIndex() % vkc::FRAMES_IN_FLIGHT];
	const MATRIX3 I = _M(1,0,0, 0,1,0, 0,0,1);
	for (const Batch &b : shadowBatches)
		sm.DrawMeshInstanced (b.mesh, B.buffer, (VkDeviceSize)b.first * Scene::INSTANCE_STRIDE, b.count, I, origin, b.impostor);
	// the far set: into the wide maps only (it begins FAR_DIST away)
	if (far.id && sm.Radius() > FAR_DIST) {
		MATRIX3 Rp;
		oapiGetRotationMatrix (hPlanet, &Rp);
		VECTOR3 gp;
		oapiGetGlobalPos (hPlanet, &gp);
		for (const Batch &b : far.batches)
			sm.DrawMeshInstanced (b.mesh, far.buf.buffer, (VkDeviceSize)b.first * Scene::INSTANCE_STRIDE, b.count, Rp, gp + mul (Rp, far.anchor), true);
	}
}

} // namespace vkscatter

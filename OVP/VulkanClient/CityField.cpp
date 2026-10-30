// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "CityField.h"
#include "Shadows.h"
#include "VulkanClient.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace vkcity {

namespace {

	// tiers: cells this near are read with the buildings this tall
	struct Tier { double minHeight, range; };
	const Tier TIERS[] = { { 0.0, 3000.0 }, { 10.0, 9000.0 }, { 35.0, 25000.0 } };
	const int NTIER = 3;
	const double MOVE = 300.0;          // [m] new set when the camera has moved this far from the anchor
	const double SHADOW_RANGE = 1500.0; // [m] buildings casting into the shadow maps
	const double RT_RANGE = 800.0;      // [m] buildings in the ray tracing scene
	const size_t RT_MAX = 3000;
	const double BUDGET = 0.002;        // [s] building the set per frame (more while there is none)
	// the road map: two levels of ROAD_N x ROAD_N texels, ROAD_FINE and ROAD_COARSE m wide
	const int ROAD_N = 2048;
	const double ROAD_FINE = 2048.0, ROAD_COARSE = 12288.0;
	// and the far map: FAR_N x FAR_N texels, ROAD_FAR m wide, the share of
	// the ground built up (from the streets' density): the roof carpet beyond
	// the small buildings (tile_main.glsl CityGround)
	const int FAR_N = 1024;
	const double ROAD_FAR = 24576.0;
	const double ROAD_RANGE = 0.71 * ROAD_FAR + 600.0; // [m] cells whose streets go into the maps
	const double LAMP_SEG_RANGE = 0.71 * ROAD_COARSE + 600.0; // [m] streets with lamps (to the coarse level's edge)
	// street lamps: spacing along avenues (both sides) and streets
	// (alternate sides) [m], the radius of their pool of light [m], the range
	// they are drawn to (fading out from LAMP_FADE) [m], their glows' size
	// [m] and brightness
	const double LAMP_AVENUE = 30.0, LAMP_STREET = 36.0, LAMP_POOL = 16.0;
	const float LAMP_FADE = 5000.0f, LAMP_RANGE = 8000.0f, LAMP_SIZE = 1.2f, LAMP_GLOW = 6.0f;


	inline uint64_t Key (int tier, int row, int col)
	{
		return ((uint64_t)tier << 56) | ((uint64_t)(uint32_t)row << 28) | (uint64_t)(uint32_t)col;
	}

	/// a box: x and z -0.5 to 0.5, y 0 to 1 (outward winding)
	std::unique_ptr<VkMesh> MakeBox (vkc::Context *ctx, TextureManager *tm)
	{
		VkMesh::Part p;
		p.mat.diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
		p.mat.specular = { 0.05f, 0.05f, 0.05f, 1.0f };
		p.pbr.roughness = 0.85f;
		const float n[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		for (int f = 0; f < 6; f++) {
			// two axes across the face, a x b along its normal
			float a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 };
			int ax = n[f][0] != 0.0f ? 0 : n[f][1] != 0.0f ? 1 : 2;
			int ia = (ax + 1) % 3, ib = (ax + 2) % 3;
			float s = n[f][ax];
			a[ia] = 1.0f, b[ib] = 1.0f;
			if (s < 0.0f) std::swap (a, b);
			uint16_t base = (uint16_t)p.vtx.size();
			for (int k = 0; k < 4; k++) {
				float u = (k == 1 || k == 2) ? 0.5f : -0.5f, v = (k >= 2) ? 0.5f : -0.5f;
				float c[3];
				for (int i = 0; i < 3; i++) c[i] = n[f][i] * 0.5f + a[i] * u + b[i] * v;
				NTVERTEX vt{};
				vt.x = c[0], vt.y = c[1] + 0.5f, vt.z = c[2];
				vt.nx = n[f][0], vt.ny = n[f][1], vt.nz = n[f][2];
				vt.tu = u + 0.5f, vt.tv = v + 0.5f;
				p.vtx.push_back (vt);
			}
			// (a x b along n: (0,1,2), (0,2,3) have normal a x b)
			p.idx.insert (p.idx.end(), { base, (uint16_t)(base + 1), (uint16_t)(base + 2), base, (uint16_t)(base + 2), (uint16_t)(base + 3) });
		}
		return std::make_unique<VkMesh> (ctx, tm, std::vector<VkMesh::Part>{ p });
	}

} // namespace

Field::Field (Scene *s, OBJHANDLE hp): scene (s), hPlanet (hp)
{
	R = oapiGetSize (hPlanet);
	cellSize = oapiCityCellSize (hPlanet);
	if (cellSize <= 0.0) return;
	box = MakeBox (scene->Client()->Ctx(), scene->Client()->TexMgr());
	auto t0 = std::chrono::steady_clock::now();
	kit = std::make_unique<Kit> (scene->Client()->Ctx(), scene->Client()->TexMgr());
	kit->SetStyles (hPlanet);
	active = true;
	char name[64];
	oapiGetObjectName (hPlanet, name, 64);
	oapiWriteLogV ("VulkanClient: cities of %s (kit made in %.0f ms)", name,
		std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() * 1e3);
}

Field::~Field ()
{
	vkc::Context *ctx = scene->Client()->Ctx();
	if (roadJob.valid()) roadJob.wait ();
	scene->SetCityMap (nullptr, nullptr, nullptr, nullptr, _V(0, 0, 0), _V(1, 0, 0), _V(0, 1, 0), 1.0, 1.0, 1.0);
	TextureManager *tm = scene->Client()->TexMgr();
	for (VkTexture *t : roadTex)
		if (t) ctx->DeferDestroy ([tm, t]() { tm->Release (t); });
	if (set.buf.buffer) {
		vkc::Buffer bb = set.buf;
		ctx->DeferDestroy ([ctx, bb]() mutable { ctx->DestroyBuffer (bb); });
	}
	if (lampBuf.buffer) {
		vkc::Buffer lb = lampBuf;
		ctx->DeferDestroy ([ctx, lb]() mutable { ctx->DestroyBuffer (lb); });
	}
}

VECTOR3 Field::CellCentre (int row, int col) const
{
	// (the core's grid, Cities.h)
	int nr = std::max (1, (int)(PI * R / cellSize));
	double lat = -PI05 + (row + 0.5) * PI / nr;
	int nc = std::max (1, (int)(PI2 * R * cos (lat) / cellSize));
	double lng = -PI + (col + 0.5) * PI2 / nc;
	return _V(cos (lat) * cos (lng), sin (lat), cos (lat) * sin (lng)) * R;
}

void Field::Fetch (uint64_t key, Cell &c)
{
	int tier = (int)(key >> 56), row = (int)((key >> 28) & 0xfffffff), col = (int)(key & 0xfffffff);
	c.tier = tier;
	double minH = TIERS[tier].minHeight;
	int n = oapiCityCell (hPlanet, row, col, minH, nullptr, 0);
	std::vector<CITYBUILDING> in (n);
	if (n) oapiCityCell (hPlanet, row, col, minH, in.data(), n);
	c.inst.clear();
	c.inst.reserve (n);
	for (const CITYBUILDING &b : in) {
		Inst x;
		x.pos = b.pos;
		x.shape = (uint8_t)kit->Make (b, x.bi, tier > 0);
		c.inst.push_back (x);
	}
}

void Field::Prepare ()
{
	rtThis.clear();
	if (!active) return;
	frame++;
	VECTOR3 cam = scene->CameraPos (), P;
	oapiGlobalToLocal (hPlanet, &cam, &P);
	origin = cam;
	double alt = length (P) - R;
	if (alt > TIERS[NTIER - 1].range) return; // (far above: the set kept a while)
	unsigned gen = oapiCityGeneration (hPlanet);
	if (gen != generation) {
		cells.clear(), generation = gen;
		build.active = false;
		set.id = 0; // (rebuilt at once)
	}

	// a new set: the camera has moved from the anchor
	if (!build.active && (!set.id || length (P - set.anchor) > MOVE)) {
		build.active = true;
		build.id = std::max (set.id, build.id) + 1;
		build.anchor = P;
		build.keys.clear();
		build.next = 0;
		int n = oapiCityCells (hPlanet, P, TIERS[NTIER - 1].range, nullptr, 0);
		std::vector<int> cl (2 * n);
		oapiCityCells (hPlanet, P, TIERS[NTIER - 1].range, cl.data(), n);
		std::vector<std::pair<double, uint64_t>> order;
		for (int i = 0; i < n; i++) {
			double d = length (CellCentre (cl[2*i], cl[2*i+1]) - P);
			int t = 0;
			while (t < NTIER && d > TIERS[t].range) t++;
			if (t < NTIER) order.push_back ({ d, Key (t, cl[2*i], cl[2*i+1]) });
		}
		std::sort (order.begin(), order.end());
		for (auto &o : order) build.keys.push_back (o.second);
		roadSegs.clear();
		build.bucket.assign (NSHAPE, {});
		build.sbucket.assign (NSHAPE, {});
		build.rtRows.clear();
	}
	if (build.active) Continue ();
	FinishRoads ();

	// ray tracing: the near buildings of the set
	if (set.id && scene->Client()->Ctx()->rayTracing && !set.rt.empty()) {
		MATRIX3 Rp;
		oapiGetRotationMatrix (hPlanet, &Rp);
		VECTOR3 gp;
		oapiGetGlobalPos (hPlanet, &gp);
		Mat4 toCam = Mat4::FromRotTrans (Rp, gp + mul (Rp, set.anchor) - cam);
		for (size_t i = 0; i + 12 <= set.rt.size() && rtThis.size() < RT_MAX; i += 12) {
			const float *row = &set.rt[i];
			Mat4 L;
			for (int a = 0; a < 3; a++) {
				for (int b = 0; b < 3; b++) L.m[b*4+a] = row[a*4+b];
				L.m[12+a] = row[a*4+3];
			}
			L.m[3] = L.m[7] = L.m[11] = 0.0, L.m[15] = 1.0;
			rtThis.push_back ({ box.get(), toCam * L });
		}
	}
}

void Field::Continue ()
{
	auto t0 = std::chrono::steady_clock::now();
	double budget = set.id ? BUDGET : 0.1;
	while (build.next < build.keys.size()) {
		uint64_t key = build.keys[build.next++];
		auto it = cells.find (key);
		if (it == cells.end()) {
			it = cells.emplace (key, Cell ()).first;
			Fetch (key, it->second);
		}
		Cell &c = it->second;
		c.set = build.id;
		// its streets (near enough for the road map)
		int row = (int)((key >> 28) & 0xfffffff), col = (int)(key & 0xfffffff);
		if (length (CellCentre (row, col) - build.anchor) < ROAD_RANGE) {
			uint64_t rk = ((uint64_t)(uint32_t)row << 28) | (uint64_t)(uint32_t)col;
			auto sit = streets.find (rk);
			if (sit == streets.end()) {
				std::vector<Seg> &v = streets[rk];
				int n = oapiCityStreets (hPlanet, row, col, nullptr, 0);
				std::vector<CITYSTREET> in (n);
				if (n) oapiCityStreets (hPlanet, row, col, in.data(), n);
				for (const CITYSTREET &x : in) v.push_back ({ x.a, x.b, (float)x.width, x.kind });
				sit = streets.find (rk);
			}
			roadSegs.insert (roadSegs.end(), sit->second.begin(), sit->second.end());
		}
		for (const Inst &x : c.inst) {
			VECTOR3 rel = x.pos - build.anchor;
			BInst bi = x.bi;
			for (int i = 0; i < 3; i++) bi.row[i*4+3] = (float)rel.data[i];
			build.bucket[x.shape].push_back (bi);
			double d = length (rel);
			if (c.tier == 0 && d < SHADOW_RANGE) build.sbucket[x.shape].push_back (bi);
			if (c.tier == 0 && d < RT_RANGE) Kit::RTBoxes (x.shape, bi, build.rtRows);
		}
		if (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() > budget) return;
	}

	// done: upload; the new set replaces the old one
	vkc::Context *ctx = scene->Client()->Ctx();
	size_t total = 0;
	for (auto *bk : { &build.bucket, &build.sbucket })
		for (auto &b : *bk) total += b.size();
	Set ns;
	ns.id = build.id;
	ns.anchor = build.anchor;
	if (total) {
		ns.buf = ctx->CreateBuffer (total * sizeof(BInst), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		BInst *dst = (BInst*)ns.buf.mapped;
		uint32_t at = 0;
		auto emit = [&](std::vector<std::vector<BInst>> &bk, std::vector<Batch> &out) {
			for (size_t k = 0; k < bk.size(); k++) {
				if (bk[k].empty()) continue;
				uint32_t n = (uint32_t)bk[k].size();
				memcpy (dst + at, bk[k].data(), n * sizeof(BInst));
				out.push_back ({ at, n, (int)k });
				at += n;
			}
		};
		emit (build.bucket, ns.batches);
		emit (build.sbucket, ns.shadow);
	}
	ns.rt.swap (build.rtRows);
	if (set.buf.buffer) {
		vkc::Buffer old = set.buf;
		ctx->DeferDestroy ([ctx, old]() mutable { ctx->DestroyBuffer (old); });
	}
	set = std::move (ns);
	build.active = false;
	build.bucket.clear(), build.sbucket.clear();
	for (auto it = cells.begin(); it != cells.end(); )
		if (it->second.set != set.id) it = cells.erase (it); else ++it;
	if (streets.size() > 12000) streets.clear(); // (read again as needed)
	StartRoads (set.anchor);

	static const bool profile = getenv ("ORBITER_VK_PROFILE") && atoi (getenv ("ORBITER_VK_PROFILE"));
	if (profile) {
		size_t n = 0, ns_ = 0;
		for (const Batch &b : set.batches) n += b.count;
		for (const Batch &b : set.shadow) ns_ += b.count;
		oapiWriteLogV ("VulkanClient: city set %u: %zu cells, %zu buildings, %zu casting shadows, %.1f ms", set.id, build.keys.size(), n, ns_,
			std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() * 1e3);
	}
}

void Field::StartRoads (const VECTOR3 &anchor)
{
	if (roadJob.valid()) return; // (one at a time; the next set starts another)
	RoadMap rm;
	rm.centre = unit (anchor) * R;
	double lng = atan2 (anchor.z, anchor.x), lat = asin (anchor.y / length (anchor));
	rm.X = _V(-sin (lng), 0, cos (lng));
	rm.Y = _V(-sin (lat) * cos (lng), cos (lat), -sin (lat) * sin (lng));
	std::vector<Seg> segs;
	segs.swap (roadSegs);
	roadJob = std::async (std::launch::async, [rm, segs]() mutable {
		// street lamps: along avenues on both sides, along streets on
		// alternate sides; their light on the ground about the foot of the
		// pole, towards the road
		struct Pool { VECTOR3 p; float led; };
		std::vector<Pool> pools;
		for (const Seg &s : segs) {
			VECTOR3 ab = s.b - s.a;
			double len = length (ab);
			if (len < 1.0 || length (s.a - rm.centre) > LAMP_SEG_RANGE) continue;
			double sp = s.kind ? LAMP_AVENUE : LAMP_STREET;
			int n = std::max (1, (int)(len / sp));
			VECTOR3 up = unit (s.a + s.b), side = unit (crossp (up, ab));
			for (int k = 0; k < n; k++) {
				VECTOR3 p = s.a + ab * ((k + 0.5) / n);
				// (sodium or LED by districts of 1.5 km)
				VECTOR3 q = p * (1.0 / 1500.0);
				uint32_t h = (uint32_t)(int64_t)floor (q.x) * 73856093u ^ (uint32_t)(int64_t)floor (q.y) * 19349663u ^ (uint32_t)(int64_t)floor (q.z) * 83492791u;
				h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
				float led = (h & 1023u) < 440u ? 1.0f : 0.0f;
				for (int sd = -1; sd <= 1; sd += 2) {
					if (!s.kind && ((k & 1) ? sd < 0 : sd > 0)) continue;
					VECTOR3 foot = p + side * (sd * (0.5 * s.width + 1.0));
					VECTOR3 lamp = foot + up * (s.kind ? 11.0 : 8.5) - side * (sd * 1.5); // (on its arm over the road)
					VECTOR3 r = lamp - rm.centre;
					rm.lamps.insert (rm.lamps.end(), { (float)r.x, (float)r.y, (float)r.z, led });
					pools.push_back ({ foot - side * (sd * 2.5), led });
				}
			}
		}
		// each level: asphalt (r) and the pavement beside it (g), antialiased;
		// the lamps' light (b) and its share of LED light (a)
		auto raster = [&](double size, std::vector<uint8_t> &out) {
			out.assign ((size_t)ROAD_N * ROAD_N * 4, 0);
			double px = size / ROAD_N;
			for (const Seg &s : segs) {
				VECTOR3 da = s.a - rm.centre, db = s.b - rm.centre;
				double ax = dotp (da, rm.X) / px + 0.5 * ROAD_N, ay = dotp (da, rm.Y) / px + 0.5 * ROAD_N;
				double bx = dotp (db, rm.X) / px + 0.5 * ROAD_N, by = dotp (db, rm.Y) / px + 0.5 * ROAD_N;
				double hw = 0.5 * s.width / px, walk = (s.kind ? 4.0 : 2.5) / px;
				double ext = hw + walk + 1.0;
				int x0 = std::max (0, (int)floor (std::min (ax, bx) - ext)), x1 = std::min (ROAD_N - 1, (int)ceil (std::max (ax, bx) + ext));
				int y0 = std::max (0, (int)floor (std::min (ay, by) - ext)), y1 = std::min (ROAD_N - 1, (int)ceil (std::max (ay, by) + ext));
				if (x0 > x1 || y0 > y1) continue;
				double ux = bx - ax, uy = by - ay, l2 = ux * ux + uy * uy;
				for (int y = y0; y <= y1; y++)
					for (int x = x0; x <= x1; x++) {
						double qx = x + 0.5 - ax, qy = y + 0.5 - ay;
						double t = l2 > 0.0 ? std::clamp ((qx * ux + qy * uy) / l2, 0.0, 1.0) : 0.0;
						double dx = qx - t * ux, dy = qy - t * uy, d = sqrt (dx * dx + dy * dy);
						uint8_t *p = &out[((size_t)y * ROAD_N + x) * 4];
						double r = std::clamp (hw - d + 0.5, 0.0, 1.0), g = std::clamp (hw + walk - d + 0.5, 0.0, 1.0);
						p[0] = std::max (p[0], (uint8_t)(r * 255.0 + 0.5));
						p[1] = std::max (p[1], (uint8_t)(g * 255.0 + 0.5));
					}
			}
			// pools of light (at least a texel across: averaged in the coarse level)
			double rad = std::max (LAMP_POOL / px, 1.0);
			double gain = std::min (1.0, (LAMP_POOL / px) * (LAMP_POOL / px));
			for (const Pool &pl : pools) {
				VECTOR3 d = pl.p - rm.centre;
				double cx = dotp (d, rm.X) / px + 0.5 * ROAD_N, cy = dotp (d, rm.Y) / px + 0.5 * ROAD_N;
				int x0 = std::max (0, (int)floor (cx - rad)), x1 = std::min (ROAD_N - 1, (int)ceil (cx + rad));
				int y0 = std::max (0, (int)floor (cy - rad)), y1 = std::min (ROAD_N - 1, (int)ceil (cy + rad));
				for (int y = y0; y <= y1; y++)
					for (int x = x0; x <= x1; x++) {
						double qx = (x + 0.5 - cx) / rad, qy = (y + 0.5 - cy) / rad, q2 = qx * qx + qy * qy;
						if (q2 >= 1.0) continue;
						double f = (1.0 - q2) * (1.0 - q2) * gain;
						uint8_t *p = &out[((size_t)y * ROAD_N + x) * 4];
						double b = std::min (1.0, p[2] / 255.0 + f);
						if (f > 0.05) p[3] = (uint8_t)(pl.led * 255.0f); // (the nearest lamp's colour, roughly)
						p[2] = (uint8_t)(b * 255.0 + 0.5);
					}
			}
		};
		raster (ROAD_FINE, rm.fine);
		raster (ROAD_COARSE, rm.coarse);
		// the far map: the streets a texel wide at least, then averaged over
		// 5 x 5 texels (120 m): the share of streets about, built-up ground
		{
			const int N = FAR_N;
			double px = ROAD_FAR / N;
			std::vector<float> cov ((size_t)N * N, 0.0f);
			for (const Seg &s : segs) {
				VECTOR3 da = s.a - rm.centre, db = s.b - rm.centre;
				double ax = dotp (da, rm.X) / px + 0.5 * N, ay = dotp (da, rm.Y) / px + 0.5 * N;
				double bx = dotp (db, rm.X) / px + 0.5 * N, by = dotp (db, rm.Y) / px + 0.5 * N;
				double l = std::hypot (bx - ax, by - ay);
				int n = std::max (1, (int)ceil (l * 2.0));
				for (int k = 0; k <= n; k++) {
					int x = (int)(ax + (bx - ax) * k / n), y = (int)(ay + (by - ay) * k / n);
					if (x >= 0 && x < N && y >= 0 && y < N) cov[(size_t)y * N + x] = 1.0f;
				}
			}
			rm.far.assign ((size_t)N * N * 4, 0);
			const int B = 2;
			std::vector<float> rows ((size_t)N * N, 0.0f);
			for (int y = 0; y < N; y++) // (box filter: rows, then columns)
				for (int x = 0; x < N; x++) {
					float s = 0.0f;
					for (int k = -B; k <= B; k++) { int xx = x + k; if (xx >= 0 && xx < N) s += cov[(size_t)y * N + xx]; }
					rows[(size_t)y * N + x] = s;
				}
			for (int y = 0; y < N; y++)
				for (int x = 0; x < N; x++) {
					float s = 0.0f;
					for (int k = -B; k <= B; k++) { int yy = y + k; if (yy >= 0 && yy < N) s += rows[(size_t)yy * N + x]; }
					rm.far[((size_t)y * N + x) * 4] = (uint8_t)std::min (255.0f, s / ((2 * B + 1) * (2 * B + 1)) * 255.0f + 0.5f);
				}
		}
		return rm;
	});
}

void Field::FinishRoads ()
{
	if (!roadJob.valid() || roadJob.wait_for (std::chrono::seconds (0)) != std::future_status::ready) return;
	RoadMap rm = roadJob.get ();
	TextureManager *tm = scene->Client()->TexMgr();
	vkc::Context *ctx = scene->Client()->Ctx();
	for (VkTexture *t : roadTex)
		if (t) ctx->DeferDestroy ([tm, t]() { tm->Release (t); });
	roadTex[0] = tm->CreateRGBADeferred (ROAD_N, ROAD_N, rm.fine.data());
	roadTex[1] = tm->CreateRGBADeferred (ROAD_N, ROAD_N, rm.coarse.data());
	roadTex[2] = tm->CreateRGBADeferred (FAR_N, FAR_N, rm.far.data());
	scene->SetCityMap (roadTex[0], roadTex[1], roadTex[2], hPlanet, rm.centre, rm.X, rm.Y, ROAD_FINE, ROAD_COARSE, ROAD_FAR);
	// the lamps
	if (lampBuf.buffer) {
		vkc::Buffer old = lampBuf;
		ctx->DeferDestroy ([ctx, old]() mutable { ctx->DestroyBuffer (old); });
		lampBuf = {};
	}
	lampCount = (uint32_t)(rm.lamps.size() / 4);
	if (lampCount) {
		lampBuf = ctx->CreateBuffer (rm.lamps.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		memcpy (lampBuf.mapped, rm.lamps.data(), rm.lamps.size() * sizeof(float));
	}
	lampCentre = rm.centre;
}

void Field::Render (RenderPass pass)
{
	if (!active || scene->InProbe ()) return;
	MATRIX3 Rp;
	oapiGetRotationMatrix (hPlanet, &Rp);
	VECTOR3 gp, sp = _V(1, 0, 0), cam = scene->CameraPos ();
	oapiGetGlobalPos (hPlanet, &gp);
	if (scene->Sun ()) oapiGetGlobalPos (scene->Sun (), &sp);
	VECTOR3 sun = unit (tmul (Rp, sp - gp)); // (planet frame)
	if (pass == RenderPass::Transparent) {
		if (lampCount) {
			Mat4 fv = scene->View () * Mat4::FromRotTrans (Rp, gp + mul (Rp, lampCentre) - cam);
			scene->DrawLamps (lampBuf.buffer, lampCount, fv, lampCentre, sun, LAMP_FADE, LAMP_RANGE, LAMP_SIZE, LAMP_GLOW);
		}
		return;
	}
	if (!set.id || set.batches.empty()) return;
	// the local (solar) time at the anchor [h]: east of the subsolar point, later
	double dl = atan2 (set.anchor.z, set.anchor.x) - atan2 (sun.z, sun.x);
	double hour = fmod (12.0 + dl * 12.0 / PI + 48.0, 24.0);
	Mat4 fv = scene->View () * Mat4::FromRotTrans (Rp, gp + mul (Rp, set.anchor) - cam);
	for (const Batch &b : set.batches) {
		const Kit::Range &r = kit->Indices (b.shape);
		scene->DrawBuildings (kit->VertexBuffer(), kit->IndexBuffer(), r.first, r.count, set.buf.buffer,
			(VkDeviceSize)b.first * Scene::BUILDING_INSTANCE, b.count, pass, fv, kit->Set(), (float)hour);
	}
	if (pass == RenderPass::Depth)
		for (auto &r : rtThis) scene->AddMeshRT (r.first, scene->View () * r.second);
}

void Field::RenderShadow (ShadowMap &sm)
{
	if (!active || !set.id || set.shadow.empty()) return;
	MATRIX3 Rp;
	oapiGetRotationMatrix (hPlanet, &Rp);
	VECTOR3 gp;
	oapiGetGlobalPos (hPlanet, &gp);
	for (const Batch &b : set.shadow) {
		const Kit::Range &r = kit->Indices (b.shape);
		sm.DrawBuildings (kit->VertexBuffer(), kit->IndexBuffer(), r.first, r.count, set.buf.buffer,
			(VkDeviceSize)b.first * Scene::BUILDING_INSTANCE, b.count, Rp, gp + mul (Rp, set.anchor));
	}
}

} // namespace vkcity

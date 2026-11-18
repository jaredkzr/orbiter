// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "CityKit.h"
#include "VkTexture.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>

namespace vkcity {

namespace {

	// ===================================================================
	// Random numbers and noise (tileable)

	inline uint32_t Hash (uint32_t x)
	{
		x ^= x >> 16; x *= 0x7feb352du;
		x ^= x >> 15; x *= 0x846ca68bu;
		x ^= x >> 16;
		return x;
	}
	inline float R01 (uint32_t a, uint32_t b = 0, uint32_t c = 0)
	{
		return (float)(Hash (a ^ Hash (b ^ Hash (c))) >> 8) * (1.0f / 16777216.0f);
	}

	struct Rnd {
		uint32_t s;
		explicit Rnd (uint32_t seed): s (Hash (seed ^ 0x9e3779b9u)) {}
		float operator() () { s = Hash (s + 0x6d2b79f5u); return (float)(s >> 8) * (1.0f / 16777216.0f); }
		float operator() (float a, float b) { return a + (b - a) * (*this)(); }
		int Pick (const float *w, int n) { // index by weights
			float t = 0.0f;
			for (int i = 0; i < n; i++) t += w[i];
			float x = (*this)() * t;
			for (int i = 0; i < n; i++) { if (x < w[i]) return i; x -= w[i]; }
			return n - 1;
		}
	};

	/// value noise at (x, y) in lattice units, periodic (px, py)
	float VNoise (float x, float y, int px, int py, uint32_t seed)
	{
		int x0 = (int)std::floor (x), y0 = (int)std::floor (y);
		float fx = x - x0, fy = y - y0;
		fx = fx * fx * (3.0f - 2.0f * fx), fy = fy * fy * (3.0f - 2.0f * fy);
		auto at = [&](int i, int j) { return R01 ((uint32_t)(((i % px) + px) % px), (uint32_t)(((j % py) + py) % py), seed); };
		float a = at (x0, y0), b = at (x0 + 1, y0), c = at (x0, y0 + 1), d = at (x0 + 1, y0 + 1);
		return (a + (b - a) * fx) * (1.0f - fy) + (c + (d - c) * fx) * fy;
	}
	/// fractal noise at (u, v) in tiles (0 to 1), f0 lattice cells across
	/// the tile in the first octave (0 to 1, mean 0.5)
	float Fbm (float u, float v, int f0, int oct, uint32_t seed, int fy0 = 0)
	{
		float s = 0.0f, a = 0.5f, t = 0.0f;
		int fx = f0, fy = fy0 ? fy0 : f0;
		for (int o = 0; o < oct; o++) {
			s += a * VNoise (u * fx, v * fy, fx, fy, seed + 101u * o);
			t += a;
			a *= 0.5f, fx *= 2, fy *= 2;
		}
		return s / t;
	}
	inline float Smooth (float e0, float e1, float x)
	{
		float t = std::clamp ((x - e0) / (e1 - e0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}
	inline float Frac (float x) { return x - std::floor (x); }

	// ===================================================================
	// Materials: their tiles, TN texels across (sizes: building.frag TILE)

	const int TN = 512;
	enum Mat { M_PLASTER, M_BRICK, M_BRICK_BUFF, M_CONCRETE, M_STONE, M_SIDING, M_METAL,
		M_ROOF_TILE, M_ROOF_SLATE, M_ROOF_SHINGLE, M_ROOF_FLAT, M_ROOF_METAL, M_PANEL, M_REGOLITH, NMAT };
	const float TILE[NMAT] = { 4.0f, 3.6f, 3.6f, 4.0f, 4.0f, 4.0f, 4.0f, 3.0f, 3.0f, 3.0f, 4.0f, 4.0f, 4.0f, 4.0f };

	struct Layer {
		std::vector<float> rgb, h, rough; // albedo (sRGB values), height [m], roughness
		Layer (): rgb (TN * TN * 3), h (TN * TN), rough (TN * TN) {}
		void Set (int x, int y, float r, float g, float b, float hh, float ro)
		{
			size_t i = (size_t)y * TN + x;
			rgb[i*3] = r, rgb[i*3+1] = g, rgb[i*3+2] = b, h[i] = hh, rough[i] = ro;
		}
	};

	/// running bond bricks: rows courses high, n bricks per row
	void Bricks (Layer &L, uint32_t seed, const float col[3], const float mortar[3], int courses, int n)
	{
		const float len = TILE[M_BRICK] / n, ht = TILE[M_BRICK] / courses, joint = 0.010f;
		for (int y = 0; y < TN; y++)
			for (int x = 0; x < TN; x++) {
				float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
				float cy = v * courses;
				int row = (int)cy;
				float fy = cy - row;
				float bx = u * n + ((row & 1) ? 0.5f : 0.0f);
				int bi = ((int)bx) % n;
				float fx = bx - std::floor (bx);
				float e = std::min (std::min (fx, 1.0f - fx) * len, std::min (fy, 1.0f - fy) * ht) - 0.5f * joint;
				float m = Smooth (-0.001f, 0.0015f, e);
				float k = R01 (seed, (uint32_t)row, (uint32_t)bi);
				float shade = 0.9f + 0.2f * k;
				if (R01 (seed + 7u, (uint32_t)row, (uint32_t)bi) < 0.1f) shade *= 0.82f; // burnt
				float grain = Fbm (u, v, 128, 2, seed + 3u) - 0.5f;
				float mg = Fbm (u, v, 64, 2, seed + 5u) - 0.5f;
				float c[3];
				for (int i = 0; i < 3; i++)
					c[i] = mortar[i] * (1.0f + 0.15f * mg) * (1.0f - m) + col[i] * shade * (1.0f + 0.15f * grain) * m;
				L.Set (x, y, c[0], c[1], c[2], 0.006f * m + 0.0006f * grain, 0.95f - 0.1f * m);
			}
	}

	void MakeMaterial (int k, Layer &L)
	{
		uint32_t seed = 1000u + 77u * k;
		switch (k) {
		case M_PLASTER:
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					float big = Fbm (u, v, 4, 4, seed) - 0.5f, fine = Fbm (u, v, 128, 2, seed + 1u) - 0.5f;
					float a = 0.84f + 0.07f * big + 0.03f * fine;
					L.Set (x, y, a, a * 0.99f, a * 0.96f, 0.0012f * fine, 0.9f);
				}
			break;
		case M_BRICK: {
			const float col[3] = { 0.58f, 0.30f, 0.22f }, mortar[3] = { 0.66f, 0.64f, 0.60f };
			Bricks (L, seed, col, mortar, 48, 16);
			break; }
		case M_BRICK_BUFF: {
			const float col[3] = { 0.78f, 0.66f, 0.48f }, mortar[3] = { 0.70f, 0.68f, 0.64f };
			Bricks (L, seed, col, mortar, 48, 16);
			break; }
		case M_CONCRETE:
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					float big = Fbm (u, v, 6, 4, seed) - 0.5f, fine = Fbm (u, v, 128, 2, seed + 1u) - 0.5f;
					float streak = Fbm (u, v, 40, 3, seed + 2u, 3) - 0.5f;
					float a = 0.62f + 0.08f * big + 0.03f * fine - 0.06f * std::max (streak, 0.0f);
					// form-work joints and tie holes
					float jy = std::fabs (Frac (v * 2.0f + 0.5f) - 0.5f) * TILE[M_CONCRETE] * 0.5f;
					float jx = std::fabs (Frac (u + 0.5f) - 0.5f) * TILE[M_CONCRETE];
					float joint = 1.0f - Smooth (0.004f, 0.012f, std::min (jx, jy));
					float hx = (Frac (u * 8.0f) - 0.5f) * 0.5f, hy = (Frac (v * 8.0f + 0.25f) - 0.5f) * 0.5f;
					float hole = 1.0f - Smooth (0.008f, 0.014f, std::sqrt (hx * hx + hy * hy));
					a *= 1.0f - 0.12f * joint - 0.3f * hole;
					L.Set (x, y, a, a * 0.995f, a * 0.98f, 0.0006f * fine - 0.002f * joint - 0.004f * hole, 0.9f);
				}
			break;
		case M_STONE: { // ashlar: courses of 0.5 m, blocks of various lengths
			const int courses = 8;
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					int row = (int)(v * courses);
					float fy = v * courses - row;
					int nb = 3 + (int)(R01 (seed, (uint32_t)row) * 2.0f);
					float off = R01 (seed + 1u, (uint32_t)row);
					float bx = Frac (u + off) * nb;
					int bi = (int)bx;
					float fx = bx - bi;
					float e = std::min (std::min (fx, 1.0f - fx) * TILE[M_STONE] / nb, std::min (fy, 1.0f - fy) * TILE[M_STONE] / courses);
					float m = Smooth (0.002f, 0.006f, e);
					float shade = 0.95f + 0.08f * R01 (seed + 2u, (uint32_t)row, (uint32_t)bi);
					float fine = Fbm (u, v, 96, 3, seed + 3u) - 0.5f, big = Fbm (u, v, 4, 3, seed + 4u) - 0.5f;
					float a = (0.80f + 0.06f * big + 0.05f * fine) * shade * (0.9f + 0.1f * m);
					L.Set (x, y, a, a * 0.93f, a * 0.80f, 0.003f * m + 0.0008f * fine, 0.85f);
				}
			break; }
		case M_SIDING: { // lap siding, boards of 0.2 m
			const int boards = 20;
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					float fy = Frac (v * boards);
					float grain = Fbm (u, v, 8, 3, seed, 128) - 0.5f;
					float a = (0.90f + 0.04f * grain) * (0.78f + 0.22f * Smooth (0.0f, 0.12f, fy));
					L.Set (x, y, a, a, a * 0.98f, 0.012f * (1.0f - fy) + 0.0005f * grain, 0.55f);
				}
			break; }
		case M_METAL: { // trapezoidal ribs every 0.25 m
			const int ribs = 16;
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					float fx = std::fabs (Frac (u * ribs) - 0.5f);
					float rib = Smooth (0.12f, 0.2f, 0.5f - fx); // (plateau about the middle)
					float streak = Fbm (u, v, 32, 3, seed, 2) - 0.5f;
					float a = 0.80f + 0.05f * streak - 0.05f * (1.0f - rib);
					L.Set (x, y, a, a, a, 0.03f * rib, 0.45f);
				}
			break; }
		case M_ROOF_TILE: { // clay tiles: rows of 0.3 m, barrel tiles 0.25 m wide
			const int rows = 10, n = 12;
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					int row = (int)(v * rows);
					float fy = v * rows - row;
					float bx = u * n + ((row & 1) ? 0.5f : 0.0f);
					int bi = ((int)bx) % n;
					float fx = bx - std::floor (bx);
					float barrel = std::sin (3.14159f * fx);
					float k = R01 (seed, (uint32_t)row, (uint32_t)bi);
					float shade = (0.8f + 0.35f * k) * (0.7f + 0.3f * barrel) * (0.75f + 0.25f * Smooth (0.0f, 0.15f, 1.0f - fy));
					float fine = Fbm (u, v, 64, 2, seed + 1u) - 0.5f;
					L.Set (x, y, 0.70f * shade * (1.0f + 0.1f * fine), 0.36f * shade, 0.22f * shade,
						0.02f * barrel + 0.015f * fy, 0.8f);
				}
			break; }
		case M_ROOF_SLATE: { // rows of 0.25 m, slates 0.3 m wide
			const int rows = 12, n = 10;
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					int row = (int)(v * rows);
					float fy = v * rows - row;
					float bx = u * n + ((row & 1) ? 0.5f : 0.0f);
					int bi = ((int)bx) % n;
					float fx = bx - std::floor (bx);
					float gap = Smooth (0.0f, 0.02f, std::min (fx, 1.0f - fx));
					float k = R01 (seed, (uint32_t)row, (uint32_t)bi);
					float shade = (0.8f + 0.35f * k) * (0.6f + 0.4f * gap) * (0.7f + 0.3f * Smooth (0.0f, 0.1f, 1.0f - fy));
					L.Set (x, y, 0.36f * shade, 0.37f * shade, 0.40f * shade, 0.006f * fy + 0.001f * gap, 0.6f);
				}
			break; }
		case M_ROOF_SHINGLE: { // asphalt: rows of 0.14 m, tabs 0.33 m wide
			const int rows = 21, n = 9;
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					int row = (int)(v * rows);
					float fy = v * rows - row;
					float bx = u * n + ((row & 1) ? 0.5f : 0.0f);
					int bi = ((int)bx) % n;
					float fx = bx - std::floor (bx);
					float gap = Smooth (0.0f, 0.015f, std::min (fx, 1.0f - fx));
					float k = R01 (seed, (uint32_t)row, (uint32_t)bi);
					float grain = R01 (seed + 1u, (uint32_t)x, (uint32_t)y) - 0.5f;
					float shade = (0.85f + 0.25f * k) * (0.7f + 0.3f * gap) * (0.65f + 0.35f * Smooth (0.0f, 0.12f, 1.0f - fy)) * (1.0f + 0.25f * grain);
					L.Set (x, y, 0.46f * shade, 0.45f * shade, 0.44f * shade, 0.004f * fy, 0.95f);
				}
			break; }
		case M_ROOF_FLAT: // membrane in rolls of 1 m, dirt
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					float ds = std::fabs (Frac (v * 4.0f + 0.5f) - 0.5f); // [m] from a seam (rolls of 1 m)
					float seam = 1.0f - Smooth (0.01f, 0.02f, ds);
					float dirt = Fbm (u, v, 4, 4, seed) - 0.5f;
					float grain = R01 (seed + 1u, (uint32_t)x, (uint32_t)y) - 0.5f;
					float a = (0.66f - 0.12f * std::max (dirt, 0.0f) + 0.03f * grain) * (1.0f + 0.06f * seam);
					L.Set (x, y, a, a, a * 0.98f, 0.002f * seam, 0.9f);
				}
			break;
		case M_PANEL: // insulated sandwich panels 1 m wide, 2 m high: joints, fasteners, a little grime (white: tinted)
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					float jx = std::fabs (Frac (u * 4.0f + 0.5f) - 0.5f) * 1.0f, jy = std::fabs (Frac (v * 2.0f + 0.5f) - 0.5f) * 2.0f;
					float joint = 1.0f - Smooth (0.004f, 0.010f, std::min (jx, jy));
					float fx = std::fabs (Frac (u * 4.0f + 0.5f) - 0.5f), fy = std::fabs (Frac (v * 16.0f) - 0.5f) * 0.25f;
					float fast = 1.0f - Smooth (0.004f, 0.008f, std::sqrt (std::pow ((0.5f - fx) * 1.0f - 0.03f, 2.0f) + fy * fy));
					float grime = Fbm (u, v, 4, 3, seed) - 0.5f, fine = Fbm (u, v, 128, 2, seed + 1u) - 0.5f;
					float pnl = R01 (seed + 3u, (uint32_t)(u * 4.0f), (uint32_t)(v * 2.0f));
					float a = (0.88f + 0.03f * pnl + 0.03f * grime + 0.015f * fine) * (1.0f - 0.45f * joint) * (1.0f - 0.25f * fast);
					L.Set (x, y, a, a, a * 0.985f, -0.003f * joint + 0.0004f * fast + 0.0002f * fine, 0.5f + 0.3f * joint);
				}
			break;
		case M_REGOLITH: { // sintered regolith, sandbags 0.8 x 0.4 m in offset rows (neutral: tinted to the soil)
			const int rows = 10, n = 5;
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					int row = (int)(v * rows);
					float fy = v * rows - row;
					float bx = u * n + ((row & 1) ? 0.5f : 0.0f);
					int bi = ((int)bx) % n;
					float fx = bx - std::floor (bx);
					float bag = std::pow (std::max (std::sin (3.14159f * fx), 0.0f), 0.35f) * std::pow (std::max (std::sin (3.14159f * fy), 0.0f), 0.5f);
					float k = R01 (seed, (uint32_t)row, (uint32_t)bi);
					float grain = Fbm (u, v, 128, 2, seed + 1u) - 0.5f;
					float a = 0.62f * (0.88f + 0.12f * bag + 0.06f * (k - 0.5f) + 0.08f * grain) * (0.55f + 0.45f * std::min (bag * 3.0f, 1.0f));
					L.Set (x, y, a, a, a, 0.06f * bag + 0.002f * grain, 0.97f);
				}
			break; }
		case M_ROOF_METAL: { // standing seams every 0.5 m
			const int seams = 8;
			for (int y = 0; y < TN; y++)
				for (int x = 0; x < TN; x++) {
					float u = (x + 0.5f) / TN, v = (y + 0.5f) / TN;
					float d = std::fabs (Frac (u * seams) - 0.5f) * TILE[M_ROOF_METAL] / seams; // [m] from the pan's middle
					float seam = Smooth (0.235f, 0.245f, d);
					float streak = Fbm (u, v, 16, 3, seed, 2) - 0.5f;
					float a = 0.78f + 0.05f * streak;
					L.Set (x, y, a, a, a, 0.03f * seam, 0.4f);
				}
			break; }
		}
	}

	// ===================================================================
	// Shapes

	struct P { float ux, uy, uz, mx, my, mz, rf, cx; };
	inline P Pt (float ux, float uy, float uz, float mx = 0, float my = 0, float mz = 0, float rf = 0, float cx = 0)
	{
		return { ux, uy, uz, mx, my, mz, rf, cx };
	}

	struct Builder {
		std::vector<BVertex> &v;
		std::vector<uint16_t> &idx;
		// (positions for a typical building: for the winding)
		void Eval (const P &p, float r[3]) const
		{
			const float w = 24.0f, d = 12.0f, h = 10.0f, rh = 4.0f;
			r[0] = p.ux * w + p.cx * d + p.mx;
			r[1] = p.uy * h + p.rf * rh + p.my;
			r[2] = p.uz * d + p.mz;
		}
		/// a convex polygon facing out along n (normal given if nrm)
		void Poly (std::initializer_list<P> pts, int part, float share, float nx, float ny, float nz, bool nrm = true)
		{
			std::vector<P> q (pts);
			float a[3], b[3], c[3];
			Eval (q[0], a), Eval (q[1], b), Eval (q[2], c);
			float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			if (cr[0] * nx + cr[1] * ny + cr[2] * nz < 0.0f) std::reverse (q.begin(), q.end());
			uint16_t base = (uint16_t)v.size();
			for (const P &p : q) {
				BVertex x;
				x.unit[0] = p.ux, x.unit[1] = p.uy, x.unit[2] = p.uz;
				x.metric[0] = p.mx, x.metric[1] = p.my, x.metric[2] = p.mz;
				x.shape[0] = p.rf, x.shape[1] = p.cx, x.shape[2] = (float)part, x.shape[3] = share;
				float l = std::sqrt (nx * nx + ny * ny + nz * nz);
				x.normal[0] = nrm ? nx / l : 0.0f, x.normal[1] = nrm ? ny / l : 0.0f, x.normal[2] = nrm ? nz / l : 0.0f;
				v.push_back (x);
			}
			for (size_t i = 1; i + 1 < q.size(); i++)
				idx.insert (idx.end(), { base, (uint16_t)(base + i), (uint16_t)(base + i + 1) });
		}
		/// a quad with a normal per vertex (curved surfaces: normals in the building frame, unscaled)
		void QuadN (const P (&q)[4], const float (&n)[4][3], int part, float share)
		{
			uint16_t base = (uint16_t)v.size();
			for (int i = 0; i < 4; i++) {
				const P &p = q[i];
				BVertex x;
				x.unit[0] = p.ux, x.unit[1] = p.uy, x.unit[2] = p.uz;
				x.metric[0] = p.mx, x.metric[1] = p.my, x.metric[2] = p.mz;
				x.shape[0] = p.rf, x.shape[1] = p.cx, x.shape[2] = (float)part, x.shape[3] = share;
				float l = std::sqrt (n[i][0] * n[i][0] + n[i][1] * n[i][1] + n[i][2] * n[i][2]);
				x.normal[0] = n[i][0] / l, x.normal[1] = n[i][1] / l, x.normal[2] = n[i][2] / l;
				v.push_back (x);
			}
			// (the winding by the first vertex's normal)
			float a[3], b[3], c[3];
			Eval (q[0], a), Eval (q[1], b), Eval (q[2], c);
			float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			float nn[3] = { n[0][0] + n[2][0], n[0][1] + n[2][1], n[0][2] + n[2][2] };
			if (cr[0] * nn[0] + cr[1] * nn[1] + cr[2] * nn[2] < 0.0f)
				idx.insert (idx.end(), { base, (uint16_t)(base + 2), (uint16_t)(base + 1), base, (uint16_t)(base + 3), (uint16_t)(base + 2) });
			else
				idx.insert (idx.end(), { base, (uint16_t)(base + 1), (uint16_t)(base + 2), base, (uint16_t)(base + 2), (uint16_t)(base + 3) });
		}
		/// four walls from y0 to y1 (shares of the walls' height), e from the middle (share of width, depth)
		void Walls (float y0, float y1, float e, int part = P_FACADE, float my0 = 0, float my1 = 0)
		{
			float s = 2.0f * e;
			Poly ({ Pt (e, y0, -e, 0, my0), Pt (e, y0, e, 0, my0), Pt (e, y1, e, 0, my1), Pt (e, y1, -e, 0, my1) }, part, s, 1, 0, 0);
			Poly ({ Pt (-e, y0, -e, 0, my0), Pt (-e, y0, e, 0, my0), Pt (-e, y1, e, 0, my1), Pt (-e, y1, -e, 0, my1) }, part, s, -1, 0, 0);
			Poly ({ Pt (-e, y0, e, 0, my0), Pt (e, y0, e, 0, my0), Pt (e, y1, e, 0, my1), Pt (-e, y1, e, 0, my1) }, part, s, 0, 0, 1);
			Poly ({ Pt (-e, y0, -e, 0, my0), Pt (e, y0, -e, 0, my0), Pt (e, y1, -e, 0, my1), Pt (-e, y1, -e, 0, my1) }, part, s, 0, 0, -1);
		}
		/// a horizontal ring at y between outlines e0 (outer) and e1 (inner), shares; part
		void Ring (float y, float e0, float e1, int part, float my = 0, float m1 = 0)
		{
			// (inner corners: e1 in shares, moved in by m1 [m])
			auto in = [&](float sx, float sz) { return Pt (sx * e1, y, sz * e1, -sx * m1, my, -sz * m1); };
			auto out = [&](float sx, float sz) { return Pt (sx * e0, y, sz * e0, 0, my, 0); };
			Poly ({ out (-1, -1), out (1, -1), in (1, -1), in (-1, -1) }, part, 1, 0, 1, 0);
			Poly ({ out (1, -1), out (1, 1), in (1, 1), in (1, -1) }, part, 1, 0, 1, 0);
			Poly ({ out (1, 1), out (-1, 1), in (-1, 1), in (1, 1) }, part, 1, 0, 1, 0);
			Poly ({ out (-1, 1), out (-1, -1), in (-1, -1), in (-1, 1) }, part, 1, 0, 1, 0);
		}
		/// a flat roof behind a parapet on outline e (share): cap, inner walls, roof 0.8 m down
		void Parapet (float e)
		{
			const float t = 0.3f, dn = -0.8f;
			Ring (1.0f, e, e, P_TRIM, 0.0f, t);
			auto in = [&](float sx, float y, float my, float sz) { return Pt (sx * e, y, sz * e, -sx * t, my, -sz * t); };
			Poly ({ in (1, 1, dn, -1), in (1, 1, dn, 1), in (1, 1, 0, 1), in (1, 1, 0, -1) }, P_WALL, 1, -1, 0, 0);
			Poly ({ in (-1, 1, dn, -1), in (-1, 1, dn, 1), in (-1, 1, 0, 1), in (-1, 1, 0, -1) }, P_WALL, 1, 1, 0, 0);
			Poly ({ in (-1, 1, dn, 1), in (1, 1, dn, 1), in (1, 1, 0, 1), in (-1, 1, 0, 1) }, P_WALL, 1, 0, 0, -1);
			Poly ({ in (-1, 1, dn, -1), in (1, 1, dn, -1), in (1, 1, 0, -1), in (-1, 1, 0, -1) }, P_WALL, 1, 0, 0, 1);
			Poly ({ in (-1, 1, dn, -1), in (1, 1, dn, -1), in (1, 1, dn, 1), in (-1, 1, dn, 1) }, P_ROOF_FLAT, 1, 0, 1, 0);
		}
		/// plant on the roof: a box of half sizes (shares) ex, ez, h [m] high
		void Plant (float ex, float ez, float h)
		{
			const float dn = -0.8f;
			auto c = [&](float sx, float sz, float my) { return Pt (sx * ex, 1, sz * ez, 0, my, 0); };
			Poly ({ c (1, -1, dn), c (1, 1, dn), c (1, 1, h + dn), c (1, -1, h + dn) }, P_WALL, 1, 1, 0, 0);
			Poly ({ c (-1, -1, dn), c (-1, 1, dn), c (-1, 1, h + dn), c (-1, -1, h + dn) }, P_WALL, 1, -1, 0, 0);
			Poly ({ c (-1, 1, dn), c (1, 1, dn), c (1, 1, h + dn), c (-1, 1, h + dn) }, P_WALL, 1, 0, 0, 1);
			Poly ({ c (-1, -1, dn), c (1, -1, dn), c (1, -1, h + dn), c (-1, -1, h + dn) }, P_WALL, 1, 0, 0, -1);
			Poly ({ c (-1, -1, h + dn), c (1, -1, h + dn), c (1, 1, h + dn), c (-1, 1, h + dn) }, P_ROOF_FLAT, 1, 0, 1, 0);
		}
		/// a gable roof along x, eaves o [m] out, gable ends overhanging g [m]
		void Gable (float o, float g)
		{
			auto eave = [&](float sx, float sz) { return Pt (0.5f * sx, 1, 0.5f * sz, sx * g, -0.15f, sz * o); };
			auto ridge = [&](float sx) { return Pt (0.5f * sx, 1, 0, sx * g, 0, 0, 1); };
			for (int side = -1; side <= 1; side += 2) {
				Poly ({ eave (-1, side), eave (1, side), ridge (1), ridge (-1) }, P_ROOF, 1, 0, 1, (float)side, false);
				Poly ({ eave (-1, side), eave (1, side), ridge (1), ridge (-1) }, P_SOFFIT, 1, 0, -1, (float)-side, false);
				Poly ({ Pt (0.5f * side, 1, -0.5f), Pt (0.5f * side, 1, 0.5f), Pt (0.5f * side, 1, 0, 0, 0, 0, 1) }, P_WALL, 1, (float)side, 0, 0);
			}
		}
	};

	// ===================================================================
	// Looks by region

	enum Region { R_GENERIC, R_NAMERICA, R_LATIN, R_EUROPE, R_SOVIET, R_EASTASIA, R_JAPAN, R_SOUTHASIA, R_MIDEAST, R_AFRICA, R_OCEANIA };
	// window styles (building.frag)
	enum Win { W_HOUSE_US, W_HOUSE, W_APART, W_CLASSIC, W_PANEL, W_BANDS, W_CURTAIN, W_GRID, W_INDUSTRIAL, W_SMALL, W_ASIA, W_SHOP,
		W_HANGAR, W_PORTHOLE, W_NONE, W_STRIP }; // (the last four: bases')
	// roof colours (building.frag ROOF_TINT)
	enum RoofCol { RC_NONE, RC_REDBROWN, RC_GREY, RC_PATINA, RC_BLUEGREY, RC_TERRACOTTA, RC_BLACK, RC_LIGHTGREY,
		RC_SAND, RC_DARKRED, RC_TEAL, RC_BROWN, RC_MIDGREY, RC_DARKGREEN, RC_WHITE, RC_MAUVE };

	struct Col { float r, g, b; };
	// wall colours (factors on the material)
	const Col PASTEL[] = { { 1.0f, 1.0f, 1.0f }, { 1.0f, 0.93f, 0.80f }, { 0.97f, 0.80f, 0.58f }, { 1.0f, 0.80f, 0.74f },
		{ 0.80f, 0.88f, 0.96f }, { 0.82f, 0.88f, 0.76f }, { 1.0f, 0.92f, 0.66f }, { 0.80f, 0.80f, 0.80f } };
	const Col BRIGHT[] = { { 0.97f, 0.60f, 0.48f }, { 0.58f, 0.78f, 0.97f }, { 0.98f, 0.86f, 0.42f }, { 0.62f, 0.86f, 0.62f },
		{ 0.94f, 0.64f, 0.82f }, { 1.0f, 1.0f, 1.0f }, { 0.97f, 0.74f, 0.46f } };
	const Col EARTH[] = { { 0.96f, 0.86f, 0.68f }, { 0.92f, 0.78f, 0.58f }, { 1.0f, 0.96f, 0.88f }, { 0.86f, 0.70f, 0.52f }, { 1.0f, 1.0f, 1.0f } };
	const Col SIDING[] = { { 1.0f, 1.0f, 1.0f }, { 0.96f, 0.92f, 0.82f }, { 0.78f, 0.86f, 0.94f }, { 0.76f, 0.78f, 0.76f },
		{ 0.80f, 0.86f, 0.74f }, { 0.98f, 0.92f, 0.70f }, { 0.62f, 0.66f, 0.70f } };
	const Col WOOD[] = { { 0.62f, 0.44f, 0.30f }, { 0.50f, 0.62f, 0.50f }, { 0.55f, 0.65f, 0.80f }, { 0.82f, 0.74f, 0.55f } };
	const Col METALC[] = { { 0.92f, 0.92f, 0.92f }, { 0.62f, 0.70f, 0.80f }, { 0.86f, 0.82f, 0.72f }, { 0.70f, 0.72f, 0.74f }, { 0.60f, 0.68f, 0.62f } };
	const Col SPANDREL[] = { { 0.30f, 0.33f, 0.38f }, { 0.22f, 0.24f, 0.26f }, { 0.55f, 0.57f, 0.60f }, { 0.35f, 0.40f, 0.38f } };
	const Col NEUTRAL[] = { { 1.0f, 1.0f, 1.0f }, { 0.92f, 0.92f, 0.92f }, { 1.05f, 1.02f, 0.97f }, { 0.86f, 0.86f, 0.85f } };

	template<size_t N> Col PickCol (Rnd &r, const Col (&c)[N]) { return c[std::min ((size_t)(r () * N), N - 1)]; }

	uint32_t Pack565 (Col c, float f)
	{
		auto q = [&](float x, int m) { return (uint32_t)std::clamp ((int)std::lround (std::clamp (x * f, 0.0f, 1.0f) * m), 0, m); };
		return (q (c.r, 31) << 11) | (q (c.g, 63) << 5) | q (c.b, 31);
	}

	struct Look {
		int shape = S_FLAT;
		int wall = M_PLASTER, roof = M_ROOF_FLAT, win = W_APART, roofCol = RC_NONE;
		Col tint { 1.0f, 1.0f, 1.0f };
		bool shop = false, door = false, coated = false;
		int deco = 0;
		float pitch = 0.0f; // pitched roofs: rise over half the depth
	};

	Look Choose (int region, int kind, int storeys, double h, Rnd &r)
	{
		Look L;
		auto pick = [&](std::initializer_list<float> w) { return r.Pick (w.begin(), (int)w.size()); };
		switch (kind) {
		case CITY_HOUSE:
			L.door = true, L.win = W_HOUSE, L.shape = r () < 0.5f ? S_GABLE : S_HIP;
			switch (region) {
			case R_NAMERICA: case R_OCEANIA: {
				int k = pick ({ 0.6f, 0.3f, 0.1f });
				L.wall = k == 0 ? M_SIDING : k == 1 ? M_BRICK : M_PLASTER;
				L.tint = k == 0 ? PickCol (r, SIDING) : k == 1 ? PickCol (r, NEUTRAL) : PickCol (r, PASTEL);
				L.win = W_HOUSE_US, L.pitch = 0.42f;
				if (region == R_OCEANIA && r () < 0.4f) L.roof = M_ROOF_METAL, L.roofCol = pick ({ 1, 1, 1, 1 }) ? RC_DARKRED : RC_GREY;
				else L.roof = M_ROOF_SHINGLE, L.roofCol = r () < 0.5f ? RC_NONE : r () < 0.5f ? RC_BROWN : RC_BLACK;
				break; }
			case R_EUROPE: case R_GENERIC: {
				int k = pick ({ 0.5f, 0.4f, 0.1f });
				L.wall = k == 0 ? M_PLASTER : k == 1 ? (r () < 0.7f ? M_BRICK : M_BRICK_BUFF) : M_STONE;
				L.tint = k == 0 ? PickCol (r, PASTEL) : PickCol (r, NEUTRAL);
				L.pitch = 0.85f;
				if (r () < 0.6f) L.roof = M_ROOF_TILE, L.roofCol = r () < 0.7f ? RC_NONE : RC_BROWN;
				else L.roof = M_ROOF_SLATE;
				L.shape = r () < 0.7f ? S_GABLE : S_HIP;
				break; }
			case R_SOVIET:
				L.wall = r () < 0.5f ? M_SIDING : M_PLASTER;
				L.tint = L.wall == M_SIDING ? PickCol (r, WOOD) : PickCol (r, PASTEL);
				L.roof = M_ROOF_METAL, L.roofCol = pick ({ 1, 1, 1, 1 }) == 0 ? RC_DARKGREEN : r () < 0.5f ? RC_GREY : RC_DARKRED;
				L.pitch = 0.6f, L.shape = S_GABLE;
				break;
			case R_EASTASIA: case R_JAPAN:
				L.wall = r () < 0.6f ? M_PLASTER : M_SIDING;
				L.tint = PickCol (r, NEUTRAL);
				L.roof = M_ROOF_SLATE, L.roofCol = r () < 0.5f ? RC_BLUEGREY : RC_BLACK;
				L.pitch = 0.55f, L.win = W_ASIA;
				break;
			default: // latin, mideast, southasia, africa: flat roofs mostly
				L.wall = M_PLASTER;
				L.tint = region == R_LATIN ? PickCol (r, BRIGHT) : PickCol (r, EARTH);
				L.win = region == R_MIDEAST ? W_SMALL : W_HOUSE;
				if (region == R_AFRICA && r () < 0.6f)
					L.shape = S_GABLE, L.roof = M_ROOF_METAL, L.roofCol = r () < 0.5f ? RC_GREY : RC_REDBROWN, L.pitch = 0.3f;
				else if (region == R_LATIN && r () < 0.4f)
					L.shape = S_GABLE, L.roof = M_ROOF_TILE, L.pitch = 0.5f;
				else L.shape = S_FLAT;
				break;
			}
			break;
		case CITY_LOWRISE:
		case CITY_MIDRISE: {
			bool mid = kind == CITY_MIDRISE;
			L.shape = mid ? S_PLANT : S_FLAT;
			L.shop = r () < (mid ? 0.6f : 0.5f);
			L.win = W_APART;
			switch (region) {
			case R_NAMERICA: case R_OCEANIA: {
				int k = pick ({ 0.5f, 0.3f, 0.2f });
				L.wall = k == 0 ? (r () < 0.75f ? M_BRICK : M_BRICK_BUFF) : k == 1 ? M_CONCRETE : M_PLASTER;
				L.tint = k == 2 ? PickCol (r, PASTEL) : PickCol (r, NEUTRAL);
				if (mid && r () < 0.3f) L.win = W_GRID;
				break; }
			case R_EUROPE: case R_GENERIC: {
				int k = pick ({ 0.5f, 0.25f, 0.25f });
				L.wall = k == 0 ? M_PLASTER : k == 1 ? M_STONE : (r () < 0.6f ? M_BRICK : M_BRICK_BUFF);
				L.tint = k == 0 ? PickCol (r, PASTEL) : PickCol (r, NEUTRAL);
				L.win = W_CLASSIC;
				float s = r ();
				if (mid && s < 0.45f && storeys >= 4) L.shape = S_MANSARD, L.roof = M_ROOF_METAL, L.roofCol = RC_BLUEGREY;
				else if (s < 0.75f) L.shape = S_GABLE, L.roof = r () < 0.6f ? M_ROOF_TILE : M_ROOF_SLATE, L.pitch = 0.7f;
				else L.shape = S_FLAT;
				break; }
			case R_SOVIET:
				if (r () < 0.6f) L.wall = M_CONCRETE, L.win = W_PANEL, L.deco = 1, L.tint = PickCol (r, NEUTRAL), L.shop = false;
				else L.wall = r () < 0.5f ? M_BRICK_BUFF : M_PLASTER, L.tint = PickCol (r, PASTEL);
				L.shape = S_FLAT;
				break;
			case R_EASTASIA: case R_JAPAN:
				L.wall = r () < 0.5f ? M_CONCRETE : M_PLASTER;
				L.tint = PickCol (r, NEUTRAL), L.win = W_ASIA;
				break;
			case R_LATIN:
				L.wall = M_PLASTER, L.tint = r () < 0.6f ? PickCol (r, BRIGHT) : PickCol (r, PASTEL);
				L.shape = S_FLAT, L.win = W_APART;
				break;
			default: // mideast, southasia, africa
				L.wall = r () < 0.7f ? M_PLASTER : M_CONCRETE, L.tint = PickCol (r, EARTH);
				L.shape = S_FLAT, L.win = region == R_MIDEAST ? W_SMALL : W_APART;
				break;
			}
			if (storeys >= 10) { // apartment towers
				L.shape = S_PLANT, L.shop = false;
				if (region != R_SOVIET) L.win = region == R_EASTASIA || region == R_JAPAN ? W_ASIA : W_APART;
			}
			break; }
		case CITY_TOWER: {
			int k = pick ({ 0.55f, 0.3f, 0.15f });
			L.win = k == 0 ? W_CURTAIN : k == 1 ? W_GRID : W_BANDS;
			if (k == 0) L.wall = M_METAL, L.tint = PickCol (r, SPANDREL), L.coated = r () < 0.5f;
			else if (k == 1) L.wall = r () < 0.6f ? M_CONCRETE : M_STONE, L.tint = PickCol (r, NEUTRAL);
			else L.wall = M_METAL, L.tint = PickCol (r, METALC);
			float s = r ();
			L.shape = s < 0.4f ? S_PLANT : s < 0.65f ? S_SETBACK : s < 0.8f && h > 120.0 ? S_SETBACK2 : s < 0.9f ? S_PYRAMID : S_PLANT;
			if (L.shape == S_PYRAMID) L.roof = M_ROOF_METAL, L.roofCol = r () < 0.5f ? RC_PATINA : RC_MIDGREY;
			L.shop = r () < 0.5f;
			break; }
		case CITY_INDUSTRIAL:
		default:
			L.wall = M_METAL, L.tint = PickCol (r, METALC), L.win = W_INDUSTRIAL;
			if (r () < 0.6f) L.shape = S_SHED, L.roof = M_ROOF_METAL, L.roofCol = r () < 0.5f ? RC_NONE : RC_LIGHTGREY;
			else L.shape = S_FLAT;
			break;
		}
		return L;
	}

} // namespace

// =======================================================================

Kit::Kit (vkc::Context *c, TextureManager *t): ctx (c), tm (t)
{
	MakeShapes ();
	MakeTextures ();
}

Kit::~Kit ()
{
	vkc::Buffer b0 = vb, b1 = ib;
	vkc::Context *cx = ctx;
	ctx->DeferDestroy ([cx, b0, b1]() mutable { cx->DestroyBuffer (b0); cx->DestroyBuffer (b1); });
	if (tex) {
		TextureManager *t = tm;
		VkTexture *x = tex;
		ctx->DeferDestroy ([t, x]() { t->Release (x); });
	}
}

VkDescriptorSet Kit::Set () const { return tex ? tex->set : VK_NULL_HANDLE; }

int Kit::Far (int shape)
{
	switch (shape) {
	case S_FLAT: case S_PLANT: return S_BOX;
	default: return shape;
	}
}

void Kit::RTBoxes (int shape, const BInst &bi, std::vector<float> &rows)
{
	float H = bi.dim[2];
	auto box = [&](float y0, float y1, float f) { // shares of the walls' height, footprint share
		float sc[3] = { bi.dim[0] * f, y1 - y0, bi.dim[1] * f };
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) rows.push_back (bi.row[i*4+j] * sc[j]);
			rows.push_back (bi.row[i*4+3] + bi.row[i*4+1] * y0);
		}
	};
	const float flat = 0.85f; // (flat roofs 0.8 m below the top)
	switch (shape) {
	case S_FLAT: case S_PLANT: box (0, H - flat, 1); break;
	case S_SETBACK: box (0, 0.72f * H - 0.05f, 1); box (0, H - flat, 0.76f); break;
	case S_SETBACK2: box (0, 0.55f * H - 0.05f, 1); box (0, 0.8f * H - 0.05f, 0.8f); box (0, H - flat, 0.56f); break;
	default: box (0, H - 0.05f, 1); break;
	}
}

void Kit::MakeShapes ()
{
	std::vector<BVertex> v;
	std::vector<uint16_t> idx;
	Builder B{ v, idx };
	for (int s = 0; s < NSHAPE; s++) {
		uint32_t first = (uint32_t)idx.size();
		switch (s) {
		case S_BOX:
			B.Walls (0, 1, 0.5f);
			B.Poly ({ Pt (-0.5f, 1, -0.5f), Pt (0.5f, 1, -0.5f), Pt (0.5f, 1, 0.5f), Pt (-0.5f, 1, 0.5f) }, P_ROOF_FLAT, 1, 0, 1, 0);
			break;
		case S_FLAT:
			B.Walls (0, 1, 0.5f);
			B.Parapet (0.5f);
			break;
		case S_PLANT:
			B.Walls (0, 1, 0.5f);
			B.Parapet (0.5f);
			B.Plant (0.12f, 0.15f, 2.8f);
			break;
		case S_GABLE:
			B.Walls (0, 1, 0.5f);
			B.Gable (0.45f, 0.3f);
			break;
		case S_SHED:
			B.Walls (0, 1, 0.5f);
			B.Gable (0.3f, 0.2f);
			break;
		case S_HIP: {
			const float o = 0.45f;
			auto eave = [&](float sx, float sz) { return Pt (0.5f * sx, 1, 0.5f * sz, sx * o, -0.15f, sz * o); };
			auto ridge = [&](float sx) { return Pt (0.5f * sx, 1, 0, 0, 0, 0, 1, -0.5f * sx); };
			B.Walls (0, 1, 0.5f);
			for (int side = -1; side <= 1; side += 2) {
				B.Poly ({ eave (-1, side), eave (1, side), ridge (1), ridge (-1) }, P_ROOF, 1, 0, 1, (float)side, false);
				B.Poly ({ eave (-1, side), eave (1, side), ridge (1), ridge (-1) }, P_SOFFIT, 1, 0, -1, (float)-side, false);
				B.Poly ({ eave (side, -1), eave (side, 1), ridge (side) }, P_ROOF, 1, (float)side, 1, 0, false);
				B.Poly ({ eave (side, -1), eave (side, 1), ridge (side) }, P_SOFFIT, 1, (float)-side, -1, 0, false);
			}
			break; }
		case S_MANSARD: { // steep lower slopes to a break 1.6 m in, a flat top
			const float in = 1.6f, o = 0.0f;
			auto eave = [&](float sx, float sz) { return Pt (0.5f * sx, 1, 0.5f * sz, sx * o, 0, sz * o); };
			auto brk = [&](float sx, float sz) { return Pt (0.5f * sx, 1, 0.5f * sz, -sx * in, 0, -sz * in, 1); };
			B.Walls (0, 1, 0.5f);
			for (int side = -1; side <= 1; side += 2) {
				B.Poly ({ eave (-1, side), eave (1, side), brk (1, side), brk (-1, side) }, P_ROOF, 1, 0, 0.4f, (float)side, false);
				B.Poly ({ eave (side, -1), eave (side, 1), brk (side, 1), brk (side, -1) }, P_ROOF, 1, (float)side, 0.4f, 0, false);
			}
			B.Poly ({ brk (-1, -1), brk (1, -1), brk (1, 1), brk (-1, 1) }, P_ROOF, 1, 0, 1, 0);
			break; }
		case S_SETBACK:
			B.Walls (0, 0.72f, 0.5f);
			B.Ring (0.72f, 0.5f, 0.38f, P_ROOF_FLAT);
			B.Walls (0.72f, 1, 0.38f);
			B.Parapet (0.38f);
			B.Plant (0.12f, 0.15f, 4.5f);
			break;
		case S_SETBACK2:
			B.Walls (0, 0.55f, 0.5f);
			B.Ring (0.55f, 0.5f, 0.4f, P_ROOF_FLAT);
			B.Walls (0.55f, 0.8f, 0.4f);
			B.Ring (0.8f, 0.4f, 0.28f, P_ROOF_FLAT);
			B.Walls (0.8f, 1, 0.28f);
			B.Parapet (0.28f);
			B.Plant (0.1f, 0.12f, 5.0f);
			break;
		case S_PYRAMID: {
			B.Walls (0, 1, 0.5f);
			P apex = Pt (0, 1, 0, 0, 0, 0, 1);
			B.Poly ({ Pt (-0.5f, 1, -0.5f), Pt (0.5f, 1, -0.5f), apex }, P_ROOF, 1, 0, 1, -1, false);
			B.Poly ({ Pt (0.5f, 1, -0.5f), Pt (0.5f, 1, 0.5f), apex }, P_ROOF, 1, 1, 1, 0, false);
			B.Poly ({ Pt (0.5f, 1, 0.5f), Pt (-0.5f, 1, 0.5f), apex }, P_ROOF, 1, 0, 1, 1, false);
			B.Poly ({ Pt (-0.5f, 1, 0.5f), Pt (-0.5f, 1, -0.5f), apex }, P_ROOF, 1, -1, 1, 0, false);
			break; }
		case S_BARREL: { // walls, a round roof across x (its roof share rising as sin), its ends closed
			const int N = 12;
			B.Walls (0, 1, 0.5f);
			for (int k = 0; k < N; k++) {
				float t0 = 3.14159265f * k / N, t1 = 3.14159265f * (k + 1) / N;
				P q[4] = { Pt (-0.5f * std::cos (t0), 1, -0.5f, 0, 0, 0, std::sin (t0)), Pt (-0.5f * std::cos (t1), 1, -0.5f, 0, 0, 0, std::sin (t1)),
					Pt (-0.5f * std::cos (t1), 1, 0.5f, 0, 0, 0, std::sin (t1)), Pt (-0.5f * std::cos (t0), 1, 0.5f, 0, 0, 0, std::sin (t0)) };
				float n[4][3] = { { -std::cos (t0), std::sin (t0), 0 }, { -std::cos (t1), std::sin (t1), 0 }, { -std::cos (t1), std::sin (t1), 0 }, { -std::cos (t0), std::sin (t0), 0 } };
				B.QuadN (q, n, P_ROOF, 1);
			}
			for (int side = -1; side <= 1; side += 2) { // (the ends above the walls: fans, convex)
				std::vector<P> e;
				for (int k = 0; k <= N; k++) {
					float t = 3.14159265f * k / N;
					e.push_back (Pt (-0.5f * std::cos (t), 1, 0.5f * side, 0, 0, 0, std::sin (t)));
				}
				for (int k = 1; k + 1 <= N; k++)
					B.Poly ({ e[0], e[k], e[k + 1] }, P_WALL, 1, 0, 0, (float)side);
			}
			break; }
		case S_ARCH: { // an arch from the ground along z: height (1 - (2x)^2) of the roof; its ends walls (windows, the door)
			const int N = 16;
			auto at = [&](int k, float z) { float x = -0.5f + (float)k / N; return Pt (x, 0, z, 0, 0, 0, 1.0f - 4.0f * x * x); };
			for (int k = 0; k < N; k++) {
				float x0 = -0.5f + (float)k / N, x1 = -0.5f + (float)(k + 1) / N;
				P q[4] = { at (k, -0.5f), at (k + 1, -0.5f), at (k + 1, 0.5f), at (k, 0.5f) };
				// (normals: as for a typical arch, its height about its half width)
				float n[4][3] = { { 8.0f * x0, 1, 0 }, { 8.0f * x1, 1, 0 }, { 8.0f * x1, 1, 0 }, { 8.0f * x0, 1, 0 } };
				B.QuadN (q, n, P_ROOF, 1);
			}
			for (int side = -1; side <= 1; side += 2)
				for (int k = 1; k + 1 <= N; k++) // (fans from the left foot: convex)
					B.Poly ({ at (0, 0.5f * side), at (k, 0.5f * side), at (k + 1, 0.5f * side) }, P_FACADE, 1, 0, 0, (float)side);
			break; }
		case S_CYL: { // a vertical cylinder: its wall (windows about it), a flat roof with a rim
			const int N = 32;
			for (int k = 0; k < N; k++) {
				float a0 = 6.2831853f * k / N, a1 = 6.2831853f * (k + 1) / N;
				P q[4] = { Pt (0.5f * std::cos (a0), 0, 0.5f * std::sin (a0)), Pt (0.5f * std::cos (a1), 0, 0.5f * std::sin (a1)),
					Pt (0.5f * std::cos (a1), 1, 0.5f * std::sin (a1)), Pt (0.5f * std::cos (a0), 1, 0.5f * std::sin (a0)) };
				float n[4][3] = { { std::cos (a0), 0, std::sin (a0) }, { std::cos (a1), 0, std::sin (a1) }, { std::cos (a1), 0, std::sin (a1) }, { std::cos (a0), 0, std::sin (a0) } };
				B.QuadN (q, n, P_CURVED, 1);
				// the rim (0.6 m) and the roof (a fan)
				P r[4] = { Pt (0.5f * std::cos (a0), 1, 0.5f * std::sin (a0)), Pt (0.5f * std::cos (a1), 1, 0.5f * std::sin (a1)),
					Pt (0.5f * std::cos (a1), 1, 0.5f * std::sin (a1), 0, 0.6f), Pt (0.5f * std::cos (a0), 1, 0.5f * std::sin (a0), 0, 0.6f) };
				B.QuadN (r, n, P_TRIM, 1);
				B.Poly ({ Pt (0, 1, 0, 0, 0.1f), Pt (0.5f * std::cos (a0), 1, 0.5f * std::sin (a0), -0.3f * std::cos (a0), 0.1f, -0.3f * std::sin (a0)),
					Pt (0.5f * std::cos (a1), 1, 0.5f * std::sin (a1), -0.3f * std::cos (a1), 0.1f, -0.3f * std::sin (a1)) }, P_ROOF_FLAT, 1, 0, 1, 0);
			}
			break; }
		}
		range[s] = { first, (uint32_t)idx.size() - first };
	}
	cv = v, cidx = idx;
	vb = ctx->CreateBuffer (v.size() * sizeof(BVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
	memcpy (vb.mapped, v.data(), v.size() * sizeof(BVertex));
	ib = ctx->CreateBuffer (idx.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
	memcpy (ib.mapped, idx.data(), idx.size() * sizeof(uint16_t));
}

void Kit::MakeTextures ()
{
	// the materials on worker threads
	std::vector<Layer> layers (NMAT);
	std::vector<std::future<void>> jobs;
	for (int k = 0; k < NMAT; k++)
		jobs.push_back (std::async (std::launch::async, [k, &layers]() { MakeMaterial (k, layers[k]); }));
	for (auto &j : jobs) j.wait ();

	// per material: albedo (a: 1), normal (x along u, y along v) and roughness
	std::vector<uint8_t> data ((size_t)TN * TN * 4 * 2 * NMAT);
	auto b8 = [](float x) { return (uint8_t)std::clamp ((int)std::lround (x * 255.0f), 0, 255); };
	for (int k = 0; k < NMAT; k++) {
		const Layer &L = layers[k];
		uint8_t *alb = &data[(size_t)TN * TN * 4 * (2 * k)], *nrm = alb + (size_t)TN * TN * 4;
		float px = TILE[k] / TN; // [m]
		for (int y = 0; y < TN; y++)
			for (int x = 0; x < TN; x++) {
				size_t i = (size_t)y * TN + x;
				auto H = [&](int xx, int yy) { return L.h[(size_t)((yy + TN) % TN) * TN + (xx + TN) % TN]; };
				float sx = (H (x + 1, y) - H (x - 1, y)) / (2.0f * px), sy = (H (x, y + 1) - H (x, y - 1)) / (2.0f * px);
				float l = std::sqrt (sx * sx + sy * sy + 1.0f);
				alb[i*4] = b8 (L.rgb[i*3]), alb[i*4+1] = b8 (L.rgb[i*3+1]), alb[i*4+2] = b8 (L.rgb[i*3+2]), alb[i*4+3] = 255;
				nrm[i*4] = b8 (-sx / l * 0.5f + 0.5f), nrm[i*4+1] = b8 (-sy / l * 0.5f + 0.5f), nrm[i*4+2] = b8 (L.rough[i]), nrm[i*4+3] = 255;
			}
	}
	tex = tm->CreateArrayLinear (TN, TN, 2 * NMAT, data.data());
}

void Kit::SetStyles (OBJHANDLE hPlanet)
{
	static const struct { const char *name; int region; } names[] = {
		{ "namerica", R_NAMERICA }, { "latin", R_LATIN }, { "europe", R_EUROPE }, { "soviet", R_SOVIET },
		{ "eastasia", R_EASTASIA }, { "japan", R_JAPAN }, { "southasia", R_SOUTHASIA }, { "mideast", R_MIDEAST },
		{ "africa", R_AFRICA }, { "oceania", R_OCEANIA } };
	styles.clear();
	for (int i = 0; ; i++) {
		const char *n = oapiCityStyle (hPlanet, i);
		if (!n) break;
		int reg = R_GENERIC;
		for (auto &x : names) if (!strcmp (n, x.name)) reg = x.region;
		styles.push_back (reg);
	}
}

int Kit::Make (const CITYBUILDING &b, BInst &inst, bool far) const
{
	Rnd r (b.seed);
	int region = b.style >= 0 && b.style < (int)styles.size() ? styles[b.style] : R_GENERIC;
	int storeys = std::max (1, b.storeys);
	Look L = Choose (region, b.kind, storeys, b.height - b.sunk, r);
	double w = b.width, d = b.depth;
	double sh = b.storeyHeight;
	double wallH = b.height, roofH = 0.0;
	// the frame's rows (x front, y up, z back); pitched roofs along the longer side
	double R[9];
	for (int i = 0; i < 9; i++) R[i] = b.rot.data[i];
	bool frontX = false;
	bool pitched = L.shape == S_GABLE || L.shape == S_HIP || L.shape == S_SHED;
	if (pitched && d > w) { // (turned a quarter about y: x' = z, z' = -x)
		for (int i = 0; i < 3; i++) {
			double x = R[i*3], z = R[i*3+2];
			R[i*3] = z, R[i*3+2] = -x;
		}
		std::swap (w, d);
		frontX = true;
	}
	switch (L.shape) {
	case S_GABLE: case S_HIP:
		wallH = b.sunk + storeys * sh;
		roofH = L.pitch * 0.5 * d;
		break;
	case S_SHED:
		wallH = b.sunk + sh;
		roofH = std::max (b.height - wallH, 0.06 * d);
		break;
	case S_MANSARD: // (the top storey in the roof)
		storeys = std::max (1, storeys - 1);
		wallH = b.sunk + storeys * sh;
		roofH = std::max (b.height - wallH, sh);
		break;
	case S_PYRAMID:
		roofH = std::min (0.12 * (b.height - b.sunk), 0.6 * std::min (w, d));
		break;
	}
	int shape = far ? Far (L.shape) : L.shape;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) inst.row[i*4+j] = (float)R[i*3+j];
		inst.row[i*4+3] = 0.0f;
	}
	inst.dim[0] = (float)w, inst.dim[1] = (float)d, inst.dim[2] = (float)wallH, inst.dim[3] = (float)roofH;
	inst.prm[0] = (float)sh, inst.prm[1] = (float)b.sunk;
	inst.bits[0] = (uint32_t)std::min (storeys, 255) | ((uint32_t)L.wall << 8) | ((uint32_t)L.roof << 12) | ((uint32_t)L.win << 16)
		| ((uint32_t)L.roofCol << 20) | (L.shop ? 1u << 24 : 0u) | (L.door ? 1u << 25 : 0u) | (L.coated ? 1u << 26 : 0u)
		| (frontX ? 1u << 27 : 0u) | ((uint32_t)L.deco << 28);
	inst.bits[1] = Pack565 (L.tint, r (0.9f, 1.05f)) | ((b.seed & 0xffffu) << 16);
	return shape;
}

// =======================================================================
// Bases' generic buildings

namespace {
	// the looks of the bases' legacy textures (32 px facades: what they showed)
	struct TexLook { const char *name; int wall; int win; Col tint; int deco; };
	const TexLook TEXLOOK[] = {
		{ "fcd01", M_PLASTER, W_APART, { 0.95f, 0.88f, 0.70f }, 0 },
		{ "fcd02", M_CONCRETE, W_PANEL, { 0.92f, 0.84f, 0.68f }, 1 },
		{ "fcd03", M_METAL, W_BANDS, { 0.82f, 0.36f, 0.30f }, 0 },
		{ "fcd04", M_CONCRETE, W_GRID, { 0.95f, 0.95f, 0.95f }, 0 },
		{ "fcd05", M_METAL, W_GRID, { 0.95f, 0.95f, 0.95f }, 0 },
		{ "fcd06", M_CONCRETE, W_GRID, { 0.85f, 0.85f, 0.86f }, 0 },
		{ "fcd07", M_METAL, W_CURTAIN, { 0.92f, 0.92f, 0.92f }, 0 },
		{ "fcd08", M_BRICK, W_GRID, { 0.85f, 0.75f, 0.70f }, 0 },
		{ "fcd09", M_BRICK_BUFF, W_APART, { 1.0f, 0.82f, 0.62f }, 0 },
		{ "fcd10", M_METAL, W_CURTAIN, { 0.35f, 0.45f, 0.48f }, 0 },
		{ "fcd11", M_CONCRETE, W_BANDS, { 0.92f, 0.90f, 0.86f }, 0 },
		{ "fcd12", M_STONE, W_CLASSIC, { 0.92f, 0.94f, 1.0f }, 0 },
		{ "fcd13", M_STONE, W_CLASSIC, { 1.0f, 0.98f, 0.96f }, 0 },
		{ "fcd14", M_PLASTER, W_STRIP, { 1.0f, 1.0f, 1.0f }, 0 },
		{ "fcd15", M_PLASTER, W_SMALL, { 1.0f, 1.0f, 1.0f }, 0 },
		{ "wall01", M_PANEL, W_INDUSTRIAL, { 1.0f, 1.0f, 0.98f }, 0 },
		{ "door01", M_METAL, W_INDUSTRIAL, { 0.62f, 0.74f, 0.60f }, 0 },
		{ "concrete1", M_CONCRETE, W_NONE, { 0.95f, 0.95f, 0.95f }, 0 },
	};
	const TexLook *FindLook (const char *name)
	{
		std::string n (name);
		for (auto &c : n) c = (char)tolower ((unsigned char)c);
		for (auto &t : TEXLOOK) if (n == t.name) return &t;
		return nullptr;
	}
}

int Kit::MakeBase (const BASEBUILDINGSPEC &b, const BaseEnv &env, BInst &inst) const
{
	Rnd r (0x5eedu ^ (b.index * 2654435761u) ^ (uint32_t)(b.pos.x * 13.0) ^ (uint32_t)(b.pos.z * 7.0));
	double c = cos (b.rot), s = sin (b.rot);
	// the building frame's axes in the base frame (x, z of the core's: x = cos x' - sin z', z = sin x' + cos z')
	double ax[3] = { c, 0, s }, az[3] = { -s, 0, c };
	double w = b.size.x, d = b.size.z, wallH = b.wallh, roofH = 0.0;
	int shape = S_FLAT;
	bool frontX = false;
	switch (b.type) {
	case BLDG_BLOCK: shape = S_FLAT; wallH = b.size.y; break;
	case BLDG_HANGAR: shape = S_BARREL; roofH = b.size.y - wallH; break;
	case BLDG_GABLE: // (Kit's gable: its ridge along x - turned a quarter: x' = z, z' = -x; the door on a gable end, -x)
		shape = S_GABLE, roofH = b.size.y - wallH, frontX = true;
		for (int i = 0; i < 3; i++) { double x = ax[i]; ax[i] = az[i], az[i] = -x; }
		std::swap (w, d);
		break;
	case BLDG_ARCH: // (the door at +z: turned half about y, the front -z)
		shape = S_ARCH, wallH = 0.0, roofH = b.size.y;
		for (int i = 0; i < 3; i++) ax[i] = -ax[i], az[i] = -az[i];
		break;
	case BLDG_TANK: shape = S_CYL, w = d = 2.0 * b.size.x, wallH = b.size.y; break;
	}
	// looks: the walls' texture (the tank's mantle), else by kind
	const TexLook *tl = FindLook (b.tex[0]);
	int wall = tl ? tl->wall : M_CONCRETE, win = tl ? tl->win : W_INDUSTRIAL, deco = tl ? tl->deco : 0;
	Col tint = tl ? tl->tint : Col { 0.9f, 0.9f, 0.88f };
	int roof = M_ROOF_FLAT, roofCol = RC_NONE;
	const char *rt = b.type == BLDG_TANK ? b.tex[1] : b.tex[2];
	std::string rtl (rt);
	for (auto &ch : rtl) ch = (char)tolower ((unsigned char)ch);
	if (rtl == "roof01") roof = M_ROOF_METAL, roofCol = RC_LIGHTGREY;
	bool door = false;
	double sh = 3.6; // storey height [m]
	if (b.type == BLDG_HANGAR || b.type == BLDG_GABLE || (b.type == BLDG_BLOCK && win == W_INDUSTRIAL)) {
		if (b.type != BLDG_BLOCK) win = W_HANGAR, roof = M_ROOF_METAL, roofCol = r () < 0.5f ? RC_LIGHTGREY : RC_NONE;
		if (wall == M_PLASTER || wall == M_STONE || wall == M_CONCRETE) wall = M_PANEL;
		sh = std::max (wallH, 4.0);
	}
	if (b.type == BLDG_TANK) { // (storage, unless faced: a hub)
		if (!tl || win == W_INDUSTRIAL || win == W_NONE) win = W_NONE, wall = M_METAL, tint = { 0.92f, 0.92f, 0.9f };
		roof = M_ROOF_METAL;
	}
	if (b.type == BLDG_ARCH) win = W_HANGAR, roof = M_ROOF_METAL;
	if (env.vacuum || env.insulated) {
		// pressurised modules: white (or insulation) panels, portholes, airlocks; long low
		// halls and arches under regolith (shielding), the soil's colour
		Col panel = env.insulated ? Col { 0.95f, 0.78f, 0.55f } : Col { 0.92f, 0.92f, 0.90f };
		bool tunnel = b.type == BLDG_HANGAR && std::max (w, d) > 5.0 * std::min (w, d);
		if (tunnel || b.type == BLDG_ARCH) {
			wall = tunnel ? M_REGOLITH : M_PANEL, roof = M_REGOLITH, deco = 2; // (deco 2: the roof regolith, the wall tint its colour; the ends' panels white)
			tint = { env.soil[0] / 0.62f, env.soil[1] / 0.62f, env.soil[2] / 0.62f };
			win = tunnel ? W_NONE : W_HANGAR;
			door = !tunnel;
		} else if (b.type == BLDG_TANK) {
			wall = M_PANEL, roof = M_ROOF_METAL, tint = panel;
			if (win != W_NONE) win = W_PORTHOLE, door = true;
		} else {
			wall = M_PANEL, tint = panel, roof = M_ROOF_METAL, roofCol = RC_LIGHTGREY;
			if (b.type == BLDG_BLOCK) win = W_PORTHOLE, door = true, roof = M_ROOF_FLAT;
			else win = W_HANGAR;
		}
		sh = b.type == BLDG_BLOCK || b.type == BLDG_TANK ? 3.2 : std::max (wallH, 4.0);
	}
	double hTot = wallH + roofH;
	int storeys = std::max (1, (int)std::floor ((b.type == BLDG_ARCH ? 0.7 * hTot : wallH) / sh + 0.25));
	if (win == W_HANGAR || win == W_INDUSTRIAL) storeys = 1, sh = std::max (b.type == BLDG_ARCH ? 0.6 * hTot : wallH, 3.0);
	for (int i = 0; i < 3; i++) {
		inst.row[i*4+0] = (float)ax[i], inst.row[i*4+1] = i == 1 ? 1.0f : 0.0f, inst.row[i*4+2] = (float)az[i];
	}
	inst.row[3] = (float)b.pos.x, inst.row[7] = (float)b.pos.y, inst.row[11] = (float)b.pos.z;
	inst.dim[0] = (float)w, inst.dim[1] = (float)d, inst.dim[2] = (float)wallH, inst.dim[3] = (float)roofH;
	inst.prm[0] = (float)sh, inst.prm[1] = 0.0f;
	inst.bits[0] = (uint32_t)std::min (storeys, 255) | ((uint32_t)wall << 8) | ((uint32_t)std::min (roof, 15) << 12) | ((uint32_t)win << 16)
		| ((uint32_t)roofCol << 20) | (door ? 1u << 25 : 0u) | (frontX ? 1u << 27 : 0u) | ((uint32_t)deco << 28);
	inst.bits[1] = Pack565 (tint, deco == 2 ? 1.0f : r (0.94f, 1.03f)) | (((uint32_t)b.index * 7919u & 0xffffu) << 16);
	return shape;
}

void Kit::BaseLights (int shape, const BInst &bi, std::vector<BaseLamp> &out) const
{
	const uint32_t bits = bi.bits[0];
	const int win = (int)((bits >> 16) & 15u), deco = (int)(bits >> 28);
	const bool door = (bits & (1u << 25)) != 0u, frontX = (bits & (1u << 27)) != 0u;
	const double w = bi.dim[0], d = bi.dim[1], wallH = bi.dim[2], roofH = bi.dim[3], hTot = wallH + roofH;
	// (a point of the building frame in the base frame; a direction)
	auto at = [&](double x, double y, double z, double *p) {
		for (int i = 0; i < 3; i++) p[i] = bi.row[i*4] * x + bi.row[i*4+1] * y + bi.row[i*4+2] * z + bi.row[i*4+3];
	};
	auto dirn = [&](double x, double y, double z, double *p) {
		for (int i = 0; i < 3; i++) p[i] = bi.row[i*4] * x + bi.row[i*4+1] * y + bi.row[i*4+2] * z;
		double l = std::sqrt (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		for (int i = 0; i < 3; i++) p[i] /= l;
	};
	auto lamp = [&](double x, double y, double z, const float *col, float size) {
		BaseLamp l{};
		at (x, y, z, l.pos);
		for (int k = 0; k < 3; k++) l.col[k] = col[k];
		l.size = size;
		out.push_back (l);
	};
	const float WARM[3] = { 1.0f, 0.86f, 0.62f }, AMBER[3] = { 1.0f, 0.55f, 0.12f }, RED[3] = { 1.0f, 0.08f, 0.04f };
	// the front: -z (-x: frontX); along it s, out of it n (building frame)
	const double fw = frontX ? d : w, half = frontX ? 0.5 * w : 0.5 * d;
	auto front = [&](double s, double y, double out_, double *p) {
		if (frontX) at (-half - out_, y, s, p); else at (s, y, -half - out_, p);
	};
	auto frontLamp = [&](double s, double y, const float *col, float size) {
		BaseLamp l{};
		front (s, y, 0.4, l.pos);
		for (int k = 0; k < 3; k++) l.col[k] = col[k];
		l.size = size;
		out.push_back (l);
	};
	const bool curved = shape == S_CYL;
	if (win == W_HANGAR && !curved) {
		// (the door as building.frag draws it)
		double H = wallH > 0.0 ? wallH : roofH;
		double hh = std::min (0.86 * (wallH > 0.0 ? wallH : 0.62 * roofH), 16.0);
		double hw = wallH > 0.0 ? 0.42 * fw : 0.5 * fw * std::sqrt (std::max (1.0 - hh / std::max (H, 1.0), 0.0)) - 0.6;
		if (hw > 1.0) {
			frontLamp (-hw, hh + 0.8, WARM, 0.7f);
			frontLamp (hw, hh + 0.8, WARM, 0.7f);
			// (a floodlight over the door: the apron before it)
			BaseLamp f{};
			front (0.0, hh + 1.5, 1.5, f.pos);
			if (frontX) dirn (-1.0, -1.1, 0.0, f.dir); else dirn (0.0, -1.1, -1.0, f.dir);
			f.col[0] = 3.0f, f.col[1] = 2.8f, f.col[2] = 2.5f;
			f.size = (float)std::clamp (hw * 0.12, 1.0, 3.0);
			f.flood = true, f.range = 150.0, f.umbra = 70.0, f.penumbra = 110.0;
			out.push_back (f);
		}
	} else if (door) {
		// (an airlock's or a door's lamp: over the door, in the front's middle bay)
		if (curved) lamp (0.0, 2.8, -0.5 * d - 0.4, WARM, 0.5f); // (a hub: on its round wall, towards -z)
		else frontLamp (0.0, 2.8, WARM, 0.5f);
	}
	// a covered tunnel: markers along its ridge
	if (shape == S_BARREL && win == W_NONE && deco == 2) {
		const bool alongX = w > d;
		const double L = alongX ? w : d;
		const int n = std::max (1, (int)(L / 18.0));
		for (int k = 0; k <= n; k++) {
			double t = -0.5 * L + 1.0 + (L - 2.0) * k / n;
			if (alongX) lamp (t, hTot + 0.15, 0.0, AMBER, 0.35f); else lamp (0.0, hTot + 0.15, t, AMBER, 0.35f);
		}
	}
	// obstruction lights: on roofs 40 m up and higher (their corners), and on a pressurised hub (its top)
	if (hTot >= 40.0) {
		if (curved) lamp (0.0, hTot + 0.5, 0.0, RED, 1.0f);
		else for (int sx = -1; sx <= 1; sx += 2) for (int sz = -1; sz <= 1; sz += 2)
			lamp (sx * (0.5 * w - 0.5), hTot + 0.5, sz * (0.5 * d - 0.5), RED, 1.0f);
	} else if (win == W_PORTHOLE && hTot >= 6.0) {
		lamp (0.0, hTot + 0.6, 0.0, RED, 0.6f);
		if (curved) { // (a hub: a wash over it - the ground of the corridors' junction about it)
			BaseLamp f{};
			at (0.0, hTot + 6.0, 0.0, f.pos);
			dirn (0.0, -1.0, 0.0, f.dir);
			f.col[0] = 5.0f, f.col[1] = 4.8f, f.col[2] = 4.4f;
			f.size = 1.5f;
			f.flood = true, f.range = 300.0, f.umbra = 120.0, f.penumbra = 165.0;
			out.push_back (f);
		}
	}
}

void Kit::Expand (int shape, const BInst &bi, std::vector<NTVERTEX> &vtx, std::vector<uint16_t> &idx) const
{
	const Range &rg = range[shape];
	uint16_t base = (uint16_t)vtx.size();
	// (the shape's vertices are shared within it: its index range's span)
	uint32_t lo = 0xffffffffu, hi = 0;
	for (uint32_t i = 0; i < rg.count; i++) lo = std::min (lo, (uint32_t)cidx[rg.first + i]), hi = std::max (hi, (uint32_t)cidx[rg.first + i]);
	if (rg.count == 0) return;
	for (uint32_t k = lo; k <= hi; k++) {
		const BVertex &v = cv[k];
		float p[3] = { v.unit[0] * bi.dim[0] + v.shape[1] * bi.dim[1] + v.metric[0],
			v.unit[1] * bi.dim[2] + v.shape[0] * bi.dim[3] + v.metric[1],
			v.unit[2] * bi.dim[1] + v.metric[2] };
		NTVERTEX o{};
		float q[3], n[3];
		for (int i = 0; i < 3; i++) {
			q[i] = bi.row[i*4] * p[0] + bi.row[i*4+1] * p[1] + bi.row[i*4+2] * p[2] + bi.row[i*4+3];
			n[i] = bi.row[i*4] * v.normal[0] + bi.row[i*4+1] * v.normal[1] + bi.row[i*4+2] * v.normal[2];
		}
		o.x = q[0], o.y = q[1], o.z = q[2], o.nx = n[0], o.ny = n[1], o.nz = n[2];
		if (n[0] == 0.0f && n[1] == 0.0f && n[2] == 0.0f) o.ny = 1.0f; // (sloped parts: an up; shadows and rays don't read it)
		vtx.push_back (o);
	}
	for (uint32_t i = 0; i < rg.count; i++) idx.push_back ((uint16_t)(base + cidx[rg.first + i] - lo));
}

} // namespace vkcity

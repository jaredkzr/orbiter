// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ShoreField.h"
#include "LegacyTex.h"
#include "OrbiterAPI.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace vkshore {

namespace {
	inline uint32_t U32 (const uint8_t *p) { uint32_t v; memcpy (&v, p, 4); return v; }
	inline void P32 (uint8_t *p, uint32_t v) { memcpy (p, &v, 4); }

	const double BIG = 1e20; // (no source: a large finite "distance" keeps the arithmetic exact enough)

	// squared Euclidean distance transform of one row/column (Felzenszwalb &
	// Huttenlocher 2012): d[i] = min_j (f[j] + (i - j)^2)
	void Edt1 (const double *f, double *d, int n, int *v, double *z)
	{
		int k = 0;
		v[0] = 0, z[0] = -1e300, z[1] = 1e300;
		for (int q = 1; q < n; q++) {
			double s = ((f[q] + (double)q * q) - (f[v[k]] + (double)v[k] * v[k])) / (2.0 * (q - v[k]));
			while (s <= z[k]) {
				k--;
				s = ((f[q] + (double)q * q) - (f[v[k]] + (double)v[k] * v[k])) / (2.0 * (q - v[k]));
			}
			k++;
			v[k] = q, z[k] = s, z[k + 1] = 1e300;
		}
		k = 0;
		for (int q = 0; q < n; q++) {
			while (z[k + 1] < q) k++;
			d[q] = (double)(q - v[k]) * (q - v[k]) + f[v[k]];
		}
	}

	// distance [texels] from every texel to the nearest texel with src true
	void Edt (const std::vector<uint8_t> &src, int w, int h, std::vector<float> &out)
	{
		int n = std::max (w, h);
		std::vector<double> f (n), d (n), z (n + 1), g ((size_t)w * h);
		std::vector<int> v (n);
		for (int x = 0; x < w; x++) {
			for (int y = 0; y < h; y++) f[y] = src[(size_t)y * w + x] ? 0.0 : BIG;
			Edt1 (f.data(), d.data(), h, v.data(), z.data());
			for (int y = 0; y < h; y++) g[(size_t)y * w + x] = d[y];
		}
		out.resize ((size_t)w * h);
		for (int y = 0; y < h; y++) {
			Edt1 (&g[(size_t)y * w], d.data(), w, v.data(), z.data());
			for (int x = 0; x < w; x++) out[(size_t)y * w + x] = (float)std::sqrt (std::min (d[x], 1e12));
		}
	}

	inline uint16_t To565 (const float c[3])
	{
		int r = std::clamp ((int)std::lround (c[0] * 31.0f / 255.0f), 0, 31);
		int g = std::clamp ((int)std::lround (c[1] * 63.0f / 255.0f), 0, 63);
		int b = std::clamp ((int)std::lround (c[2] * 31.0f / 255.0f), 0, 31);
		return (uint16_t)(r << 11 | g << 5 | b);
	}
	inline void From565 (uint16_t v, float c[3])
	{
		int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
		c[0] = (float)(r << 3 | r >> 2), c[1] = (float)(g << 2 | g >> 4), c[2] = (float)(b << 3 | b >> 2);
	}

	// one 4 x 4 block -> BC3 (16 bytes): alpha block (8 values between max and min), colour block
	// (four colours, the end points the extremes along the block's largest colour axis)
	void EncodeBC3 (const uint8_t px[16][4], uint8_t *out)
	{
		// alpha
		int amax = 0, amin = 255;
		for (int i = 0; i < 16; i++) amax = std::max (amax, (int)px[i][3]), amin = std::min (amin, (int)px[i][3]);
		out[0] = (uint8_t)amax, out[1] = (uint8_t)amin;
		uint64_t abits = 0;
		if (amax > amin) {
			float ap[8] = { (float)amax, (float)amin };
			for (int k = 2; k < 8; k++) ap[k] = ((8 - k) * amax + (k - 1) * amin) / 7.0f;
			for (int i = 0; i < 16; i++) {
				int best = 0;
				float bd = 1e9f;
				for (int k = 0; k < 8; k++) {
					float e = std::fabs (ap[k] - px[i][3]);
					if (e < bd) bd = e, best = k;
				}
				abits |= (uint64_t)best << (3 * i);
			}
		}
		for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(abits >> (8 * k));
		// colour
		float mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
		for (int i = 0; i < 16; i++)
			for (int c = 0; c < 3; c++) mn[c] = std::min (mn[c], (float)px[i][c]), mx[c] = std::max (mx[c], (float)px[i][c]);
		uint16_t c0 = To565 (mx), c1 = To565 (mn);
		if (c0 < c1) std::swap (c0, c1);
		uint8_t *cb = out + 8;
		cb[0] = (uint8_t)c0, cb[1] = (uint8_t)(c0 >> 8), cb[2] = (uint8_t)c1, cb[3] = (uint8_t)(c1 >> 8);
		uint32_t bits = 0;
		if (c0 != c1) {
			float e0[3], e1[3], pal[4][3];
			From565 (c0, e0), From565 (c1, e1);
			for (int c = 0; c < 3; c++) {
				pal[0][c] = e0[c], pal[1][c] = e1[c];
				pal[2][c] = (2.0f * e0[c] + e1[c]) / 3.0f, pal[3][c] = (e0[c] + 2.0f * e1[c]) / 3.0f;
			}
			for (int i = 0; i < 16; i++) {
				int best = 0;
				float bd = 1e30f;
				for (int k = 0; k < 4; k++) {
					float e = 0.0f;
					for (int c = 0; c < 3; c++) { float t = pal[k][c] - px[i][c]; e += t * t; }
					if (e < bd) bd = e, best = k;
				}
				bits |= (uint32_t)best << (2 * i);
			}
		}
		cb[4] = (uint8_t)bits, cb[5] = (uint8_t)(bits >> 8), cb[6] = (uint8_t)(bits >> 16), cb[7] = (uint8_t)(bits >> 24);
	}
}

bool Enabled ()
{
	static const bool on = [] { const char *e = getenv ("ORBITER_VK_SHORE_FIELD"); return !(e && *e && !atoi (e)); }();
	return on;
}

bool EncodeShoreField (std::vector<uint8_t> &dds)
{
	if (!Enabled ()) return false;
	if (dds.size() < 128 || memcmp (dds.data(), "DDS ", 4)) return false;
	const uint8_t *hd = dds.data();
	uint32_t h = U32 (hd + 12), w = U32 (hd + 16), pf = U32 (hd + 80), fourCC = U32 (hd + 84), bits = U32 (hd + 88);
	if (w < 4 || h < 4 || w > 8192 || h > 8192) return false;
	bool compressed = (pf & 4) != 0;
	std::vector<uint8_t> rgba;
	if (compressed) {
		uint32_t ww, hh;
		if (!legacytex::Decode (dds, ww, hh, rgba) || ww != w || hh != h) return false;
	} else {
		// (the procedural masks: RGBA8, R at byte 0, alpha mask 0xFF000000)
		if (bits != 32 || !(pf & 1) || U32 (hd + 104) != 0xFF000000u || dds.size() < 128 + (size_t)w * h * 4) return false;
		rgba.assign (dds.begin() + 128, dds.begin() + 128 + (size_t)w * h * 4);
	}
	(void)fourCC;
	const size_t N = (size_t)w * h;
	std::vector<uint8_t> land (N), water (N);
	bool anyLand = false, anyWater = false;
	for (size_t i = 0; i < N; i++) {
		land[i] = rgba[4 * i + 3] >= 128;
		water[i] = !land[i];
		anyLand |= land[i] != 0, anyWater |= water[i] != 0;
	}
	std::vector<uint8_t> a8 (N);
	if (!anyLand || !anyWater) {
		std::fill (a8.begin(), a8.end(), anyLand ? 255 : 0);
	} else {
		std::vector<float> dLand, dWater;
		Edt (land, (int)w, (int)h, dLand);   // (water texels: to the nearest land)
		Edt (water, (int)w, (int)h, dWater); // (land texels: to the nearest water)
		for (size_t i = 0; i < N; i++) {
			// the edge lies half way between a land and a water texel; texels with a share
			// of water (the procedural masks' coasts) put it within themselves
			float a = rgba[4 * i + 3] / 255.0f;
			float sd = land[i] ? dWater[i] - 0.5f : -(dLand[i] - 0.5f);
			if (a > 0.0f && a < 1.0f && std::fabs (sd) <= 0.5f) sd = a - 0.5f;
			float v = 0.5f + sd / (2.0f * SD_MAX);
			a8[i] = (uint8_t)std::clamp ((int)std::lround (v * 255.0f), 0, 255);
		}
	}
	{
		static std::atomic<int> logged { 0 };
		if (getenv ("ORBITER_VK_SHORE_LOG") && logged++ < 20)
			oapiWriteLogV ("VulkanClient: shore field: %ux%u %s, land %d water %d", w, h, compressed ? "DXT -> BC3" : "RGBA8", anyLand ? 1 : 0, anyWater ? 1 : 0);
	}
	if (!compressed) {
		for (size_t i = 0; i < N; i++) dds[128 + 4 * i + 3] = a8[i];
		return true;
	}
	// BC3: header as the source's (one level), the blocks re-encoded
	uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
	std::vector<uint8_t> out (128 + (size_t)bw * bh * 16);
	memcpy (out.data(), dds.data(), 128);
	uint8_t *oh = out.data();
	P32 (oh + 8, U32 (oh + 8) & ~0x20000u);            // (no mip map count)
	P32 (oh + 20, bw * bh * 16);                        // linear size
	P32 (oh + 28, 1);                                   // mip levels
	P32 (oh + 80, 4);                                   // DDPF_FOURCC
	P32 (oh + 84, 0x35545844);                          // 'DXT5'
	P32 (oh + 108, U32 (oh + 108) & ~0x400008u);        // (no complex/mip caps)
	uint8_t *ob = out.data() + 128;
	for (uint32_t by = 0; by < bh; by++)
		for (uint32_t bx = 0; bx < bw; bx++, ob += 16) {
			uint8_t px[16][4];
			for (int i = 0; i < 16; i++) {
				uint32_t x = std::min (bx * 4 + (i & 3), w - 1), y = std::min (by * 4 + i / 4, h - 1);
				size_t k = (size_t)y * w + x;
				px[i][0] = rgba[4 * k], px[i][1] = rgba[4 * k + 1], px[i][2] = rgba[4 * k + 2], px[i][3] = a8[k];
			}
			EncodeBC3 (px, ob);
		}
	dds.swap (out);
	return true;
}

}

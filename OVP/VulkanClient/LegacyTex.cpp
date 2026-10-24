// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "LegacyTex.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace legacytex {

namespace {
	uint32_t U32 (const uint8_t *p) { uint32_t v; memcpy (&v, p, 4); return v; }

	// size of a DDS file starting at p (header and all mip levels), 0 if invalid
	size_t ImageSize (const uint8_t *p, size_t avail)
	{
		if (avail < 128 || memcmp (p, "DDS ", 4)) return 0;
		uint32_t h = U32 (p + 12), w = U32 (p + 16), mips = U32 (p + 28);
		uint32_t pfFlags = U32 (p + 80), fourCC = U32 (p + 84), bits = U32 (p + 88);
		if (!w || !h) return 0;
		size_t size = 0;
		for (uint32_t i = 0; i < (mips ? mips : 1); i++) {
			uint32_t mw = std::max (1u, w >> i), mh = std::max (1u, h >> i);
			if (pfFlags & 4) // FourCC: block compressed
				size += (size_t)((mw + 3) / 4) * ((mh + 3) / 4) * (fourCC == 0x31545844 /*DXT1*/ ? 8 : 16);
			else
				size += (size_t)mw * mh * bits / 8;
		}
		return 128 + size <= avail ? 128 + size : 0;
	}

	void Colour565 (uint16_t c, int rgb[3])
	{
		rgb[0] = ((c >> 11) & 31) * 255 / 31;
		rgb[1] = ((c >> 5) & 63) * 255 / 63;
		rgb[2] = (c & 31) * 255 / 31;
	}

	// colour part of a BC block (8 bytes) into a 4x4 RGBA tile
	void DecodeColour (const uint8_t *b, bool dxt1, uint8_t out[16][4])
	{
		uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
		int p[4][4];
		Colour565 (c0, p[0]); Colour565 (c1, p[1]);
		p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
		for (int k = 0; k < 3; k++) {
			if (!dxt1 || c0 > c1) {
				p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
				p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
			} else {
				p[2][k] = (p[0][k] + p[1][k]) / 2;
				p[3][k] = 0;
			}
		}
		if (dxt1 && c0 <= c1) p[3][3] = 0;
		uint32_t bits = U32 (b + 4);
		for (int i = 0; i < 16; i++)
			for (int k = 0; k < 4; k++) out[i][k] = (uint8_t)p[(bits >> (2 * i)) & 3][k];
	}
}

bool Read (const std::string &path, std::vector<std::vector<uint8_t>> &images)
{
	std::ifstream f (path, std::ios::binary);
	if (!f) return false;
	std::vector<uint8_t> d ((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	images.clear();
	for (size_t p = 0; p < d.size(); ) {
		size_t n = ImageSize (d.data() + p, d.size() - p);
		if (!n) break;
		images.emplace_back (d.begin() + p, d.begin() + p + n);
		p += n;
	}
	return !images.empty();
}

bool Decode (const std::vector<uint8_t> &dds, uint32_t &w, uint32_t &h, std::vector<uint8_t> &rgba)
{
	if (dds.size() < 128) return false;
	const uint8_t *p = dds.data();
	h = U32 (p + 12), w = U32 (p + 16);
	uint32_t pfFlags = U32 (p + 80), fourCC = U32 (p + 84);
	if (!(pfFlags & 4)) return false;
	int type = fourCC == 0x31545844 ? 1 : fourCC == 0x33545844 ? 3 : fourCC == 0x35545844 ? 5 : 0;
	if (!type) return false;
	size_t bs = type == 1 ? 8 : 16;
	uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
	if (dds.size() < 128 + bs * bw * bh) return false;
	rgba.assign ((size_t)w * h * 4, 0);
	const uint8_t *b = p + 128;
	for (uint32_t by = 0; by < bh; by++)
		for (uint32_t bx = 0; bx < bw; bx++, b += bs) {
			uint8_t tile[16][4];
			DecodeColour (type == 1 ? b : b + 8, type == 1, tile);
			if (type == 3) {
				for (int i = 0; i < 16; i++) tile[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
			} else if (type == 5) {
				int a[8] = { b[0], b[1] };
				for (int i = 2; i < 8; i++)
					a[i] = a[0] > a[1] ? ((8 - i) * a[0] + (i - 1) * a[1]) / 7
					     : i < 6 ? ((6 - i) * a[0] + (i - 1) * a[1]) / 5 : (i == 6 ? 0 : 255);
				uint64_t bits = 0;
				for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
				for (int i = 0; i < 16; i++) tile[i][3] = (uint8_t)a[(bits >> (3 * i)) & 7];
			}
			for (int i = 0; i < 16; i++) {
				uint32_t x = bx * 4 + (i & 3), y = by * 4 + i / 4;
				if (x < w && y < h) memcpy (&rgba[((size_t)y * w + x) * 4], tile[i], 4);
			}
		}
	return true;
}

bool GlobalMap (const std::vector<std::vector<uint8_t>> &images, uint32_t &w, uint32_t &h, std::vector<uint8_t> &rgba)
{
	if (images.size() >= 5) { // level 4: western and eastern hemisphere
		uint32_t w0, h0, w1, h1;
		std::vector<uint8_t> a, b;
		if (Decode (images[3], w0, h0, a) && Decode (images[4], w1, h1, b) && w0 == w1 && h0 == h1) {
			w = 2 * w0, h = h0;
			rgba.resize ((size_t)w * h * 4);
			for (uint32_t y = 0; y < h; y++) {
				memcpy (&rgba[(size_t)y * w * 4], &a[(size_t)y * w0 * 4], (size_t)w0 * 4);
				memcpy (&rgba[((size_t)y * w + w0) * 4], &b[(size_t)y * w0 * 4], (size_t)w0 * 4);
			}
			return true;
		}
	}
	for (int i = (int)std::min<size_t> (images.size(), 3) - 1; i >= 0; i--)
		if (Decode (images[i], w, h, rgba)) return true;
	return false;
}

} // namespace legacytex

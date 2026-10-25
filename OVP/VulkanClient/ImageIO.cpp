// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ImageIO.h"
#include <zlib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

	void Put32 (std::vector<uint8_t> &out, uint32_t v)
	{
		out.push_back ((uint8_t)(v >> 24));
		out.push_back ((uint8_t)(v >> 16));
		out.push_back ((uint8_t)(v >> 8));
		out.push_back ((uint8_t)v);
	}

	// PNG chunk: length, type, data, CRC over type and data
	void Chunk (std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
	{
		Put32 (out, (uint32_t)data.size());
		size_t start = out.size();
		out.insert (out.end(), type, type + 4);
		out.insert (out.end(), data.begin(), data.end());
		uLong crc = crc32 (0L, out.data() + start, (uInt)(out.size() - start));
		Put32 (out, (uint32_t)crc);
	}

} // namespace

bool image::WritePNG (const std::string &path, uint32_t w, uint32_t h, const uint8_t *rgb, bool fast)
{
	// raw scanlines, each prefixed with its filter type: 0 (none), or for
	// fast writing 2 (up: difference to the line above, which compresses
	// well at a low compression level)
	std::vector<uint8_t> raw;
	size_t stride = (size_t)w * 3;
	raw.reserve ((size_t)h * (stride + 1));
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *line = rgb + y * stride;
		if (fast && y > 0) {
			raw.push_back (2);
			const uint8_t *prev = line - stride;
			for (size_t i = 0; i < stride; i++) raw.push_back ((uint8_t)(line[i] - prev[i]));
		} else {
			raw.push_back (0);
			raw.insert (raw.end(), line, line + stride);
		}
	}
	uLongf zlen = compressBound ((uLong)raw.size());
	std::vector<uint8_t> z (zlen);
	if (compress2 (z.data(), &zlen, raw.data(), (uLong)raw.size(), fast ? 1 : 6) != Z_OK) return false;
	z.resize (zlen);

	std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector<uint8_t> ihdr;
	Put32 (ihdr, w);
	Put32 (ihdr, h);
	ihdr.insert (ihdr.end(), { 8, 2, 0, 0, 0 }); // 8 bit, RGB, deflate, adaptive filtering (per line), no interlace
	Chunk (png, "IHDR", ihdr);
	Chunk (png, "IDAT", z);
	Chunk (png, "IEND", {});

	// (written to a temporary file and renamed, so that the file appears complete)
	std::string tmp = path + ".part";
	FILE *f = fopen (tmp.c_str(), "wb");
	if (!f) return false;
	bool ok = fwrite (png.data(), 1, png.size(), f) == png.size();
	fclose (f);
	if (ok) ok = rename (tmp.c_str(), path.c_str()) == 0;
	else remove (tmp.c_str());
	return ok;
}

// ======================================================================
// PNG reading (see the PNG specification, ISO/IEC 15948)

namespace {

	uint32_t Get32 (const uint8_t *p)
	{
		return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
	}

	uint8_t Paeth (int a, int b, int c)
	{
		int p = a + b - c;
		int pa = abs (p - a), pb = abs (p - b), pc = abs (p - c);
		if (pa <= pb && pa <= pc) return (uint8_t)a;
		return (uint8_t)(pb <= pc ? b : c);
	}

} // namespace

bool image::ReadPNG (const std::string &path, uint32_t &w, uint32_t &h, std::vector<uint8_t> &rgba)
{
	FILE *f = fopen (path.c_str(), "rb");
	if (!f) return false;
	std::vector<uint8_t> file;
	fseek (f, 0, SEEK_END);
	long size = ftell (f);
	fseek (f, 0, SEEK_SET);
	if (size > 0) {
		file.resize (size);
		if (fread (file.data(), 1, size, f) != (size_t)size) file.clear();
	}
	fclose (f);

	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	if (file.size() < 8 || memcmp (file.data(), sig, 8)) return false;

	uint8_t bitDepth = 0, colourType = 0, interlace = 0;
	std::vector<uint8_t> idat, palette, trns;
	size_t pos = 8;
	while (pos + 12 <= file.size()) {
		uint32_t len = Get32 (&file[pos]);
		const char *type = (const char*)&file[pos + 4];
		const uint8_t *data = &file[pos + 8];
		if (pos + 12 + (size_t)len > file.size()) return false;
		if (!memcmp (type, "IHDR", 4) && len >= 13) {
			w = Get32 (data);
			h = Get32 (data + 4);
			bitDepth = data[8];
			colourType = data[9];
			interlace = data[12];
		} else if (!memcmp (type, "PLTE", 4)) {
			palette.assign (data, data + len);
		} else if (!memcmp (type, "tRNS", 4)) {
			trns.assign (data, data + len);
		} else if (!memcmp (type, "IDAT", 4)) {
			idat.insert (idat.end(), data, data + len);
		} else if (!memcmp (type, "IEND", 4)) {
			break;
		}
		pos += 12 + len;
	}
	if (!w || !h || bitDepth != 8 || interlace) return false;

	int channels;
	switch (colourType) {
	case 0: channels = 1; break; // grey
	case 2: channels = 3; break; // RGB
	case 3: channels = 1; break; // palette
	case 4: channels = 2; break; // grey + alpha
	case 6: channels = 4; break; // RGBA
	default: return false;
	}
	if (colourType == 3 && palette.size() < 3) return false;

	size_t stride = (size_t)w * channels;
	std::vector<uint8_t> raw ((stride + 1) * h);
	uLongf rawLen = (uLongf)raw.size();
	if (uncompress (raw.data(), &rawLen, idat.data(), (uLong)idat.size()) != Z_OK || rawLen != raw.size())
		return false;

	// undo the scanline filters
	std::vector<uint8_t> px (stride * h);
	for (uint32_t y = 0; y < h; y++) {
		uint8_t filter = raw[y * (stride + 1)];
		const uint8_t *src = &raw[y * (stride + 1) + 1];
		uint8_t *dst = &px[y * stride];
		const uint8_t *prev = y ? &px[(y - 1) * stride] : nullptr;
		for (size_t x = 0; x < stride; x++) {
			int a = x >= (size_t)channels ? dst[x - channels] : 0;
			int b = prev ? prev[x] : 0;
			int c = (prev && x >= (size_t)channels) ? prev[x - channels] : 0;
			switch (filter) {
			case 0: dst[x] = src[x]; break;
			case 1: dst[x] = (uint8_t)(src[x] + a); break;
			case 2: dst[x] = (uint8_t)(src[x] + b); break;
			case 3: dst[x] = (uint8_t)(src[x] + ((a + b) >> 1)); break;
			case 4: dst[x] = (uint8_t)(src[x] + Paeth (a, b, c)); break;
			default: return false;
			}
		}
	}

	rgba.resize ((size_t)w * h * 4);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		const uint8_t *s = &px[i * channels];
		uint8_t *d = &rgba[i * 4];
		switch (colourType) {
		case 0: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
		case 2: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
		case 3: {
			size_t idx = s[0];
			if (idx * 3 + 2 < palette.size()) { d[0] = palette[idx*3]; d[1] = palette[idx*3+1]; d[2] = palette[idx*3+2]; }
			else d[0] = d[1] = d[2] = 0;
			d[3] = idx < trns.size() ? trns[idx] : 255;
			} break;
		case 4: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
		case 6: memcpy (d, s, 4); break;
		}
	}
	return true;
}

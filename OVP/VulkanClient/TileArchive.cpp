// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "TileArchive.h"
#include "zlib.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace {
	const char *layerName[] = { "Surf", "Mask", "Elev", "Elev_mod", "Cloud" };
	const char *layerExt[]  = { ".dds", ".dds", ".elv", ".elv", ".dds" };

	constexpr size_t HEADER_SIZE = 48;   // archive file header
	constexpr size_t NODE_SIZE = 32;     // TOC entry (int64 pos, uint32 size, uint32 child[4], padding)

	uint32_t U32 (const uint8_t *p) { uint32_t v; memcpy (&v, p, 4); return v; }
	uint64_t U64 (const uint8_t *p) { uint64_t v; memcpy (&v, p, 8); return v; }

	int Seek64 (FILE *f, uint64_t pos)
	{
#ifdef _WIN32
		return _fseeki64 (f, (__int64)pos, SEEK_SET);
#else
		return fseeko (f, (off_t)pos, SEEK_SET);
#endif
	}
}

TileLayer::TileLayer (const std::string &planetDir, Layer layer)
	: dir(planetDir), name(layerName[layer]), ext(layerExt[layer])
{
	std::error_code ec;
	cacheExists = std::filesystem::is_directory (dir + "/" + name + "/04", ec);

	std::string path = dir + "/Archive/" + name + ".tree";
	FILE *f = fopen (path.c_str(), "rb");
	if (!f) {
		// (no archive and no cache: the legacy patch textures, if any)
		if (!cacheExists && (layer == SURF || layer == CLOUD))
			legacy = LegacyMap::Load (dir + (layer == CLOUD ? "_cloud.tex" : ".tex"));
		return;
	}
	uint8_t hdr[HEADER_SIZE];
	if (fread (hdr, 1, HEADER_SIZE, f) != HEADER_SIZE || memcmp (hdr, "TX\x01\x00", 4) || U32 (hdr + 4) != HEADER_SIZE) {
		fclose (f);
		return;
	}
	dataOfs = U32 (hdr + 12);
	dataLength = U64 (hdr + 16);
	uint32_t nNode = U32 (hdr + 24);
	for (int i = 0; i < 3; i++) root[i] = U32 (hdr + 28 + 4*i);
	root4[0] = U32 (hdr + 40);
	root4[1] = U32 (hdr + 44);

	std::vector<uint8_t> raw ((size_t)nNode * NODE_SIZE);
	if (fread (raw.data(), 1, raw.size(), f) != raw.size()) {
		fclose (f);
		return;
	}
	toc.resize (nNode);
	for (uint32_t i = 0; i < nNode; i++) {
		const uint8_t *p = raw.data() + (size_t)i * NODE_SIZE;
		toc[i].pos = (int64_t)U64 (p);
		toc[i].size = U32 (p + 8);
		for (int c = 0; c < 4; c++) toc[i].child[c] = U32 (p + 12 + 4*c);
	}
	archive = f;
}

TileLayer::~TileLayer ()
{
	if (archive) fclose (archive);
}

uint32_t TileLayer::Index (int lvl, int ilat, int ilng) const
{
	if (!archive || lvl < 1) return NONE;
	if (lvl <= 3) return root[lvl - 1];
	if (lvl == 4) return (ilng >= 0 && ilng < 2 && ilat == 0) ? root4[ilng] : NONE;
	uint32_t p = Index (lvl - 1, ilat / 2, ilng / 2);
	if (p == NONE || p >= toc.size()) return NONE;
	return toc[p].child[((ilat & 1) << 1) | (ilng & 1)];
}

bool TileLayer::HasTile (int lvl, int ilat, int ilng) const
{
	if (legacy) return lvl >= 4 && lvl <= legacy->MaxLevel();
	uint32_t idx = Index (lvl, ilat, ilng);
	if (idx != NONE && idx < toc.size() && toc[idx].size) return true;
	return cacheExists;
}

bool TileLayer::ReadCache (int lvl, int ilat, int ilng, std::vector<uint8_t> &data) const
{
	if (!cacheExists) return false;
	char rel[64];
	snprintf (rel, sizeof(rel), "/%02d/%06d/%06d", lvl, ilat, ilng);
	std::string path = dir + "/" + name + rel + ext;
	FILE *f = fopen (path.c_str(), "rb");
	if (!f) return false;
	fseek (f, 0, SEEK_END);
	long n = ftell (f);
	fseek (f, 0, SEEK_SET);
	bool ok = n > 0;
	if (ok) {
		data.resize (n);
		ok = fread (data.data(), 1, n, f) == (size_t)n;
	}
	fclose (f);
	return ok;
}

bool TileLayer::Read (int lvl, int ilat, int ilng, std::vector<uint8_t> &data)
{
	if (legacy) {
		if (lvl < 4 || lvl > legacy->MaxLevel()) return false;
		legacy->Tile (lvl, ilat, ilng, data);
		return true;
	}
	// individual tile files take precedence over the archive
	if (ReadCache (lvl, ilat, ilng, data)) return true;

	uint32_t idx = Index (lvl, ilat, ilng);
	if (idx == NONE || idx >= toc.size() || !toc[idx].size) return false;
	uint64_t end = (idx + 1 < toc.size()) ? (uint64_t)toc[idx + 1].pos : dataLength;
	uint64_t zsize = end - (uint64_t)toc[idx].pos;
	std::vector<uint8_t> zbuf (zsize);
	{
		std::lock_guard<std::mutex> lock (mtx);
		if (Seek64 (archive, dataOfs + toc[idx].pos) || fread (zbuf.data(), 1, zsize, archive) != zsize)
			return false;
	}
	data.resize (toc[idx].size);
	uLongf n = (uLongf)data.size();
	if (uncompress (data.data(), &n, zbuf.data(), (uLong)zsize) != Z_OK) return false;
	data.resize (n);
	return true;
}

// ======================================================================
// legacy patch textures

namespace {
	struct Rgba { float r, g, b, a; };

	// one block-compressed texture (DXT1, DXT3, DXT5) -> RGBA8
	bool DecodeDXT (const uint8_t *d, size_t n, uint32_t fourcc, int w, int h, std::vector<uint32_t> &out)
	{
		int bs = fourcc == 0x31545844 ? 8 : 16; // ('DXT1')
		if (n < (size_t)(w / 4) * (h / 4) * bs) return false;
		out.assign ((size_t)w * h, 0);
		auto col = [](uint16_t c) {
			int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
			return Rgba{ (float)(r << 3 | r >> 2), (float)(g << 2 | g >> 4), (float)(b << 3 | b >> 2), 255.0f };
		};
		for (int by = 0; by < h / 4; by++)
			for (int bx = 0; bx < w / 4; bx++, d += bs) {
				const uint8_t *cb = bs == 8 ? d : d + 8; // colour block
				uint16_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
				Rgba e0 = col (c0), e1 = col (c1), pal[4];
				pal[0] = e0, pal[1] = e1;
				bool four = c0 > c1 || bs == 16;
				for (int k = 0; k < 2; k++) {
					float t = four ? (k ? 2.0f : 1.0f) / 3.0f : 0.5f;
					Rgba &p = pal[2 + k];
					p = { e0.r + (e1.r - e0.r) * t, e0.g + (e1.g - e0.g) * t, e0.b + (e1.b - e0.b) * t, 255.0f };
				}
				if (!four) pal[3] = { 0, 0, 0, 0 };
				uint32_t bits = cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
				// alpha: explicit (DXT3) or interpolated (DXT5)
				float al[16];
				for (int k = 0; k < 16; k++) al[k] = -1.0f;
				if (fourcc == 0x33545844) // DXT3
					for (int k = 0; k < 16; k++) al[k] = (float)(((d[k / 2] >> (4 * (k & 1))) & 15) * 17);
				else if (fourcc == 0x35545844) { // DXT5
					float a0 = d[0], a1 = d[1], ap[8] = { a0, a1 };
					for (int k = 2; k < 8; k++)
						ap[k] = a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7.0f : (k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5.0f : (k == 6 ? 0.0f : 255.0f));
					uint64_t ab = 0;
					for (int k = 0; k < 6; k++) ab |= (uint64_t)d[2 + k] << (8 * k);
					for (int k = 0; k < 16; k++) al[k] = ap[(ab >> (3 * k)) & 7];
				}
				for (int k = 0; k < 16; k++) {
					Rgba p = pal[(bits >> (2 * k)) & 3];
					if (al[k] >= 0.0f) p.a = al[k];
					uint32_t v = (uint32_t)p.r | (uint32_t)p.g << 8 | (uint32_t)p.b << 16 | (uint32_t)p.a << 24;
					out[(size_t)(by * 4 + k / 4) * w + bx * 4 + k % 4] = v;
				}
			}
		return true;
	}

	// sphere patches of the levels: bands per hemisphere from the equator
	// to the pole (level 4: the two halves in longitude)
	const std::vector<int> BANDS[9] = { {}, {}, {}, {}, {}, { 4 }, { 8, 4 }, { 16, 16, 12, 6 }, { 32, 32, 30, 28, 24, 18, 12, 6 } };
	const int NPATCH[9] = { 0, 1, 1, 1, 2, 8, 24, 100, 364 };
}

std::unique_ptr<LegacyMap> LegacyMap::Load (const std::string &texPath)
{
	FILE *f = fopen (texPath.c_str(), "rb");
	if (!f) return nullptr;
	std::vector<uint8_t> file;
	fseek (f, 0, SEEK_END);
	long n = ftell (f);
	fseek (f, 0, SEEK_SET);
	if (n > 0) {
		file.resize (n);
		if (fread (file.data(), 1, n, f) != (size_t)n) file.clear();
	}
	fclose (f);
	// the textures, in order
	struct Tex { size_t ofs, size; int w, h; uint32_t fourcc; };
	std::vector<Tex> tex;
	size_t o = 0;
	while (o + 128 <= file.size() && !memcmp (&file[o], "DDS ", 4)) {
		Tex t;
		t.h = (int)U32 (&file[o + 12]), t.w = (int)U32 (&file[o + 16]);
		t.fourcc = U32 (&file[o + 84]);
		if (t.fourcc != 0x31545844 && t.fourcc != 0x33545844 && t.fourcc != 0x35545844) break;
		int mips = std::max (1, (int)U32 (&file[o + 28]));
		size_t sz = 0;
		for (int m = 0, ww = t.w, hh = t.h; m < mips; m++, ww = std::max (1, ww / 2), hh = std::max (1, hh / 2))
			sz += (size_t)std::max (1, ww / 4) * std::max (1, hh / 4) * (t.fourcc == 0x31545844 ? 8 : 16);
		t.ofs = o + 128, t.size = sz;
		if (t.ofs + sz > file.size()) break;
		tex.push_back (t);
		o = t.ofs + sz;
	}
	// the finest complete level
	int level = 0;
	size_t first = 0, cum = 0;
	for (int l = 1; l <= 8; l++) {
		if (cum + NPATCH[l] > tex.size()) break;
		level = l, first = cum;
		cum += NPATCH[l];
	}
	if (level < 3) return nullptr;
	auto map = std::unique_ptr<LegacyMap> (new LegacyMap);
	map->alpha = tex[0].fourcc != 0x31545844;
	std::vector<uint32_t> p;
	if (level == 3 || level == 4) {
		// a whole-sphere texture (level 3), or the two halves (level 4)
		int np = level == 3 ? 1 : 2;
		int pw = tex[first].w, ph = tex[first].h;
		map->w = pw * np * (level == 3 ? 1 : 1), map->h = ph;
		if (level == 3) map->w = pw * 2, map->h = ph; // (256 x 256 over 360 x 180 degrees: stretched to 2:1)
		map->px.assign ((size_t)map->w * map->h, 0);
		for (int k = 0; k < np; k++) {
			if (!DecodeDXT (&file[tex[first + k].ofs], tex[first + k].size, tex[first + k].fourcc, pw, ph, p)) return nullptr;
			int cw = map->w / np;
			for (int y = 0; y < map->h; y++)
				for (int x = 0; x < cw; x++)
					map->px[(size_t)y * map->w + k * cw + x] = p[(size_t)y * pw + (int)((x + 0.5) * pw / cw)];
		}
	} else {
		const std::vector<int> &bands = BANDS[level];
		int nb = (int)bands.size(), P = tex[first].w;
		map->w = bands[0] * P, map->h = 2 * nb * P;
		map->px.assign ((size_t)map->w * map->h, 0);
		size_t k = first;
		for (int hemi = 0; hemi < 2; hemi++)
			for (int b = nb - 1; b >= 0; b--) { // (stored from the pole to the equator)
				int nlng = bands[b], cw = map->w / nlng;
				int row = hemi == 0 ? nb - 1 - b : nb + b;
				for (int i = 0; i < nlng; i++, k++) {
					if (!DecodeDXT (&file[tex[k].ofs], tex[k].size, tex[k].fourcc, tex[k].w, tex[k].h, p)) return nullptr;
					// (the southern patches are turned by 180 degrees, in reverse order)
					int col = hemi ? nlng - 1 - i : i;
					for (int y = 0; y < P; y++)
						for (int x = 0; x < cw; x++) {
							double sx = (x + 0.5) * tex[k].w / cw - 0.5, sy = y; // (horizontal resampling: bands with fewer patches)
							int x0 = std::clamp ((int)std::floor (sx), 0, tex[k].w - 1), x1 = std::min (x0 + 1, tex[k].w - 1);
							double t = std::clamp (sx - x0, 0.0, 1.0);
							int yy = (int)sy;
							if (hemi) yy = P - 1 - yy, x0 = tex[k].w - 1 - x0, x1 = tex[k].w - 1 - x1;
							uint32_t a = p[(size_t)yy * tex[k].w + x0], c = p[(size_t)yy * tex[k].w + x1], v = 0;
							for (int ch = 0; ch < 4; ch++) {
								double va = (a >> (8 * ch)) & 255, vc = (c >> (8 * ch)) & 255;
								v |= (uint32_t)std::lround (va + (vc - va) * t) << (8 * ch);
							}
							map->px[(size_t)(row * P + y) * map->w + col * cw + x] = v;
						}
				}
			}
	}
	// tiles of their own while they have at least 128 of the map's texels across
	map->maxLevel = std::max (4, (int)std::floor (3.0 + std::log2 (map->w / 128.0)));
	return map;
}

void LegacyMap::Tile (int lvl, int ilat, int ilng, std::vector<uint8_t> &dds) const
{
	const int N = 256;
	int nlat = 1 << (lvl - 4), nlng = 2 << (lvl - 4);
	double latmax = 0.5 * M_PI - ilat * M_PI / nlat, dlat = M_PI / nlat;
	double lngmin = -M_PI + ilng * 2.0 * M_PI / nlng, dlng = 2.0 * M_PI / nlng;
	dds.assign (128 + (size_t)N * N * 4, 0);
	uint8_t *hd = dds.data();
	auto put = [&](size_t at, uint32_t v) { memcpy (hd + at, &v, 4); };
	memcpy (hd, "DDS ", 4);
	put (4, 124);
	put (8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x8);   // caps, height, width, pixel format, pitch
	put (12, N), put (16, N), put (20, N * 4);
	put (76, 32);                               // pixel format
	put (80, 0x40 | (alpha ? 0x1 : 0));         // RGB (alpha)
	put (88, 32);
	put (92, 0x000000FF), put (96, 0x0000FF00), put (100, 0x00FF0000), put (104, alpha ? 0xFF000000 : 0);
	put (108, 0x1000);
	uint8_t *o = hd + 128;
	for (int y = 0; y < N; y++) {
		double lat = latmax - (y + 0.5) / N * dlat;
		double gy = (0.5 * M_PI - lat) / M_PI * h - 0.5;
		int y0 = std::clamp ((int)std::floor (gy), 0, h - 1), y1 = std::min (y0 + 1, h - 1);
		double ty = std::clamp (gy - std::floor (gy), 0.0, 1.0);
		for (int x = 0; x < N; x++, o += 4) {
			double lng = lngmin + (x + 0.5) / N * dlng;
			double gx = (lng + M_PI) / (2.0 * M_PI) * w - 0.5;
			int x0 = (int)std::floor (gx);
			double tx = gx - x0;
			x0 = ((x0 % w) + w) % w;
			int x1 = (x0 + 1) % w;
			uint32_t a = px[(size_t)y0 * w + x0], b = px[(size_t)y0 * w + x1], c = px[(size_t)y1 * w + x0], d = px[(size_t)y1 * w + x1];
			for (int ch = 0; ch < 4; ch++) {
				double va = (a >> (8 * ch)) & 255, vb = (b >> (8 * ch)) & 255, vc = (c >> (8 * ch)) & 255, vd = (d >> (8 * ch)) & 255;
				double v = (va + (vb - va) * tx) * (1.0 - ty) + (vc + (vd - vc) * tx) * ty;
				o[ch] = (uint8_t)std::lround (v);
			}
		}
	}
}

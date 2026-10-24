// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "TileArchive.h"
#include "zlib.h"
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
	if (!f) return;
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

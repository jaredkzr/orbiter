// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Access to planetary tile layers (Orbiter 2016 format): compressed
// quadtree archives (Textures/<planet>/Archive/<layer>.tree) and the
// individual tile files of the cache (Textures/<planet>/<layer>/...).
//
// Planets with only the legacy format (Textures/<planet>.tex, and
// <planet>_cloud.tex: DDS textures of the sphere patches of levels 1 to
// 8 back to back) get their surface and cloud tiles made from it: the
// patches of the finest level are assembled into a global map (longitude
// and latitude), from which tiles are sampled on demand.
// =======================================================================

#ifndef __TILEARCHIVE_H
#define __TILEARCHIVE_H

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/// A global map from the legacy patch textures
class LegacyMap {
public:
	/// texPath: the .tex file; null if it cannot be read
	static std::unique_ptr<LegacyMap> Load (const std::string &texPath);
	/// finest tile level with data of its own (coarser than the map's texels)
	int MaxLevel () const { return maxLevel; }
	/// a tile (level >= 4) as an uncompressed DDS file (RGBA, 256 x 256)
	void Tile (int lvl, int ilat, int ilng, std::vector<uint8_t> &dds) const;
	int Width () const { return w; }
	int Height () const { return h; }

private:
	int w = 0, h = 0, maxLevel = 4;
	bool alpha = false;
	std::vector<uint32_t> px; // RGBA8 (r in the low byte), rows from north to south, longitude -180 at the left
};

class TileLayer {
public:
	enum Layer { SURF, MASK, ELEV, ELEVMOD, CLOUD };

	/// planetDir: Textures/<planet> directory
	TileLayer (const std::string &planetDir, Layer layer);
	~TileLayer ();

	/// true if the layer has any data (archive, cache or legacy map)
	bool Valid () const { return archive != nullptr || cacheExists || legacy; }
	/// the layer comes from the legacy patch textures
	bool Legacy () const { return legacy != nullptr; }

	/// Quick check whether a tile has data. Tiles present only in the cache
	/// are reported as available (checked on read).
	bool HasTile (int lvl, int ilat, int ilng) const;

	/// Read the uncompressed tile data (thread safe). Returns false if not present.
	bool Read (int lvl, int ilat, int ilng, std::vector<uint8_t> &data);

private:
	struct Node {
		int64_t pos;
		uint32_t size;
		uint32_t child[4];
	};
	static constexpr uint32_t NONE = 0xFFFFFFFF;
	uint32_t Index (int lvl, int ilat, int ilng) const;
	bool ReadCache (int lvl, int ilat, int ilng, std::vector<uint8_t> &data) const;

	std::string dir;       // planet texture directory
	const char *name;      // layer name
	const char *ext;       // tile file extension
	FILE *archive = nullptr;
	std::vector<Node> toc;
	uint32_t root[3] = { NONE, NONE, NONE }; // levels 1-3
	uint32_t root4[2] = { NONE, NONE };      // level 4 quadtree roots
	uint64_t dataOfs = 0, dataLength = 0;
	bool cacheExists = false;
	std::unique_ptr<LegacyMap> legacy;
	std::mutex mtx;
};

#endif // !__TILEARCHIVE_H

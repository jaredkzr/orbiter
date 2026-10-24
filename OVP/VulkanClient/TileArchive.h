// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Access to planetary tile layers (Orbiter 2016 format): compressed
// quadtree archives (Textures/<planet>/Archive/<layer>.tree) and the
// individual tile files of the cache (Textures/<planet>/<layer>/...)
// =======================================================================

#ifndef __TILEARCHIVE_H
#define __TILEARCHIVE_H

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

class TileLayer {
public:
	enum Layer { SURF, MASK, ELEV, ELEVMOD, CLOUD };

	/// planetDir: Textures/<planet> directory
	TileLayer (const std::string &planetDir, Layer layer);
	~TileLayer ();

	/// true if the layer has any data (archive or cache)
	bool Valid () const { return archive != nullptr || cacheExists; }

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
	std::mutex mtx;
};

#endif // !__TILEARCHIVE_H

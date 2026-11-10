// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Playa map: the data behind the playa rule (dry lakes, salt flats) of the
// terrain detail (TerrainDetail::Playa, Height). Two parts:
//
// Colour: a planet's surface texture is playa-like where it is bright and
// unsaturated, neither blue (ice) nor green (vegetation), and brighter than
// its region (a dry lake in its basin: CONTRAST_0-1 times the mean of the
// land around it over about 200 km; uniformly bright sand seas are not
// playas). It is judged on
// the texels of the finest Surf tile level up to PLAYA_LEVEL (about 600 m
// texels on Earth) whose tile exists, and interpolated bilinearly over them
// (continuous across tile seams). The level is capped so that the mask is the
// same whatever finer tiles exist or happen to be cached, and the physics
// (which reads this through the terrain detail) does not depend on what is
// drawn.
//
// Ground: a smoothed reference ground, the elevation data of level
// PLAYA_REF_LEVEL (153 m nodes on Earth) filtered by a Gaussian (sigma 3
// nodes) and interpolated bicubically. The terrain detail levels the data on
// playas to it (the data's hummocks, e.g. the SRTM swells on bright lakes,
// are not ground), so the heights, the physics and the client's meshes get a
// flat lakebed. Tiles that do not exist at that level are made from the finest
// existing ancestor, as the meshes' are; the filter's margins come from the
// neighbouring tiles. A pure function of position.
//
// The playa rule needs the planet's surface textures (PlanetTexDir and the
// TileLoadFlags): without them there are no playas, so the ground there (and
// the physics) differs from an install with them (a warning is logged).
//
// The client does not evaluate either: the terrain detail gives it the
// resulting playa weight (oapiSurfaceDetail's playa).
//
// Thread safe: the physics thread and the graphics client's mesh workers call
// Colour and Ground. Tiles are cached (a few, shared), with a lock-free
// per-thread cache of the last tiles used.
// =======================================================================

#ifndef __PLAYAMAP_H
#define __PLAYAMAP_H

#include <atomic>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class Planet;
class ZTreeMgr;

class PlayaMap {
public:
	explicit PlayaMap (const Planet *planet);
	~PlayaMap ();

	/// colour part of the playa rule (0-1) at a point [rad] (lng within +-pi): bilinear over the
	/// texels of the finest Surf level <= PLAYA_LEVEL there whose texels are at least spacing/2 [m]
	/// (spacing 0: level PLAYA_LEVEL; coarse data read few coarse tiles); 0 without textures. Thread safe.
	double Colour (double lat, double lng, double spacing = 0.0);

	/// smoothed reference ground [m above the mean radius] at a point [rad]
	/// (any longitude): NaN without elevation data. Thread safe.
	double Ground (double lat, double lng);

	/// the rule on a linear RGB colour
	static double ColourRule (double r, double g, double b);

	/// the regional part: smoothstep (CONTRAST_0, CONTRAST_1) of a texel's luminance over its region's
	static double ContrastRule (double lum, double regional);

	static const int PLAYA_LEVEL = 10;
	static const int REGION_LEVEL = 6;       ///< Surf level of the regional means (about 10 km texels on Earth)
	static const int REGION_DOWN = 4;        ///< its texels averaged to one cell (about 40 km)
	static const int REGION_BOX = 2;         ///< box filter radius [cells]: a 5-cell (about 200 km) region
	static constexpr double CONTRAST_0 = 1.08, CONTRAST_1 = 1.18;
	static constexpr double LAND_LUM = 0.08; ///< texels darker than this (water, forest) are not in the regional mean
	static const int PLAYA_REF_LEVEL = 13;   ///< file level of the elevation nodes Ground filters

private:
	struct Tile {
		int lvl, ilat, ilng, w, h;
		std::vector<uint8_t> c;  ///< the rule per texel (x 255)
	};
	/// the tile (null if there is none)
	std::shared_ptr<const Tile> Get (int lvl, int ilat, int ilng);   // (locks, loads outside the lock)
	/// the tile, through the thread's cache (a hit: raw pointer, no reference count; a miss: also in hold)
	const Tile *Fetch (int lvl, int ilat, int ilng, std::shared_ptr<const Tile> &hold);
	/// a counted reference to a tile of the thread's cache
	std::shared_ptr<const Tile> Pin (const Tile *t);
	/// whether a tile exists (remembered); lock held
	bool Exists (int lvl, int ilat, int ilng);   // lock (mtx) held
	std::shared_ptr<const Tile> Load (int lvl, int ilat, int ilng);
	/// decode a Surf tile (from the tree or a loose file): false if it is missing or not DXT1
	bool Decode (int lvl, int ilat, int ilng, std::vector<uint32_t> &px, int &w, int &h);
	/// the regional mean land luminance at a point [rad] (NaN: unknown)
	double Region (double lat, double lng);
	void BuildRegion ();
	std::once_flag regionOnce;
	std::vector<float> region;           ///< mean land luminance per cell, rows from the north (NaN: no land)
	int regW = 0, regH = 0;

	/// a tile of filtered reference ground: nodes -1 to 257 of the level's tile
	struct GTile {
		int ilat, ilng;
		std::vector<float> g;   ///< RefStride()^2, rows from the north
	};
	std::shared_ptr<const GTile> GetG (int ilat, int ilng);   // (locks, builds outside the lock)
	const GTile *FetchG (int ilat, int ilng, std::shared_ptr<const GTile> &hold);
	std::shared_ptr<const GTile> BuildG (int ilat, int ilng);

	const Planet *planet;
	ZTreeMgr *tree = nullptr;
	std::string dir;                     ///< loose tiles
	bool hasSource = false;
	std::mutex mtx;                      ///< colour tiles, their LRU and exists
	std::mutex gmtx;                     ///< ground tiles and their LRU
	std::mutex ioMtx;                    ///< reads from the tile tree (one file handle)
	double radius;
	double texel (int lvl) const;        ///< texel size of a level [m]
	std::list<std::shared_ptr<const Tile>> lru;  ///< most recent first
	std::set<uint64_t> gnone;            ///< ground tiles without data
	std::list<std::shared_ptr<const GTile>> glru;  ///< most recent first
	std::map<uint64_t, bool> exists;     ///< loose files: hits and misses
	const uint64_t serial;               ///< (this one, for the threads' caches: not its address, reused)
};

#endif // !__PLAYAMAP_H

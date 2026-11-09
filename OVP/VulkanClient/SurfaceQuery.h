// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// What the ground is at a point of a planet (ground, water, ice), decided
// by the water mask the tile shader draws, so that effects agree with the
// picture; the body's sea flag and the elevation where there is no mask.
// =======================================================================

#ifndef __SURFACEQUERY_H
#define __SURFACEQUERY_H

#include "OrbiterAPI.h"
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

struct PlanetLayers;
class Scene;

namespace vksurf {

enum class Kind : uint8_t { Ground = 0, Water, Ice };
enum class Source : uint8_t { Fallback = 0, Mask };   // how the kind was decided

struct Point {
	Kind kind = Kind::Ground;
	float water = 0.0f;   // share of water 0..1 (mask: 1 - bilinear alpha; fallback: 0 or 1)
	double elev = 0.0;    // [m] the core's surface elevation there (oapiSurfaceElevation)
	bool sea = false;     // HasSea (hp)
	Source source = Source::Fallback;
	int maskLevel = 0;    // level of the mask tile sampled (0: none / fallback)
	int procClass = -1;   // PROCSURFACEPARAM::surfClass (0 rock 1 ocean 2 ice 3 desert 4 lava), -1: not procedural
	bool IsWater () const { return kind == Kind::Water; }
};

/// the body has seas: SURFDETAILPARAM::sea when terrain detail is on (TerrainDetailSea; Earth's
/// default), else a procedural Ocean world (oapiProcSurfaceParams surfClass 1)
bool HasSea (OBJHANDLE hp);

/// what the ground is at planet-frame longitude/latitude [rad] (main thread)
Point Query (Scene *scene, OBJHANDLE hp, double lng, double lat);
/// ... at the surface below a global position
Point QueryGlobal (Scene *scene, OBJHANDLE hp, const VECTOR3 &gpos);

/// Texels of one tile layer of a planet about given points: the finest tile with data up to
/// maxLevel holding the point, read and decoded to RGBA8 on a worker thread (one read in flight),
/// a few tiles kept (LRU). Main thread only (Sample); the worker touches only the layers.
class TileProbe {
public:
	enum class Layer { Surf, Mask };
	TileProbe (std::shared_ptr<PlanetLayers> layers, Layer layer, int maxLevel, const char *bodyName);
	~TileProbe ();   // waits for a read in flight
	/// bilinear RGBA [0,1] at lat/lng [rad]. false: its tile is being read (try again next frame)
	/// or the layer has no tile there. lvl: the level of the tile sampled
	/// mean: the tile's mean RGBA [0,1] (may be null)
	bool Sample (double lat, double lng, float rgba[4], int *lvl = nullptr, float *mean = nullptr);
	/// Sample, or while the point's tile is being read (or has no data) the last value sampled
	/// within maxAng [rad] of it (no jump in colour for the frames a new tile takes to read)
	bool SampleOrLast (double lat, double lng, double maxAng, float rgba[4], float mean[4], int *lvl = nullptr);
private:
	struct Entry {
		int keyLvl = 0, keyLat = 0, keyLng = 0; // the maxLevel tile asked for (the cache key)
		int lvl = 0, ilat = 0, ilng = 0;         // the tile found (maxLevel or coarser)
		int w = 0, h = 0;                        // 0: no tile with data (remembered)
		std::vector<uint8_t> rgba;
		float mean[4] = { 0, 0, 0, 0 };          // mean of the texels [0,1] (computed by Load, on the worker)
		double ms = 0.0;                         // read + decode time (for the log)
		uint64_t used = 0;
	};
	static Entry Load (std::shared_ptr<PlanetLayers> layers, Layer layer, int maxLevel, double lat, double lng);
	std::shared_ptr<PlanetLayers> layers;
	Layer layer;
	int maxLevel;
	std::string name;
	std::vector<Entry> cache;            // up to 6
	struct Last {
		bool valid = false;
		double lat = 0, lng = 0;
		float rgba[4] = {}, mean[4] = {};
		int lvl = 0;
	} last;                              // the last good sample (SampleOrLast)
	std::future<Entry> pending;          // the read in flight (if valid())
	uint64_t clock = 0;
};

/// the colour of the dust a jet or a crash throws up at a point (linear albedo, for
/// ParticleSystem::Emission::albedo): the drawn ground (the surface tile, linearised, the body's
/// SurfaceSaturation applied, its luminance lifted for the finer grains that fly, hue kept), 70%
/// with 30% the body's dust default; plants (texels greener than red and blue) and water in the
/// texel give way to the default. The default alone where the body has no surface tiles or none read within 20 km yet.
/// pt: the point's Query if the caller has it (else made here). veg: the plant share 0..1 (may be null).
/// true: the ground was sampled. Main thread.
bool DustColour (Scene *scene, OBJHANDLE hp, double lng, double lat, float rgb[3], const Point *pt = nullptr, float *veg = nullptr);
/// ... at the surface below a global position
bool DustColourGlobal (Scene *scene, OBJHANDLE hp, const VECTOR3 &gpos, float rgb[3], const Point *pt = nullptr, float *veg = nullptr);

} // namespace vksurf

#endif // !__SURFACEQUERY_H

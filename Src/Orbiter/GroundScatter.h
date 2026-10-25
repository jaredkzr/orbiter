// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Ground scatter: rocks, boulders, trees, shrubs and grass on a planet's
// surface, placed deterministically, so that physics and graphics clients
// (oapiScatter*) see the same objects. Large boulders and tree trunks are
// obstacles vessels collide with; small rocks, shrubs and grass are only
// drawn.
//
// The assets and layers of a body are in Config/Scatter/<body>.cfg:
//   ASSETS <file>                         asset list made by the tools
//                                         (Utils/ScatterAssets: ROCK lines)
//   ROCK <mesh stem> <radius> <height> .. a rock (meshes <stem>_lod0..2;
//                                         its collision hull: the convex
//                                         hull of the coarsest level)
//   PLANT <TREE|SHRUB|GRASS> <name> <height> <trunk radius> [<lat0> <lat1>]
//                                         a plant made by the graphics
//                                         client (by name); trees and shrubs
//                                         grow between latitudes lat0 and
//                                         lat1 [deg, absolute], fading 5 deg
//                                         beyond
//   LAYER <name> <kind> <min size> <max size> <cell> <spacing> <density>
//         <range> <collide> <cover>
//                                         objects of a kind and size range
//                                         [m] (size: largest of width and
//                                         height); cells of <cell> m with a
//                                         candidate every <spacing> m (a
//                                         jittered grid), <density> per m²
//                                         on open ground; drawn within
//                                         <range> m; <collide> 1: obstacles;
//                                         <cover>: none, slope (more on
//                                         slopes), bare (rocky ground, from
//                                         the surface texture), forest,
//                                         bush, meadow (vegetation, from the
//                                         surface texture; no water)
//
// Cells: rows of latitude <cell> m high, each split in longitude into
// cells about <cell> m wide. Each cell's objects come from a hash of the
// body, the layer and the cell. No objects near surface bases or around
// vessels that start landed. Ground cover is read from the planet's
// surface texture (Surf) and water mask (Mask) tiles.
//
// On only with Orbiter.cfg GroundScatter (default on) and a graphics client
// that draws it (oapiScatterSupported), or without a graphics client;
// ORBITER_NO_SCATTER=1 turns it off (tests).
// =======================================================================

#ifndef __GROUNDSCATTER_H
#define __GROUNDSCATTER_H

#include "Vecmat.h"
#include "ConvexGeometry.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Planet;
class ZTreeMgr;
struct ElevationTile;

namespace scatter {

	enum Kind { ROCK = 0, GRASS = 1, SHRUB = 2, TREE = 3 };

	struct Asset {
		Kind kind;
		std::string name;      ///< mesh stem (rocks) or plant name
		double radius, height; ///< horizontal extent, height [m]
		double trunk = 0.0;    ///< trunk radius at the base [m] (trees)
		double lat0 = 0.0, lat1 = 90.0; ///< latitude range [deg] (plants)
		double Size () const { return std::max (2.0 * radius, height); }
		convex::Polytope hull; ///< collision shape (object frame, unit scale)
		bool hasHull = false;
	};

	struct Layer {
		std::string name;
		Kind kind;
		double minSize, maxSize, cell, spacing, density, range;
		bool collide;
		enum Cover { NONE, SLOPE, BARE, FOREST, BUSH, MEADOW } cover;
		std::vector<int> assets; ///< assets of the layer
		std::vector<double> weight; ///< their relative frequencies
	};

	/// An object: in the planet frame (rotating with it)
	struct Instance {
		int asset;
		Vector pos;  ///< position of the object's origin [m]
		Matrix rot;  ///< object frame (x east, y up, z north) -> planet frame
		double scale;
	};

	/// An obstacle: an instance's collision hull in the planet frame
	struct Obstacle {
		convex::Polytope hull;
		int asset;
	};

	/// Ground cover from the surface texture and water mask tiles
	class Cover {
	public:
		Cover (const Planet *planet);
		~Cover ();
		/// colour (linear 0-1) and water (0 or 1) at a point; false if there are no tiles
		bool Sample (double lat, double lng, Vector &rgb, double &water);
	private:
		struct Tile { int lvl, ilat, ilng; std::vector<uint32_t> px; int w = 0, h = 0; uint64_t used = 0; };
		const Tile *Get (int layer, double lat, double lng);
		bool Load (int layer, int lvl, int ilat, int ilng, Tile &t);
		const Planet *planet;
		::ZTreeMgr *tree[2] = { nullptr, nullptr }; // Surf, Mask
		std::string dir[2];  // loose tiles
		std::vector<Tile> cache[2];
		uint64_t clock = 0;
		int maxLevel;
	};

	class GroundScatter {
	public:
		GroundScatter (const Planet *planet);
		~GroundScatter ();

		/// On for this planet: configuration found, enabled, supported
		bool Active () const { return active; }
		/// the graphics client draws the scatter (oapiScatterSupported)
		static bool clientSupport;

		const std::vector<Asset> &Assets () const { return assets; }
		const std::vector<Layer> &Layers () const { return layers; }

		/// Cells of a layer within radius [m] of a point (planet frame)
		void CellsNear (int layer, const Vector &p, double radius, std::vector<std::pair<int,int>> &cells) const;
		/// The objects of a cell (cached)
		const std::vector<Instance> &Cell (int layer, int row, int col);
		/// The obstacles of colliding layers within radius [m] of a point
		/// (planet frame)
		void Obstacles (const Vector &p, double radius, std::vector<const Obstacle*> &out);

		/// Clear the ground of objects within radius [m] of a point (planet
		/// frame), e.g. under a vessel placed there; cells already made are
		/// made again
		void Clear (const Vector &p, double radius);
		/// incremented when cells change (Clear)
		unsigned Generation () const { return generation; }

	private:
		struct CellData {
			std::vector<Instance> inst;
			std::vector<Obstacle> obst; // (colliding layers, made on demand)
			bool obstDone = false;
			uint64_t used = 0;
		};
		void Load ();
		void LoadAssets (const std::string &file);
		void Make (int layer, int row, int col, CellData &cd);
		double CoverFactor (const Layer &L, const Vector &rgb, double water, bool haveCover) const;
		double Clearance (const Vector &dir) const; // 0 at bases and cleared ground, 1 away from them
		void Trim ();
		int Rows (const Layer &L) const;
		int Cols (const Layer &L, int row) const;

		const Planet *planet;
		double R;
		bool active = false;
		uint32_t seed;
		std::vector<Asset> assets;
		std::vector<Layer> layers;
		std::unique_ptr<Cover> cover;
		std::map<uint64_t, CellData> cells;
		uint64_t clock = 0;
		std::unique_ptr<std::vector<ElevationTile>> etile;
		struct Clearing { Vector dir; double r0, r1; }; // nothing within r0, fewer to r1 [m]
		std::vector<Clearing> clear;
		bool basesDone = false;
		unsigned generation = 0;
	};

} // namespace scatter

#endif // !__GROUNDSCATTER_H

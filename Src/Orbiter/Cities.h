// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Procedural cities: buildings on a planet's built-up ground, placed
// deterministically, so that physics and graphics clients (oapiCity*) see
// the same ones. Buildings are obstacles vessels collide with.
//
// Where: the planet's night lights (the RGB of its Mask tiles; alpha:
// water). They are a halftone: built-up ground is speckled with lit texels
// about as densely in a city's core as in its suburbs, so the local share
// of lit texels says whether the ground is built up, not how densely. How
// tall: by the city's size and the distance to its centre, from a
// catalogue of city centres made from the same lights (Utils/Cities/
// hotspots.py): each with the lit area about it ("metro") and whether it is
// its metro's main centre.
//
// Config/Cities/<planet>.cfg:
//   CENTRES <file>      the catalogue (Config/Cities/<file>): lines
//                       lat lng [deg] share [km2] peak metro [km2] main (0/1)
//   DOWNTOWNS <file>    known city centres (Config/Cities/<file>): lines
//                       lat lng [deg] height [m] radius [m] grid [name]
//                       (height: of its typical tall buildings, 60 m or
//                       less: a dense mid-rise centre; radius of its core;
//                       grid: bearing of its streets [deg east of north],
//                       * for its style's). They replace the catalogue's
//                       centres near them, and are built up whether the
//                       lights show it or not (e.g. poleward of 56 deg,
//                       where Earth's night lights end).
//   STYLE <name> <key>=<value> ...
//                       a building style (keys: Style below)
//   REGION <style> <lat0> <lat1> <lng0> <lng1>
//                       where a style is used [deg]; the first region that
//                       contains a point (none: the first style)
//
// Layout: around each centre a street grid (its own orientation; organic
// styles warped), streets about a block apart, every fifth one an avenue. A point belongs to the grid
// of the centre that is nearest relative to its reach. Blocks are zoned by
// the lit share and the nearness to centres (towers, mid-rise, low-rise,
// houses, industrial, parks) and cut into lots that get buildings.
//
// Cells: rows of latitude CELL m high, each split in longitude into cells
// about CELL m wide (as GroundScatter). A cell has the blocks whose centres
// lie in it. No buildings near surface bases, on water, on steep ground,
// or around vessels that start landed.
//
// On only with Orbiter.cfg Cities (default on) and a graphics client that
// draws them (oapiCitySupported), or without a graphics client;
// ORBITER_NO_CITIES=1 turns them off (tests).
// =======================================================================

#ifndef __CITIES_H
#define __CITIES_H

#include "Vecmat.h"
#include "ConvexGeometry.h"
#include <cstdint>
#include <functional>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

class Planet;
class ZTreeMgr;
class ElevationManager;
struct ElevationTile;
namespace scatter { class Cover; }

namespace cities {

	struct Grid; struct Zone; // (Cities.cpp)

	enum Kind { HOUSE = 0, LOWRISE = 1, MIDRISE = 2, TOWER = 3, INDUSTRIAL = 4 };

	/// A building: a box (later: the client's model of its kind and style)
	struct Building {
		Vector pos;     ///< centre of its footprint on the ground (planet frame) [m]
		Matrix rot;     ///< building frame (x along its front, y up, z back) -> planet frame
		float w, d, h;  ///< width (x), depth (z), height [m]
		float sunk;     ///< part of the height below the ground at pos [m]
		float storey;   ///< storey height [m]
		uint16_t storeys; ///< storeys above the ground (the rest of the height above them: roof, parapet)
		uint8_t kind, style;
		uint32_t seed;  ///< for its looks
	};

	/// A street: a straight piece of its centre line (planet frame, its ends
	/// on the ground), its width
	struct Street {
		Vector a, b;    ///< [m]
		float width;    ///< [m]
		uint8_t kind;   ///< 0 street, 1 avenue
	};

	struct Style {
		std::string name;
		double blockS = 160.0, blockT = 100.0; ///< street spacing along the grid's two axes [m]
		double jitter = 0.15;   ///< street positions moved by up to this share of a block
		double street = 14.0;   ///< street width [m]
		double warp = 0.0;      ///< warp of the street grid [m] (organic layouts) ...
		double warpScale = 600.0; ///< ... over this distance [m]
		double spread = -1.0;   ///< grid orientations within this of north [deg] (-1: any)
		double houseW = 12.0, houseD = 10.0, houseLot = 20.0; ///< detached houses: footprint, lot width [m]
		double houseStoreys = 1.5; ///< mean storeys of houses
		double storey = 3.2;    ///< storey height [m]
		double lowStoreys[2] = { 2, 4 }, midStoreys[2] = { 4, 8 }; ///< storeys of low- and mid-rise
		double perimeter = 0.0; ///< share of mid-rise blocks built round their edges (courtyards)
		double highrise = 0.0;  ///< share of dense non-core blocks with apartment towers
		double industrial = 0.08; ///< share of blocks on the urban fringe that are industrial
		double park = 0.06;     ///< share of blocks without buildings
		double towerScale = 1.0; ///< factor on tower heights
	};

	/// A city centre (catalogue)
	struct Centre {
		double lat, lng;     // [rad]
		Vector dir;          // unit vector (planet frame)
		double share, metro; // [km2]
		bool main;
		bool downtown = false; // from the downtown list
		double reach;        // [m] its street grid's reach (ownership: distance / reach)
		double rCore;        // [m] its high-rise core radius
		double hMax;         // [m] its tallest towers (typical)
		int style;
		Vector X, Y;         // its street grid's axes (planet frame)
		uint64_t seed;
	};

	/// The lit share of the ground about a point, from the night lights
	class Lights {
	public:
		Lights (const Planet *planet);
		~Lights ();
		/// lit share (0-1) about a point, smoothed over about a km; -1 if there
		/// are no tiles (any thread)
		double Sample (double lat, double lng);
	private:
		struct Tile { int lvl, ilat, ilng; std::vector<float> lit; int n = 0; std::atomic<uint64_t> used{0}; };
		/// the finest tile with data at a point (null: none); load: read
		/// tiles not cached yet (else: false if one would be needed)
		std::shared_ptr<const Tile> Get (double lat, double lng, bool load, bool &miss);
		const Planet *planet;
		::ZTreeMgr *tree = nullptr;
		std::string dir;
		std::unordered_map<uint64_t, std::shared_ptr<Tile>> cache; // (also tiles without data: n 0; shared: the threads' last ones kept)
		std::atomic<uint64_t> clock{0};
		std::shared_mutex mtx; // (reads shared, loads exclusive)
		const uint64_t serial; // (this one, for the threads' caches: not its address, reused)
	};

	class CityMap {
	public:
		CityMap (const Planet *planet);
		~CityMap ();

		/// On for this planet: configuration found, enabled, supported
		bool Active () const { return active; }
		/// the graphics client draws the cities (oapiCitySupported)
		static bool clientSupport;

		double CellSize () const;
		const std::vector<Style> &Styles () const { return styles; }
		/// Cells within radius [m] of a point (planet frame)
		void CellsNear (const Vector &p, double radius, std::vector<std::pair<int,int>> &cells) const;
		/// The buildings of a cell at least minHeight [m] tall (cached)
		const std::vector<Building> &Cell (int row, int col, double minHeight = 0.0);
		/// The streets of a cell (cached): about its built-up blocks
		const std::vector<Street> &Streets (int row, int col);
		/// The obstacles within radius [m] of a point (planet frame)
		void Obstacles (const Vector &p, double radius, std::vector<const convex::Polytope*> &out);
		/// Built-up share of the ground at a point (0-1; for the vegetation)
		double Urban (const Vector &dir);
		/// Built-up share of the ground at a point (0-1, as the blocks are
		/// zoned, bases not cleared; any thread: TerrainDetail)
		double Built (const Vector &dir);
		double Built (const Vector &dir, double lat, double lng); ///< (its latitude and longitude known)
		/// The ground under the buildings and trees at a point [m] (any
		/// thread): the elevation data are surface models (in cities their
		/// buildings, towers up to some 80 m in downtown Houston); the lowest
		/// eighth of the data (sea level left out) within about 180 m, at
		/// nodes 0.0025 deg apart, interpolated; NaN where there are none
		double Ground (double lat, double lng);
		/// whether a point (planet frame) is within margin [m] of a building's footprint
		bool Occupied (const Vector &p, double margin);

		/// Clear the ground of buildings within radius [m] of a point (planet
		/// frame); cells already made are made again
		void Clear (const Vector &p, double radius);
		/// incremented when cells change (Clear)
		unsigned Generation () const { return generation; }

	private:
		struct CellData {
			std::vector<Building> b;
			std::vector<convex::Polytope> obst; // (made on demand)
			bool obstDone = false;
			uint64_t used = 0;
		};
		void Load ();
		void Make (int row, int col, double minHeight, CellData &cd);
		/// the street grid a point belongs to (index of its centre; -1: none near)
		int Owner (const Vector &dir) const;
		/// ... its id (none near: the grid of its degree square, < 0), the grid
		int64_t OwnerId (const Vector &dir) const;
		Grid GridOf (int64_t id) const;
		/// the blocks of a cell: fn (grid, block indices, direction of its centre)
		void Blocks (int row, int col, const std::function<void(const Grid&, int64_t, int64_t, const Vector&)> &fn);
		/// a block's zoning inputs; false if it is not built up
		bool Zoned (const Grid &g, int64_t bi, int64_t bj, const Vector &bdir, Zone &z);
		/// nearness to high-rise cores (0-1) and their tower height [m]; the
		/// built-up share about downtowns (0-1, regardless of the lights)
		void Core (const Vector &dir, double &core, double &hMax, double &built) const;
		int StyleAt (double lat, double lng) const;
		bool Water (double lat, double lng);
		void Near (const Vector &dir, double radius, std::vector<int> &out) const; // centres within radius [m]
		void NearScan (const Vector &dir, double radius, std::vector<int> &out) const; // (Near: the bins' search)
		double Clearance (const Vector &dir) const; // 0 at bases and cleared ground, 1 away from them
		void Trim ();
		int Rows () const;
		int Cols (int row) const;

		const Planet *planet;
		double R;
		bool active = false;
		uint32_t seed;
		std::vector<Style> styles;
		struct Region { int style; double lat0, lat1, lng0, lng1; };
		std::vector<Region> regions;
		std::vector<Centre> centres;
		int nDowntown = 0;
		std::map<int, std::vector<int>> bins; // centres by 1 deg bins (key: lat bin * 1000 + lng bin)
		std::unique_ptr<Lights> lights;
		std::unique_ptr<scatter::Cover> cover; // water
		std::map<uint64_t, CellData> cells;
		struct StreetCell { std::vector<Street> s; uint64_t used = 0; };
		std::map<uint64_t, StreetCell> streetCells;
		uint64_t clock = 0;
		std::unique_ptr<std::vector<ElevationTile>> etile;
		struct Clearing { Vector dir; double r0, r1; };
		std::vector<Clearing> clear;
		bool basesDone = false;
		unsigned generation = 0;
		// the ground (Ground): its nodes, made from the data alone by elevation
		// managers of our own (one per thread at a time)
		// in blocks of GBLOCK x GBLOCK nodes
		static const int GBLOCK = 8;
		typedef std::array<float, GBLOCK * GBLOCK> GroundBlock;
		std::shared_mutex groundMtx;
		std::unordered_map<int64_t, std::shared_ptr<const GroundBlock>> groundBlocks;
		const uint64_t serial;     // (this map, for the threads' caches of its blocks: not its address, reused)
		struct RawElev { std::mutex m; std::unique_ptr<::ElevationManager> em; std::unique_ptr<std::vector<ElevationTile>> tiles; };
		std::vector<std::unique_ptr<RawElev>> rawElev;
		std::mutex rawMtx;
		float GroundNode (int64_t i, int64_t j);
		std::shared_ptr<const GroundBlock> MakeGroundBlock (int64_t bi, int64_t bj);
		static int64_t NodeKey (int64_t i, int64_t j) { return i * 1000003 + j; }
	};

} // namespace cities

#endif // !__CITIES_H

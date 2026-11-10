// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Procedural relief for planets without elevation data (the exoplanets of
// other systems): continents and basins, mountain ranges, ridges and
// craters, defined on the sphere and deterministic. The elevation tiles
// are made from it on demand, by the core (ElevationManager: the ground of
// the physics) and through the API for the graphics client
// (oapiProcElevationTile), so that both see the same ground. The finer
// terrain detail (TerrainDetail) continues it below the tiles' spacing.
//
// Parameters (planet configuration files):
//   SurfaceClass      Rock, Ocean, Ice, Desert, Lava or Gas (the look)
//   SurfaceSeed       the world's seed
//   SurfaceRelief     [m] continents and basins (one standard deviation)
//   SurfaceMountains  [m] ranges and ridges
//   SurfaceSea        share of the surface below the datum (0 to 1)
//   SurfaceCraters    large craters (0: none; 1: saturated)
//   SurfaceMaxLevel   finest elevation tile level (default 12)
// and for the graphics client:
//   SurfaceOpenWater  [deg] open water about the substellar point (tidally
//                     locked oceans), else the latitude of the ice edges
//   SurfaceColours    albedo r g b of the lowlands, then of the highlands
//                     (a gas giant's belts and zones)
// =======================================================================

#ifndef __PROCTERRAIN_H
#define __PROCTERRAIN_H

#include <cstdint>
#include <istream>

class ProcTerrain {
public:
	enum Class { ROCK, OCEAN, ICE, DESERT, LAVA, GAS };
	struct Params {
		int cls = ROCK;
		uint32_t seed = 1;
		double relief = 2000.0, mountains = 1500.0, sea = 0.0, craters = 0.5;
		int maxLevel = 12;
		// (the look: for the graphics client)
		double openWater = 0.0;   // [deg]
		double colLow[3] = { 0.20, 0.19, 0.18 }, colHigh[3] = { 0.36, 0.34, 0.31 };
	};
	/// Parameters from a planet's configuration (false: no SurfaceClass)
	static bool Read (std::istream &cfg, Params &prm);
	static const char *ClassName (int cls);

	/// radius: the planet's mean radius [m]
	ProcTerrain (const Params &prm, double radius);
	const Params &Prm () const { return prm; }

	/// Elevation [m] at a point of the unit sphere (planet frame: y north,
	/// longitude from x towards z), wavelengths down to lmin [m]
	/// (seas: their surface, the datum; seabed: the ground under them)
	double Height (double x, double y, double z, double lmin, bool seabed = false) const;

	/// An elevation tile (the elevation files' layout: 259 x 259 nodes,
	/// rows south to north, a border of one node before and two after the
	/// 256 intervals) of level lvl (4: the hemispheres), in units of res
	/// [m]; wavelengths down to two node spacings of level trunc (<= lvl:
	/// coarser; 0: lvl)
	void Tile (int lvl, int ilat, int ilng, int trunc, double res, int16_t *elev, bool seabed = false) const;

	/// the shortest wavelength [m] of tiles of level lvl
	double LambdaOf (int lvl) const;

	/// The ground's cover as the graphics client draws it (its climate: open
	/// water, sea ice, the slush shore, snow) at a point of the unit sphere:
	/// the same noise and rules as OVP/VulkanClient/ProcSurface.cpp (Texels)
	/// at its finest texture level. Used by the ground scatter (A2); the
	/// place for site-scale materials (F3).
	struct Ground {
		double seabed = 0.0; ///< [m] the ground under the seas (Ocean only; else 0)
		double theta = 0.0;  ///< [rad] from the substellar point (tidally locked), else |latitude|
		double edge = 0.0;   ///< [rad] theta - (open water + 0.06 n1 + 0.015 nf): > 0 beyond the open water
		double n1 = 0.0, nf = 0.0; ///< the client's climate noise
		double sea = 0.0;    ///< 0-1: not ground: open water, sea ice, floes (Ocean), open water or slush (Ice)
		double snow = 0.0;   ///< 0-1: snow over the ground (Ocean land)
		double coast = 0.0;  ///< 0-1: coastal sediment (1 at the datum, 0 from 150 m up; Ocean)
	};
	Ground GroundAt (double x, double y, double z, bool tidalLock) const;

	/// a texel [rad] of the client's finest colour tiles (the edge blends)
	double TexelAngle () const;

private:
	double Continent (const double p[3], double lmin) const;
	double Ridges (const double p[3], double lmin) const;
	double Craters (const double p[3], double lmin) const;
	Params prm;
	double R;
	double c0 = 0.0, cs = 1.0; // the continents' datum (quantile SurfaceSea) and spread
	double rot[16][9];         // octave rotations
	double ofs[16][3];         // octave offsets
};

#endif // !__PROCTERRAIN_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// A planet's sea for the physics: where the open water is, its liquid, the
// ground under it (the seabed), and its swell (SeaSwell.h).
//
// Open water:
//   - Earth and other bodies with elevation data: the water mask tiles say
//     water, and the terrain is at the datum (within 0.5 m; the data hold
//     the sea's surface, not its floor). Where there are no mask tiles (Earth
//     poleward of 56 deg), dark blue texels of the surface tiles at the datum.
//     No tiles at all (no planet textures installed): no water.
//   - Procedural Ocean worlds: below the datum and not under the sea ice
//     (ProcTerrain::GroundAt, as the client draws it).
//   - Procedural Ice worlds with open water: the open water about the
//     substellar point, its surface the terrain's elevation.
//   - A floating deck (ElevationManager::AddDeck) over the water is ground.
// Only bodies with a section in Config/Sea.cfg have a sea.
//
// The seabed: the procedural worlds' own; elsewhere (no bathymetry in the
// data) a nominal floor, FloorDepth below the surface, shelving up to the
// shore at ShelfSlope (the distance to the nearest land in the mask).
//
// The elevation (Planet::Elevation, ElevationManager) is still the sea's
// surface, its mean level: altitudes, the HUD and the autopilots don't
// change. Contacts take Planet::SolidElevation, the seabed under water.
// =======================================================================

#ifndef __SEA_H
#define __SEA_H

#include "SeaSwell.h"
#include "Vecmat.h"
#include <memory>
#include <string>
#include <unordered_map>

class Planet;
namespace scatter { class Cover; }

namespace sea {

	/// Config/Sea.cfg: a body's section
	struct Config {
		bool found = false;
		double wind = -1.0;        ///< [m/s] wind 10 m above the water (-1: the core's wind model)
		double density = 1025.0;   ///< [kg/m^3]
		double viscosity = 1.19e-6; ///< [m^2/s] kinematic
		double floorDepth = 40.0;  ///< [m] the nominal seabed below the surface
		double shelfSlope = 0.02;  ///< the seabed's rise towards the shore
		double lambdaMin = 4.0;    ///< [m] the swell's shortest wave
		uint32_t seed = 0;         ///< (0: from the body's name)
	};

	/// Read Config/Sea.cfg (once) for a body
	Config ReadConfig (const char *body);

	/// What lies at a point of the surface
	struct Point {
		bool water = false;  ///< open water
		double datum = 0.0;  ///< [m] elevation of the water's mean surface
		double seabed = 0.0; ///< [m] elevation of the ground under the water (water only)
	};

	class Sea {
	public:
		Sea (const Planet *planet);
		~Sea ();

		/// a sea on this planet (a Sea.cfg section; and a surface)
		bool Active () const { return active; }
		const Config &Cfg () const { return cfg; }
		double Density () const { return cfg.density; }
		double Viscosity () const { return cfg.viscosity; }
		const Swell &GetSwell () const { return swell; }

		/// the surface at lat, lng [rad]: water or not, the datum, the seabed
		/// (seabed: also look up the seabed, else seabed = datum - floorDepth)
		Point At (double lat, double lng, bool seabed = true);
		/// open water at lat, lng
		bool Water (double lat, double lng) { return At (lat, lng, false).water; }

		/// time base of the swell [s] for a time given as MJD
		static double SwellTime (double mjd) { return (mjd - 51544.5) * 86400.0; }

		/// the wind 10 m above the water: speed [m/s], bearing [rad] (towards, clockwise from north)
		void Wind (double &speed, double &bearing) const;

		/// counts for tests and logs
		struct Stats { uint64_t queries = 0, cacheHits = 0, seabedSearches = 0; };
		const Stats &GetStats () const { return stats; }

	private:
		bool MaskWater (double lat, double lng, bool &haveData);
		double ShoreDistance (double lat, double lng, double maxDist);
		const Planet *planet;
		Config cfg;
		bool active = false;
		Swell swell;
		std::unique_ptr<scatter::Cover> cover;
		// (results by ~10 m cell: vessels stay where they are)
		struct Cached { bool water; bool hasSeabed; double datum, seabed; };
		std::unordered_map<uint64_t, Cached> cache;
		Stats stats;
	};

} // namespace sea

#endif // !__SEA_H

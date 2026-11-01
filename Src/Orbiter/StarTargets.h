// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Named stars: the targets of interstellar travel. The stars named in
// Config/CSphere/Sky/stars_named.csv (Utils/SkyKit/build_names.py: the IAU
// names and common ones, at the positions and velocities of the sky's star
// catalogue, stars.bin, in full precision), and the Galactic Centre of the
// sky's model of the Galaxy (GalacticFrame.h). A trip arrives at rest at a
// point near one (no star bodies: points only).
//
// Positions are heliocentric (the Sun at rest; the Galaxy's rotation over
// the millennia of a trip, ~0.04 deg per 26,000 years at the Sun, is left
// out, as by the sky's model). A star moves at its catalogue velocity; the
// catalogue has it where it was seen from the Sun at its epoch, its light
// then d/c old: where it is at time t is its position then, moved over
// t - epoch + d/c.
// =======================================================================

#ifndef __STARTARGETS_H
#define __STARTARGETS_H

#include "Vecmat.h"
#include <cstdint>
#include <string>
#include <vector>

namespace startargets {

	struct Star {
		std::string name;        ///< e.g. "Sirius"
		std::string designation; ///< e.g. "HR 2491"
		std::string con;         ///< constellation (IAU abbreviation; may be empty)
		int64_t index = -1;      ///< in stars.bin (-1: a point, the Galactic Centre)
		uint64_t id = 0;         ///< stars.bin's identifier
		uint32_t hip = 0;        ///< Hipparcos number (0: none)
		double pos[3] = {0, 0, 0}; ///< heliocentric Galactic [pc] (as seen from the Sun at the epoch)
		double vel[3] = {0, 0, 0}; ///< [km/s]
		double mv = 0.0;         ///< absolute visual magnitude
		double teff = 0.0;       ///< effective temperature [K] (0: not a star)
		int follow = -1;         ///< an entry of a world's star orbiting another (Primary =): the other's place (its own: the body's)
		bool IsStar () const { return teff > 0.0; }
	};

	class Catalogue {
	public:
		/// Read the named stars (false if missing: only the Galactic Centre)
		bool Load (const char *namedCsv, const char *starsBin);
		const std::vector<Star> &Stars () const { return stars; }
		int Find (const char *name) const;     ///< by name (case and '_' for ' ' ignored); -1: none
		double Epoch () const { return epoch; } ///< of the positions [Julian year]

		/// The state of star i in Orbiter's global frame (ecliptic, relative
		/// to the global frame's origin) at simulation time simt
		void State (int i, double simt, Vector &x, Vector &v) const;
		/// Heliocentric Galactic position [pc] of star i at simulation time simt
		void GalacticPos (int i, double simt, double p[3]) const;
		/// The default distance of an arrival [m]: where the star's light is
		/// as bright as the Sun's at 1 AU (bolometric, as a black body); the
		/// Galactic Centre: 1 pc
		double ArrivalDistance (int i) const;
		/// Bolometric luminosity [L_sun] of star i (as a black body of its temperature)
		double Luminosity (int i) const;
		/// Radius [m] of star i (from its luminosity and temperature)
		double Radius (int i) const;
		/// Add an entry (a star or black hole of the world placed by its
		/// configuration, Config/Systems); its index (one of the name: replaced)
		int Add (const Star &s);
		/// absolute visual magnitude of a black body of luminosity lum [L_sun], temperature teff
		static double AbsMagV (double lum, double teff);

	private:
		std::vector<Star> stars;
		double epoch = 2016.0;
	};

	/// The named stars, read on first use
	const Catalogue &Get ();
	/// (Catalogue::Add on the named stars)
	int AddEntry (const Star &s);

} // namespace startargets

#endif // !__STARTARGETS_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Star - logical interface

#ifndef __STAR_H
#define __STAR_H

#include "Celbody.h"
#include <string>

/// The axes of an orbit seen on the sky (Thiele-Innes) at a star in
/// direction dir from the Sun (unit vector, Orbiter's frame): P towards the
/// periapsis, Q a quarter turn on in the direction of motion, in Orbiter's
/// frame. i, node (position angle of the ascending node, from north through
/// east), w: [deg]; the line of sight (away from the Sun) is the orbits' +z,
/// the ascending node the one where the body moves away from the Sun.
void SkyOrbitAxes (const Vector &dir, double i, double node, double w, Vector &P, Vector &Q);

class Star: public CelestialBody {
	friend class VStar;

public:
	Star (double _mass, double _mean_radius);

	Star (char *fname);
	// create a star from a config file

	~Star ();

	void Setup ();

	int Type() const { return OBJTP_STAR; }

	void Update (bool force = false);
	// Perform time step

	Vector Pos2Barycentre (Vector &pos);

	Vector4 GetLightColor();

	/// A star of the sky's catalogue (Catalogue = <named star> in its
	/// configuration, StarTargets.h): moving at its catalogue velocity; one
	/// of a binary (Binary = <the other's body name>) on the pair's orbit
	/// about their barycentre (the visual orbit, Orbit = P [yr] T [yr] e
	/// a ["] i Omega omega [deg], in one of the two: the other's motion
	/// relative to it). A star or black hole placed by its configuration
	/// (Place = l b [deg] d [pc], Galactic; Velocity = U V W [km/s],
	/// heliocentric Galactic): a catalogue entry of its own. A star orbiting
	/// another (Primary = <its body name>, Orbit = as above, or OrbitAU = the
	/// same with a in AU): on that orbit about it (the primary's reflex
	/// motion left out), its catalogue entry at the primary's place
	bool AnchoredState (double t, Vector &x, Vector &v) const override;
	int CatalogueIndex () const { return catIndex; } ///< (-1: not of the catalogue)
	/// (Psys: the primary of a star orbiting another, by its body name in psys)
	void LinkPrimary (const class PlanetarySystem *psys);
	const Star *OrbitPrimary () const { return orbPrimary; }
	/// the name of the star system it belongs to (Config/Systems: its Name;
	/// empty: the Solar System)
	const std::string &SystemName () const { return sysName; }
	void SetSystemName (const std::string &n) { sysName = n; }
	/// effective temperature [K] and luminosity [L_sun] (0: unknown)
	double Teff () const { return teff; }
	double Luminosity () const { return lum; }
	/// a ring of dust about it (a debris disc: DustRing = radius [AU], width
	/// (FWHM) [AU], fractional luminosity, i, node [deg] as seen from the Sun)
	struct DustRing { double radius = 0.0, width = 0.0, fraction = 0.0, incl = 0.0, node = 0.0; };
	const DustRing *Dust () const { return dust.radius > 0.0 ? &dust : nullptr; }
	/// its axis (global frame, unit)
	Vector DustAxis () const;

private:
	int catIndex = -1;
	std::string sysName;
	std::string binaryName;           // the other of a binary
	mutable const Star *binary = nullptr;
	bool hasOrbit = false;            // (the pair's orbit: the other's motion relative to this one)
	bool orbitAU = false;             // (its a in AU, not arcseconds)
	std::string primaryName;          // a star orbiting another: the other
	const Star *orbPrimary = nullptr;
	double orbP = 0.0, orbT = 0.0, orbE = 0.0, orbA = 0.0, orbI = 0.0, orbNode = 0.0, orbW = 0.0;
	double teff = 0.0, lum = 0.0;
	DustRing dust;
	/// the pair's relative position (the secondary from the primary, global
	/// frame [m]) at t, seen from the Sun in direction dir at distance d [pc]
	Vector RelativeOrbit (double t, const Vector &dir, double dpc) const;
	const Star *Binary () const;
};

#endif // !__STAR_H
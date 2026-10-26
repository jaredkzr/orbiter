// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Gas giants: a deep atmosphere without a solid surface. Altitude 0 (the
// planet's radius) is the 1-bar level. Below it the temperature follows
// the dry adiabat; above it, a lapse rate up to the tropopause, then
// isothermal. Pressure and density are hydrostatic.
// Winds: zonal jets by latitude at the cloud level, decaying above the
// clouds and strengthening below them (as the Galileo probe found), with
// convective storm cells (deterministic, drifting with the jets) that add
// updrafts and turbulence. The graphics client draws the same storms
// (oapiGetStorm).
// Configuration (Config/<Planet>.cfg): GasGiant = TRUE and the GG* keys
// (see Load).
// =======================================================================

#ifndef __GASGIANT_H
#define __GASGIANT_H

#include "Vecmat.h"
#include <istream>
#include <vector>

#include "OrbiterAPI.h" // (ATMPARAM)

class GasGiant {
public:
	/// Read the parameters from a planet's configuration (false: not a gas
	/// giant). R, gamma: the atmosphere's gas constant and heat capacity
	/// ratio; g: gravity at the 1-bar level [m/s^2]
	bool Load (std::istream &cfg, double R, double gamma, double g);

	/// Temperature, pressure, density at altitude alt [m] above the 1-bar level
	void AtmParam (double alt, ATMPARAM *prm) const;

	/// A convective storm at a point: strength 0..1 (the cell's activity at
	/// time t [s]), and the cell's updraft there [m/s]
	struct Storm { double strength, updraft; };
	Storm StormAt (double lng, double lat, double alt, double t) const;

	/// The storm cells within range [m] of a point (active ones, up to max):
	/// centre longitude and latitude [rad], radius [m], strength 0..1 (the
	/// cell's activity), peak updraft [m/s]
	struct Cell { double lng, lat, radius, strength, updraft; unsigned id; };
	int StormCells (double lng, double lat, double range, double t, Cell *cells, int max) const;

	/// The steady wind (local horizon frame: x east, y up, z north) [m/s]
	Vector Wind (double lng, double lat, double alt, double t) const;

	/// Eastward jet speed at the cloud level at latitude lat [rad] (m/s)
	double Jet (double lat) const;

	/// Turbulence amplitude at a point [m/s] (gusts: the caller's random,
	/// time-correlated perturbation of this size)
	double Turbulence (double lng, double lat, double alt, double t) const;

	double CrushPressure () const { return crushP; }
	double Pressure1 () const { return P1; }
	double Density1 () const { return P1 / (R * T1); }

private:
	double R = 3615.0, gamma = 1.4, g = 24.8;
	double P1 = 1e5;          // pressure at altitude 0 [Pa]
	double T1 = 165.0;        // temperature at altitude 0 [K]
	double lapseDown = 0.0;   // below altitude 0 [K/m] (0: the dry adiabat g/cp)
	double lapseUp = 1.5e-3;  // above it, to the tropopause [K/m]
	double Ttrop = 110.0;     // tropopause temperature [K]
	double crushP = 0.0;      // [Pa] beyond: vessels are crushed (not enforced yet)
	double windDecay = 50e3;  // [m] scale height of the jets' decay above the clouds
	double windDeep = 0.7;    // relative increase of the jets below the clouds ...
	double windDeepAlt = -60e3; //   ... reached at this altitude [m]
	double cloudAlt = 0.0;    // altitude of the jets' reference level [m]
	double turb = 3.0;        // background turbulence [m/s]
	double stormDensity = 0.3; // share of storm cells that are active
	double stormSize = 1.5e6; // storm cell size [m]
	double stormUpdraft = 40.0; // peak updraft [m/s]
	double stormTurb = 15.0;  // turbulence in a storm [m/s]
	double radius = 7e7;
	std::vector<double> jetLat, jetU; // zonal wind profile: latitude [rad], eastward speed [m/s]
	double Tz (double alt) const;
	friend class Planet;
};

#endif // !__GASGIANT_H

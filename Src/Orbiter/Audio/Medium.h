// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The air sound travels in: how loud a sound is in it, how much of its
// high end the air absorbs over a distance, how fast it travels.
//
// Loudness: the same acoustic power gives a pressure at the ear that goes
// with the square root of the air's impedance (rho c). Split between the
// source's air (how well it couples into the air) and the listener's
// (how much pressure reaches the ear): a fourth root each, so that in one
// air the sound is sqrt(rho c / rho0 c0) of sea level's. On Mars: about
// 20 dB quieter, as measured by Perseverance. In vacuum, at either end:
// nothing.
//
// Absorption: about f^2 / rho (classical and relaxation absorption both
// fall with density), at sea level 0.028 dB/m at 4 kHz (ISO 9613-1,
// 20 C, 50 % humidity). A path's absorption is a low-pass: the cutoff
// where the path absorbs 3 dB.
//
// Realism: Physical uses the air as it is. Cinematic: sound everywhere,
// as in films: a thin reference air where there is less, no travel time.
// =======================================================================

#ifndef __AUDIOMEDIUM_H
#define __AUDIOMEDIUM_H

namespace audio {

/// Sea level on Earth: the reference air
constexpr double RHO0 = 1.225;   // [kg/m^3]
constexpr double C0 = 340.3;     // [m/s]
/// Below this density there is no air to carry sound [kg/m^3]
constexpr double RHO_VACUUM = 1e-7;

enum Realism { PHYSICAL, CINEMATIC };

struct Air {
	double rho = 0.0;   // density [kg/m^3]
	double T = 0.0;     // temperature [K]
	double p = 0.0;     // pressure [Pa]
	double c = 0.0;     // speed of sound [m/s]

	bool Present () const { return rho > RHO_VACUUM && c > 0.0; }
	/// Air of a density, temperature and gas (R, gamma)
	static Air From (double rho, double T, double p, double R, double gamma);
	static Air SeaLevel ();
};

/// The air sound behaves in for a realism setting: Cinematic gives at
/// least CINEMATIC_RHO of sea level's density
Air Effective (const Air &a, Realism r);
constexpr double CINEMATIC_RHO = 0.25;

/// Coupling into (source) or out of (listener) the air: (rho c / rho0 c0)^(1/4)
double Coupling (const Air &a);

/// Spherical spreading: 1 within the reference distance, then ref/d
double Spreading (double dist, double refDist);

/// Absorption over a path [dB] at a frequency
double AbsorptionDb (const Air &a, double dist, double freq);
/// The path's low-pass cutoff [Hz] (where it absorbs 3 dB), at most `fmax`
double AbsorptionCutoff (const Air &a, double dist, double fmax = 20000.0);

/// Ratio of received to emitted frequency, for the unit vector `n` from
/// source to listener and their velocities in the air (vs, vl)
/// (equivalently dtau/dt, the rate of the emission time heard; its
/// magnitude: a sound heard backwards plays as fast), within 1/8..8
double DopplerFactor (const double n[3], const double vs[3], const double vl[3], double c);

} // namespace audio

#endif // !__AUDIOMEDIUM_H

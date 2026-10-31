// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The Galactic frame, for positions beyond the solar system.
//
// Heliocentric Galactic coordinates [pc]: x toward the Galactic centre
// (l = 0, b = 0), y toward l = 90 deg (the direction of Galactic
// rotation), z toward the north Galactic pole. Right-handed, unlike
// Orbiter's ecliptic frame (x, y = ecliptic north, z), which is
// left-handed.
//
// Galactocentric coordinates [pc]: the same axes about the Galactic
// centre, which lies at heliocentric (R0, 0, -zSun).
//
// A sky (or anything placed in the Galaxy) is described by an Observer:
// its heliocentric Galactic position, velocity and time. The solar
// system's frame gives one; another star system's frame will give
// another. Positions are kept in double precision: a float resolves
// only ~0.001 pc (200 AU) at the Galactic centre's distance.
// =======================================================================

#ifndef __GALACTICFRAME_H
#define __GALACTICFRAME_H

#include <cmath>

namespace galactic {

	const double PARSEC = 3.0856775814913673e16; ///< [m]
	const double R0 = 8150.0;  ///< Sun's distance from the Galactic centre [pc] (Reid et al. 2019)
	const double ZSUN = 20.8;  ///< Sun's height above the Galactic mid-plane [pc] (Bennett & Bovy 2019)

	/// Rotation from the ICRS (equatorial J2000) to the Galactic frame, by
	/// the Hipparcos definition of the Galactic pole and centre (ESA 1997)
	static const double ICRS_TO_GALACTIC[3][3] = {
		{ -0.0548755604162154, -0.8734370902348850, -0.4838350155487132 },
		{  0.4941094278755837, -0.4448296299600112,  0.7469822444972189 },
		{ -0.8676661490190047, -0.1980763734312015,  0.4559837761750669 } };

	/// Galactic unit vector of the direction of right ascension ra and
	/// declination dec (ICRS) [deg]
	inline void EquatorialToGalactic (double ra, double dec, double gal[3])
	{
		const double d2r = 3.14159265358979323846 / 180.0;
		double e[3] = { std::cos (dec * d2r) * std::cos (ra * d2r), std::cos (dec * d2r) * std::sin (ra * d2r), std::sin (dec * d2r) };
		for (int i = 0; i < 3; i++)
			gal[i] = ICRS_TO_GALACTIC[i][0] * e[0] + ICRS_TO_GALACTIC[i][1] * e[1] + ICRS_TO_GALACTIC[i][2] * e[2];
	}

	/// Rotation from Orbiter's ecliptic J2000 frame (left-handed: x, y =
	/// ecliptic north, z) to the Galactic frame (right-handed), row-major:
	/// gal[i] = sum_j R[i][j] ecl[j]. Ecliptic -> equatorial by the J2000
	/// obliquity (84381.406 arcsec, IAU 2006), equatorial -> Galactic as
	/// ICRS_TO_GALACTIC.
	inline void EclipticToGalactic (double R[3][3])
	{
		const double (&N)[3][3] = ICRS_TO_GALACTIC;
		const double eps = 84381.406 / 3600.0 * 3.14159265358979323846 / 180.0;
		const double ce = std::cos (eps), se = std::sin (eps);
		// Orbiter (x, y, z) -> right-handed ecliptic (x, z, y) -> equatorial
		// (x, z cos e - y sin e, z sin e + y cos e)
		const double E[3][3] = {
			{ 1.0, 0.0, 0.0 },
			{ 0.0, -se, ce },
			{ 0.0, ce, se } };
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				R[i][j] = N[i][0] * E[0][j] + N[i][1] * E[1][j] + N[i][2] * E[2][j];
	}

	/// Where (and how fast, when) a sky is seen from
	struct Observer {
		double pos[3] = { 0.0, 0.0, 0.0 }; ///< heliocentric Galactic position [pc]
		double vel[3] = { 0.0, 0.0, 0.0 }; ///< velocity relative to the Sun [km/s]
		double mjd = 51544.5;              ///< time (modified Julian date)
	};

	/// Heliocentric Galactic position [pc] of a point in Orbiter's global
	/// (ecliptic, barycentric) frame [m]
	inline void GlobalToGalactic (const double ecl[3], double gal[3])
	{
		double R[3][3];
		EclipticToGalactic (R);
		for (int i = 0; i < 3; i++)
			gal[i] = (R[i][0] * ecl[0] + R[i][1] * ecl[1] + R[i][2] * ecl[2]) / PARSEC;
	}

} // namespace galactic

#endif // !__GALACTICFRAME_H

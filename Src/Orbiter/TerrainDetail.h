// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Procedural terrain detail: elevation finer than the planet's elevation
// data. Two parts, both defined on the sphere (continuous across tiles)
// and deterministic, so that physics (ElevationManager::Elevation) and
// graphics clients (oapiSurfaceDetail) get the same heights:
//
// - fractal noise continuing the spectrum of the data below its node
//   spacing, its strength following the local relief of the data, with a
//   floor for plains (not below sea level on bodies with oceans)
// - craters (airless bodies, Mars), sizes in a power law (equilibrium
//   cratering: the same share of the ground at each size), fresh and
//   degraded ones
//
// Both fade out around surface bases. Wavelengths reach down to
// lambdaMin, four node spacings of the finest mesh of the detail
// (TerrainDetailLevel): the geometry shows them, and the ground contact
// matches what is seen. Finer detail is left to shading (the Vulkan
// client evaluates the same function there).
//
// Parameters (planet configuration files; defaults by body):
//   TerrainDetail             strength of the noise (0: no detail at all)
//   TerrainDetailHurst        octave amplitudes ~ wavelength^Hurst
//   TerrainDetailRidge        ridged share of the octaves of 8 m and more (0 to 1)
//   TerrainDetailFloor        relief of plains, relative to the data's node spacing
//   TerrainDetailSea          no floor at or below sea level (bodies with oceans)
//   TerrainDetailCraters      crater cells per lattice cell (0: none; up to 4)
//   TerrainDetailCraterDepth  depth scale (1: depth a fifth of the diameter)
//   TerrainDetailCraterFresh  share of fresh craters (bright ejecta)
//   TerrainDetailLevel        finest mesh level of the detail (default: about
//                             1.2 m node spacing)
// =======================================================================

#ifndef __TERRAINDETAIL_H
#define __TERRAINDETAIL_H

#include "Vecmat.h"
#include <vector>

class Planet;

class TerrainDetail {
public:
	struct Params {
		double gain = 0.35, hurst = 1.0, ridge = 1.0, floor = 0.0;
		bool sea = false;
		double craters = 0.0, craterDepth = 1.0, craterFresh = 0.3;
		int level = 0;   // 0: default
	};
	/// Values set in a planet's configuration (NaN: not set)
	struct Overrides {
		double gain, hurst, ridge, floor, sea, craters, craterDepth, craterFresh, level;
		Overrides ();
		void Apply (Params &p) const;
	};
	/// Defaults by body: Earth, Mars, airless bodies, others
	static Params Defaults (const Planet *planet);

	TerrainDetail (const Planet *planet, const Params &prm);

	const Params &Prm () const { return prm; }
	/// the graphics client renders the detail (oapiSurfaceDetailSupported)
	static bool clientSupport;
	/// shortest wavelength [m] (four times the node spacing of the finest mesh)
	double LambdaMin () const { return lambdaMin; }
	/// node spacing [m] to use for planets without elevation data (flat
	/// data, relief 0: the detail's floor, wavelengths of a few km down)
	double FlatSpacing () const { return flatSpacing; }

	/// Detail elevation [m] at a point
	/// \param lat, lng position [rad]
	/// \param elev elevation of the data there [m]
	/// \param spacing node spacing of the elevation data there [m]
	/// \param relief local relief of the data there [m] (see Relief)
	/// \param lmin shortest wavelength [m] (at least LambdaMin; coarser meshes need less detail)
	/// \param amplitude if set, receives the noise amplitude factor A [m]:
	///   octave wavelength lambda has amplitude A*(lambda/spacing)^Hurst
	/// \param fade if set, receives the fade factor of the craters (bases)
	double Height (double lat, double lng, double elev, double spacing, double relief, double lmin = 0.0,
		double *amplitude = nullptr, double *fade = nullptr) const;

	/// Local relief at a point inside a cell of an elevation grid: the
	/// largest height difference of each corner node to its neighbours,
	/// interpolated over the cell. The grid must have a border of one node
	/// around the cell (as the elevation tiles do).
	/// \param e height of the cell's south-west node; rows south to north
	/// \param stride row length
	/// \param scale height unit [m]
	/// \param wlat, wlng position in the cell (0 to 1)
	template<typename T>
	static double Relief (const T *e, int stride, double scale, double wlat, double wlng)
	{
		auto node = [&](const T *p) {
			double h = p[0], r = 0.0;
			for (const T *q : { p - 1, p + 1, p - stride, p + stride }) {
				double d = h - *q;
				r = (d > r ? d : -d > r ? -d : r);
			}
			return r;
		};
		double r0 = node (e) * (1.0 - wlng) + node (e + 1) * wlng;
		double r1 = node (e + stride) * (1.0 - wlng) + node (e + stride + 1) * wlng;
		return (r0 * (1.0 - wlat) + r1 * wlat) * scale;
	}

private:
	double Fade (const Vector &dir) const; // 0 at the surface bases, 1 away from them
	void InitBases () const;

	const Planet *planet;
	Params prm;
	double radius;
	double lambdaMin;
	double flatSpacing;
	struct Flat { Vector dir; double r0, r1; }; // fully flat within r0, detail beyond r1 [m]
	mutable std::vector<Flat> flats;
	mutable bool basesDone = false;
};

#endif // !__TERRAINDETAIL_H

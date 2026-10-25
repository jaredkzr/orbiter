// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Procedural terrain detail: elevation finer than the planet's elevation
// data, added as fractal noise. Its strength follows the local relief of
// the data (flat ground such as runways and the sea stays flat), it is
// faded out around surface bases, and it is defined on the sphere, so it
// is continuous across tiles. The noise is deterministic: physics
// (ElevationManager::Elevation) and graphics clients (oapiSurfaceDetail)
// get the same heights.
//
// Wavelengths reach down to lambdaMin, which follows the finest mesh a
// client draws (MaxPatchResolution): the geometry can show them, and the
// ground contact matches what is seen. Finer detail is left to shading.
// =======================================================================

#ifndef __TERRAINDETAIL_H
#define __TERRAINDETAIL_H

#include "Vecmat.h"
#include <vector>

class Planet;

class TerrainDetail {
public:
	/// Detail of a planet: gain from the planet's configuration
	/// (TerrainDetail, default 0.35; 0 switches it off)
	TerrainDetail (const Planet *planet, double gain);

	bool Enabled () const { return gain > 0.0; }
	/// the graphics client renders the detail (oapiSurfaceDetailSupported)
	static bool clientSupport;
	/// shortest wavelength [m] (four times the node spacing of the finest mesh)
	double LambdaMin () const { return lambdaMin; }

	/// Detail elevation [m] at a point
	/// \param lat, lng position [rad]
	/// \param spacing node spacing of the elevation data there [m]
	/// \param relief local relief of the data there [m] (see Relief)
	/// \param lmin shortest wavelength [m] (at least LambdaMin; coarser meshes need less detail)
	/// \param amplitude if set, receives the amplitude factor A [m]: octave
	///   wavelength lambda has amplitude A*(lambda/spacing)^Hurst
	double Height (double lat, double lng, double spacing, double relief, double lmin = 0.0, double *amplitude = nullptr) const;
	double Hurst () const { return hurst; }

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
	double gain;
	double hurst = 1.0;
	double radius;
	double lambdaMin;
	struct Flat { Vector dir; double r0, r1; }; // fully flat within r0, detail beyond r1 [m]
	mutable std::vector<Flat> flats;
	mutable bool basesDone = false;
};

#endif // !__TERRAINDETAIL_H

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
// Both fade out around surface bases; the noise mostly on built-up
// ground (procedural cities, Cities.h), where the data are also levelled to
// the ground under their buildings and trees (CityMap::Ground). Wavelengths reach down to
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
//   TerrainDetailPebbles      pebble and clod cells per lattice cell (0: none;
//                             up to 4): mounds of 2.5 to 12 cm radius
//   TerrainDetailPebbleShape  Angular (default), Rounded or Platy: the stones' shape
//   TerrainDetailPebbleAlbedo albedo factor of the stones (0.3 to 2; shading only)
//   TerrainDetailMicro        slope floor (amplitude/wavelength, 0 to 0.05) of the
//                             noise octaves of 2 m and less (0: none)
//   TerrainDetailGrit         density of grains of a few mm (0 to 1; shading only)
//   TerrainDetailPlaya        strength of the playa rule (0 to 1; Earth 1): dry lakes
//                             and salt flats are flat (the Vulkan client also
//                             cracks them)
//   TerrainDetailSite         site materials on procedural worlds (0: off; 1, the
//                             default there: on; no effect elsewhere)
//   TerrainDetailLevel        finest mesh level of the detail (default: about
//                             2 cm node spacing: craters and pebbles are
//                             relief close up)
// Site materials (procedural worlds, SurfaceClass other than Gas): three
// sub-materials at scales of 30 m to 1 km, chosen by class (SitePreset): coarse
// (gravel lag, blocky ejecta), fine (sand, mature fines, snow) and flat (pans,
// glaze ice); between them the ground stays the base material. Two fractal
// fields on the detail's lattices (SiteWeights) give the weights; their thresholds
// follow from the materials' shares of the surface (measured once on the
// sphere). The mask sets the pebble density (Height: the stones' lattices are
// filled in sequence, so that gravel has more of them), the ground scatter's
// rocks (SiteRocks) and, in the Vulkan client, which evaluates the same mask in
// float, the ground colour, grit, cracks and smoothing. Only the pebbles change the
// height. Bodies without it keep their code paths bit for bit.
// Playa rule (Playa): ground whose surface texture is bright and unsaturated
// (neither blue like ice nor green; PlayaMap), below the snow line (by
// latitude and elevation) and on gentle data (relief below 15-40 m) is a dry
// lake or salt flat. The data are levelled to a smoothed reference ground there
// (PlayaMap::Ground: the data's swells and hummocks are not a lakebed; not into
// the sea; also near surface bases, but the pads' levels stay). Its weight w
// (the factor, 0 on landing pads; the levelling only where the data resolve hummocks, spacing below 2 x the level-10 spacing) also sets the noise amplitude (x0.05) and the fade (x0.2: no pebbles,
// craters or micro relief), and Height gives w to the client (cracks, blotch damping; no base fade). Elsewhere the heights
// are unchanged.
// Sea gate: on bodies with seas (TerrainDetailSea) the craters, pebbles and
// micro relief are off on the flat sea, so that the sea (and its ice) stays
// flat: at and below the data's sea level (fading in between 0.25 m and 1 m
// above it) where the data have no relief. Where the detail has relief (its
// amplitude A > 0, e.g. land below the datum on coarse data) the gate is open.
// =======================================================================

#ifndef __TERRAINDETAIL_H
#define __TERRAINDETAIL_H

#include "Vecmat.h"
#include <memory>
#include <mutex>
#include <vector>

class Planet;
class PlayaMap;
namespace cities { class CityMap; }

class TerrainDetail {
public:
	/// shape of the pebbles (see PebblePreset)
	struct PebbleShape { double size = 1.0, round = 0.0, elong = 0.0, height = 1.0, bevel = 0.6, tilt = 0.5, crag = 1.0, tone = 0.0; };
	enum { PEBBLE_ANGULAR, PEBBLE_ROUNDED, PEBBLE_PLATY, PEBBLE_SHAPES };
	static const PebbleShape &PebblePreset (int id);
	/// preset by name (case-insensitive); -1 if unknown
	static int ShapeByName (const char *name);

	struct Params {
		double gain = 0.35, hurst = 1.0, ridge = 1.0, floor = 0.0;
		bool sea = false;
		double craters = 0.0, craterDepth = 1.0, craterFresh = 0.3;
		double pebbles = 0.0; // pebble and clod cells per lattice cell (0: none; up to 4)
		PebbleShape shape;
		double pebbleAlbedo = 0.9, micro = 0.0, grit = 0.0;
		bool pebbleGate = false;
		double playa = 0.0; // strength of the playa rule (0: none)
		int level = 0;   // 0: default
		/// site materials (SitePreset): three materials, coarse, fine, flat
		struct Site {
			struct Mat {
				double tint[3] = { 1, 1, 1 };  // colour factor (linear rgb)
				double abs[3] = { 0, 0, 0 };   // colour mixed in ...
				double amt = 0.0;              // ... by this much
				double kp = 1.0, kg = 1.0;     // pebble and grit density factors
				double kc = 0.0, km = 0.0;     // crack weight, smoothing of mottling and grain (shading)
				double kr = 1.0;               // rock density factor (ground scatter)
			};
			bool on = false;
			uint32_t seed = 0;                 // lattice seed (< 65536)
			double edge = 0.03;                // edge softness (field units)
			double cover[3] = { 0, 0, 0 };     // shares of the surface
			Mat mat[3];
		};
		Site site;
	};
	/// Values set in a planet's configuration (NaN: not set)
	struct Overrides {
		double gain, hurst, ridge, floor, sea, craters, craterDepth, craterFresh, pebbles, level;
		double pebbleShape, pebbleAlbedo, micro, grit, playa, site;
		Overrides ();
		void Apply (Params &p) const;
	};
	/// Defaults by body: Earth, Mars, airless bodies, others
	static Params Defaults (const Planet *planet);
	/// Site materials of a surface class (ProcTerrain::Class; off for gas giants)
	static Params::Site SitePreset (int cls);

	TerrainDetail (const Planet *planet, const Params &prm);
	~TerrainDetail ();

	const Params &Prm () const { return prm; }
	/// the graphics client renders the detail (oapiSurfaceDetailSupported)
	static bool clientSupport;
	/// shortest wavelength [m] (four times the node spacing of the finest mesh)
	double LambdaMin () const { return lambdaMin; }
	/// node spacing [m] to use for planets without elevation data (flat
	/// data, relief 0: the detail's floor, wavelengths of a few km down)
	double FlatSpacing () const { return flatSpacing; }

	/// Site materials: weights w (coarse, fine, flat; sum at most 1) at a point
	/// (unit vector in the planet frame), at full resolution. Pure, thread safe.
	/// Only meaningful if Prm().site.on.
	void SiteWeights (const Vector &dir, double w[3]) const;
	/// the pebble density factor of weights w (1 + sum w_i (kp_i - 1), at least 0)
	double SiteDensity (const double w[3]) const;
	/// the rock density factor of weights w (1 + sum w_i (kr_i - 1), at least 0)
	double SiteRocks (const double w[3]) const;
	/// the largest factors (at least 1) over the materials
	double SiteKpMax () const { return siteKpMax; }
	double SiteKrMax () const { return siteKrMax; }
	/// thresholds (coarse where field A > [0], fine where A < [1], flat where field B > [2]; rounded to float)
	const double *SiteThresholds () const { return siteThr; }

	/// Detail elevation [m] at a point
	/// \param lat, lng position [rad]
	/// \param elev elevation of the data there [m]
	/// \param spacing node spacing of the elevation data there [m]
	/// \param relief local relief of the data there [m] (see Relief)
	/// \param lmin shortest wavelength [m] (at least LambdaMin; coarser meshes need less detail)
	/// \param amplitude if set, receives the noise amplitude factor A [m]:
	///   octave wavelength lambda has amplitude A*(lambda/spacing)^Hurst
	/// \param fade if set, receives the fade factor of the craters, pebbles and micro relief (bases, sea)
	/// \param playa if set, receives the playa weight pl*(1-wp) (0-1; no base fade: 0 on landing pads, where there is no playa and where the function returns before pl is known)
	double Height (double lat, double lng, double elev, double spacing, double relief, double lmin = 0.0,
		double *amplitude = nullptr, double *fade = nullptr, double *playa = nullptr) const;

	/// The playa factor (0-1) at a point (see the header comment): the
	/// texture's colour, the snow line, the data's relief
	/// \param elev elevation [m]
	/// \param relief local relief of the data [m] (0: left out)
	/// \param spacing node spacing of the data [m] (selects the colour level: coarse data read coarse
	///   texture tiles; 0: the finest, level 10)
	double Playa (double lat, double lng, double elev, double relief, double spacing = 0.0) const;

	/// Share (0-1) of the data's elevation replaced by the ground under the
	/// cities' buildings at a point (Height levels built-up ground to
	/// CityMap::Ground where the data's nodes are spacing [m] apart: the
	/// data are surface models, with the buildings and trees on it); its
	/// slopes are the rest's; also levelled to the smoothed ground on playas
	/// (elev, relief: as Height's)
	double Levelled (double lat, double lng, double spacing, double elev, double relief) const;

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
	/// the noise's fade factor: 0 at the surface bases, 1 away from them and
	/// built-up ground (only if built is given: its share, times wb);
	/// bases: the bases' part
	double Fade (const Vector &dir, double *bases = nullptr, double *built = nullptr, double wb = 1.0, const double *latlng = nullptr) const;
	void InitBases () const;
	/// share (0-1) of the data levelled to the smoothed ground on a playa of
	/// weight w, and that ground g [m] (0 without ground data)
	double PlayaLevel (double lat, double lng, double elev, double w, double &g) const;

	const Planet *planet;
	Params prm;
	double radius;
	double lambdaMin;
	double flatSpacing;
	double playaSpacing;                       // level-10 data spacing [m]
	double siteThr[3] = { 1e9, -1e9, 1e9 };    // site materials: thresholds of the two fields
	double siteKpMax = 1.0, siteKrMax = 1.0;   // largest pebble and rock factors (at least 1)
	double SiteField (const Vector &dir, bool b) const; // (field A or B)
	void InitSite ();
	struct Pad { Vector dir; double g; };     // a landing pad: its centre, its level (the data's there) [m]
	struct Grade { Vector dir, along, across; double hl, hw, g0, gl, gw; }; // graded ground: centre, axes, half extents [m], level g0 + gl s + gw t
	struct Flat { Vector dir; double r0, r1; std::vector<Pad> pads; std::vector<Grade> grades; double reach; }; // fully flat within r0, detail beyond r1; pads and grades within reach [m]
	mutable std::vector<Flat> flats;
	/// landing pads and graded ground (Base::Grades): the ground levelled
	/// to the pad's level within PAD_R0 of it, blended back to the data's
	/// by PAD_R1; graded areas likewise, blended over GRADE_BLEND (the share
	/// of the level returned; g: the level)
	double PadLevel (const Vector &dir, double &g) const;
	mutable cities::CityMap *cities = nullptr; // (flattened where built up)
	mutable std::once_flag basesOnce;          // (any thread)
	/// the flats that may reach a point (of those that may reach points about
	/// it: the thread's last few), in their order
	const std::vector<int> &FlatsNear (const Vector &dir) const;
	mutable std::once_flag playaOnce;          // (any thread)
	mutable std::unique_ptr<PlayaMap> playaMap;
	const uint64_t serial;                     // (this one, for the threads' caches: not its address, reused)
};

#endif // !__TERRAINDETAIL_H

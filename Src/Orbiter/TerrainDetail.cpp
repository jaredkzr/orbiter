// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "TerrainDetail.h"
#include "Planet.h"
#include "Base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

	// The noise is defined so that a GPU can evaluate the same function
	// (32-bit integer hash, exact octave rotations): the Vulkan client's
	// tile shader continues it below the resolution of its meshes.

	inline uint32_t Pcg (uint32_t v)
	{
		uint32_t state = v * 747796405u + 2891336453u;
		uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}

	inline uint32_t Hash (int32_t x, int32_t y, int32_t z, uint32_t seed)
	{
		return Pcg ((uint32_t)x + Pcg ((uint32_t)y + Pcg ((uint32_t)z + Pcg (seed))));
	}

	inline double Grad (uint32_t h, double x, double y, double z)
	{
		switch (h % 12u) {
		case 0:  return  x + y;
		case 1:  return -x + y;
		case 2:  return  x - y;
		case 3:  return -x - y;
		case 4:  return  x + z;
		case 5:  return -x + z;
		case 6:  return  x - z;
		case 7:  return -x - z;
		case 8:  return  y + z;
		case 9:  return -y + z;
		case 10: return  y - z;
		default: return -y - z;
		}
	}

	inline double Smoother (double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
	inline double Lerp (double a, double b, double t) { return a + (b - a) * t; }

	// gradient noise, about -1 to 1
	double Noise (double x, double y, double z, uint32_t seed)
	{
		double fx = std::floor (x), fy = std::floor (y), fz = std::floor (z);
		int32_t ix = (int32_t)fx, iy = (int32_t)fy, iz = (int32_t)fz;
		x -= fx, y -= fy, z -= fz;
		double u = Smoother (x), v = Smoother (y), w = Smoother (z);
		auto g = [&](int dx, int dy, int dz) { return Grad (Hash (ix + dx, iy + dy, iz + dz, seed), x - dx, y - dy, z - dz); };
		return Lerp (Lerp (Lerp (g (0,0,0), g (1,0,0), u), Lerp (g (0,1,0), g (1,1,0), u), v),
		             Lerp (Lerp (g (0,0,1), g (1,0,1), u), Lerp (g (0,1,1), g (1,1,1), u), v), w);
	}

	inline double SmoothStep (double a, double b, double x)
	{
		double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	// octaves: wavelengths 2^j m; crater layers: cells of 2^k m. Each
	// lattice m has its own orientation (one of the 24 rotations of the
	// cube: exact) and offset, so that the lattices don't line up
	constexpr int JMIN = -4, JMAX = 20;   // noise octaves (lattice m = j - JMIN)
	constexpr int KMIN = -2, KMAX = 16;   // crater layers (lattice m = k - JMIN + 32)
	Vector LatticePoint (int m, const Vector &d, double scale)
	{
		static const int P[6][3] = { {0,1,2}, {1,2,0}, {2,0,1}, {0,2,1}, {1,0,2}, {2,1,0} };
		static const double S[4][3] = { {1,1,1}, {1,-1,-1}, {-1,1,-1}, {-1,-1,1} };
		int k = m % 24;
		const int *p = P[k % 6];
		double sg = (k % 6) < 3 ? 1.0 : -1.0; // (odd permutations: all signs flipped)
		const double *s = S[k / 6];
		return Vector (sg * s[0] * d.data[p[0]] * scale + ((m * 37) & 63) / 64.0,
		               sg * s[1] * d.data[p[1]] * scale + ((m * 23) & 63) / 64.0,
		               sg * s[2] * d.data[p[2]] * scale + ((m * 53) & 63) / 64.0);
	}

	// the octaves' shape: gradient noise, with a ridged part (sharp crests,
	// rounded hollows, as in eroded rock) in octaves of 8 m and more
	inline double Shape (double n, int j, double ridge)
	{
		double r = ridge * std::clamp ((j - 3) / 3.0, 0.0, 1.0);
		return (1.0 - 0.4 * r) * n + 0.8 * r * (0.35 - std::fabs (n));
	}

	constexpr double CRATER_D = 0.22; // typical crater diameter / cell size (weight against the data's resolution)
	constexpr double CRATER_G = 0.1;  // resolution a crater needs / cell size (sharp rims: the smallest
	                                  // ones span six node spacings of a mesh whose lmin is four)

	inline double Unit16 (uint32_t h) { return (h & 0xffffu) / 65535.0; }
	constexpr double PEBBLE_G = 0.15; // resolution a pebble needs / cell size (six to twelve mesh nodes across: fewer alias)
	constexpr double PEBBLE_SOFT = 0.08; // rounding of the pebbles' corners (outline units)

	// One layer of craters (cells of size s = 2^k m): at most one crater per
	// cell, inside it with its rim (radius 0.08 to 0.22 cell sizes, the
	// centre anywhere it fits, so that the cells don't show as rows): a bowl
	// a fifth of the diameter deep with a raised rim, shallower when older.
	// Height [m].
	// A crater's shape (shared with the Vulkan client's terraindetail.glsl:
	// keep them identical). After LROC and Apollo images of small craters
	// (and depth/diameter measurements: fresh about 0.13 to 0.17, degraded
	// below 0.08): most are degraded, subdued and wider, a few fresh with
	// steep walls and a sharp rim; outlines not quite round (ellipticity
	// and lobes), rims of uneven height. The profile is smooth across the
	// rim (no crease).
	struct CraterShape {
		double rL;          // radius [cells]
		double depth, rim;  // floor below and crest above the surroundings [radii]
		double k;           // wall shape: (1 - x^2)^k (small: steep walls near the rim)
		double wr;          // rim's outer width [radii]
		double ecc, lobe;   // outline: ellipticity and three lobes
		Vector a1, a2, a3;  // their axes, and of the rim's height variation (unit, lattice frame)
		double age;         // degradation (0 fresh, 1 old)
	};

	Vector UnitFromHash (uint32_t h)
	{
		uint32_t h2 = Pcg (h), h3 = Pcg (h2);
		Vector v (Unit16 (h) * 2.0 - 1.0, Unit16 (h2) * 2.0 - 1.0, Unit16 (h3) * 2.0 - 1.0);
		double l = v.length();
		return l > 1e-6 ? v / l : Vector (1, 0, 0);
	}

	// the crater of a cell (h4: its 4th hash), depth scale of the body
	CraterShape MakeCrater (uint32_t h4, double depthScale)
	{
		CraterShape c;
		c.rL = 0.06 + 0.10 * Unit16 (h4);
		double u = Unit16 (h4 >> 16);
		c.age = u;
		c.depth = 0.32 * depthScale * (1.0 - 0.7 * c.age);        // (depth/diameter 0.16 to 0.05)
		c.rim = 0.08 * depthScale * (1.0 - 0.85 * c.age);
		c.k = 1.6 + 2.0 * c.age;
		c.wr = 0.6 + 0.2 * c.age;
		c.rL *= 1.0 + 0.12 * c.age;                               // (degradation widens)
		uint32_t h5 = Pcg (h4 ^ 0x9e3779b9u), h6 = Pcg (h5), h7 = Pcg (h6);
		c.ecc = 0.12 * Unit16 (h5);
		c.lobe = 0.06 * Unit16 (h5 >> 16);
		c.a1 = UnitFromHash (h5), c.a2 = UnitFromHash (h6), c.a3 = UnitFromHash (h7);
		return c;
	}

	// height [radii] at offset v from the crater's centre [cells]
	double CraterHeight (const CraterShape &c, const Vector &v)
	{
		double dist = v.length();
		if (dist < 1e-9) return -c.depth;
		Vector u = v / dist;
		double c1 = dotp (u, c.a1), c2 = dotp (u, c.a2), c3 = dotp (u, c.a3);
		double rr = c.rL * (1.0 + c.ecc * (2.0 * c1 * c1 - 1.0) + c.lobe * (4.0 * c2 * c2 * c2 - 3.0 * c2));
		double x = dist / rr;
		double rim = c.rim * (1.0 + 0.4 * c3);
		if (x < 1.0) return rim - (rim + c.depth) * std::pow (1.0 - x * x, c.k);
		double t = (x - 1.0) / c.wr;
		if (t >= 1.0) return 0.0;
		return rim * (1.0 - t * t) * (1.0 - t * t);
	}

	// One layer of pebbles and clods (cells of size s = 2^k m): at most one
	// per cell, an angular stone: its outline a polygon (five edges at
	// random), a bevelled flat top, tilted (radius 0.12 to 0.24 cell sizes,
	// height 0.25 to 0.55 times it). Fewer of the larger ones. Height [m].
	double PebbleLayer (const Vector &d, int k, int layer, double R, double density)
	{
		int m = k - JMIN + 64 + 29 * layer;
		double s = std::ldexp (1.0, k);
		Vector q = LatticePoint (m, d, R / s);
		double fx = std::floor (q.x), fy = std::floor (q.y), fz = std::floor (q.z);
		uint32_t h0 = Hash ((int32_t)fx, (int32_t)fy, (int32_t)fz, 3000u + (uint32_t)m);
		if (Unit16 (h0) >= density * (k == -1 ? 0.4 : k == -2 ? 0.7 : 1.0)) return 0.0;
		uint32_t h1 = Pcg (h0), h2 = Pcg (h1), h3 = Pcg (h2), h4 = Pcg (h3);
		double r = 0.12 + 0.12 * Unit16 (h4), hgt = 0.25 + 0.3 * Unit16 (h4 >> 16);
		double ext = 1.05 * r;
		Vector c (ext + (1.0 - 2.0 * ext) * Unit16 (h1), ext + (1.0 - 2.0 * ext) * Unit16 (h2), ext + (1.0 - 2.0 * ext) * Unit16 (h3));
		Vector v (q.x - fx - c.x, q.y - fy - c.y, q.z - fz - c.z);
		if (v.length() >= ext) return 0.0;
		// the outline: a smooth maximum of five edge distances (a polygon
		// with rounded corners: sharp ones alias on the meshes' grid)
		double x = 0.0;
		uint32_t he = h4;
		for (int e = 0; e < 5; e++) {
			he = Pcg (he ^ 0x51ed27u);
			x += std::exp (dotp (v, UnitFromHash (he)) / (r * (0.75 + 0.25 * Unit16 (he >> 16))) / PEBBLE_SOFT);
		}
		x = PEBBLE_SOFT * std::log (x);
		if (x >= 1.0) return 0.0;
		// a bevelled top (sides over the outer 60%, rounded at both ends), tilted
		double top = SmoothStep (0.0, 0.6, 1.0 - x);
		double tilt = 1.0 + 0.5 * dotp (v, UnitFromHash (Pcg (h3 ^ 0x2545f491u))) / r;
		return hgt * r * s * top * std::max (tilt, 0.3);
	}

	double CraterLayer (const Vector &d, int k, int layer, double R, double density, double depth)
	{
		int m = k - JMIN + 32 + 29 * layer;
		double s = std::ldexp (1.0, k);
		Vector q = LatticePoint (m, d, R / s);
		double fx = std::floor (q.x), fy = std::floor (q.y), fz = std::floor (q.z);
		uint32_t h0 = Hash ((int32_t)fx, (int32_t)fy, (int32_t)fz, 1000u + (uint32_t)m);
		if (Unit16 (h0) >= density) return 0.0;
		uint32_t h1 = Pcg (h0), h2 = Pcg (h1), h3 = Pcg (h2), h4 = Pcg (h3);
		CraterShape cs = MakeCrater (h4, depth);
		// (the whole crater within its cell: its outline and rim, anywhere
		// in the cell otherwise; where the surface slices its sphere)
		double ext = cs.rL * (1.0 + cs.ecc + cs.lobe) * (1.0 + cs.wr);
		Vector c (ext + (1.0 - 2.0 * ext) * Unit16 (h1), ext + (1.0 - 2.0 * ext) * Unit16 (h2), ext + (1.0 - 2.0 * ext) * Unit16 (h3));
		Vector v (q.x - fx - c.x, q.y - fy - c.y, q.z - fz - c.z);
		if (v.length() >= ext) return 0.0;
		return CraterHeight (cs, v) * cs.rL * s;
	}

} // namespace

bool TerrainDetail::clientSupport = false;

TerrainDetail::Overrides::Overrides ()
{
	gain = hurst = ridge = floor = sea = craters = craterDepth = craterFresh = pebbles = level = std::nan ("");
}

void TerrainDetail::Overrides::Apply (Params &p) const
{
	if (!std::isnan (gain)) p.gain = std::max (0.0, gain);
	if (!std::isnan (hurst)) p.hurst = std::clamp (hurst, 0.3, 1.5);
	if (!std::isnan (ridge)) p.ridge = std::clamp (ridge, 0.0, 1.0);
	if (!std::isnan (floor)) p.floor = std::max (0.0, floor);
	if (!std::isnan (sea)) p.sea = sea != 0.0;
	if (!std::isnan (craters)) p.craters = std::clamp (craters, 0.0, 4.0);
	if (!std::isnan (craterDepth)) p.craterDepth = std::max (0.0, craterDepth);
	if (!std::isnan (craterFresh)) p.craterFresh = std::clamp (craterFresh, 0.0, 1.0);
	if (!std::isnan (pebbles)) p.pebbles = std::clamp (pebbles, 0.0, 4.0);
	if (!std::isnan (level)) p.level = (int)level;
}

TerrainDetail::Params TerrainDetail::Defaults (const Planet *planet)
{
	Params p;
	const char *name = planet->Name();
	if (!strcmp (name, "Earth")) {
		// eroded relief; plains gently rolling, the sea flat
		p.gain = 0.5, p.ridge = 1.0, p.floor = 0.03, p.sea = true;
	} else if (!strcmp (name, "Mars")) {
		// wind-worn relief, old and shallow craters
		p.gain = 0.4, p.ridge = 0.7, p.floor = 0.04;
		p.craters = 0.8, p.craterDepth = 0.5, p.craterFresh = 0.03;
		p.pebbles = 2.0; // (the rover images: pebbles and float everywhere)
	} else if (!strcmp (name, "Venus")) {
		// volcanic plains; its atmosphere stops small impactors: no small craters
		p.gain = 0.4, p.ridge = 0.6, p.floor = 0.03;
	} else if (!strcmp (name, "Titan")) {
		// plains of sediment and dunes, hardly any craters
		p.gain = 0.3, p.ridge = 0.3, p.floor = 0.02;
		p.craters = 0.02, p.craterDepth = 0.4, p.craterFresh = 0.0;
	} else if (!strcmp (name, "Io")) {
		// lava plains resurfaced faster than impacts crater them: no craters
		p.gain = 0.35, p.ridge = 0.3, p.floor = 0.04;
	} else if (!strcmp (name, "Europa")) {
		// young ice: double ridges, few craters
		p.gain = 0.4, p.ridge = 1.0, p.floor = 0.03;
		p.craters = 0.05, p.craterDepth = 0.5, p.craterFresh = 0.3;
	} else if (!strcmp (name, "Ganymede")) {
		// grooved terrain over old cratered ice
		p.gain = 0.35, p.ridge = 0.8, p.floor = 0.03;
		p.craters = 1.0, p.craterDepth = 0.8, p.craterFresh = 0.3;
	} else if (!strcmp (name, "Enceladus")) {
		// ridged plains, craters on the old terrain only
		p.gain = 0.35, p.ridge = 0.9, p.floor = 0.03;
		p.craters = 0.6, p.craterDepth = 0.8, p.craterFresh = 0.3;
	} else if (!strcmp (name, "Miranda")) {
		// coronae: ridges and grooves among old craters
		p.gain = 0.4, p.ridge = 1.0, p.floor = 0.04;
		p.craters = 1.2, p.craterDepth = 0.9, p.craterFresh = 0.2;
	} else if (!strcmp (name, "Triton")) {
		// young icy plains, "cantaloupe" terrain, few craters
		p.gain = 0.35, p.ridge = 0.7, p.floor = 0.03;
		p.craters = 0.15, p.craterDepth = 0.6, p.craterFresh = 0.3;
	} else if (!planet->HasAtmosphere()) {
		// airless: rounded by impacts, craters of all sizes (in equilibrium:
		// overlapping, mostly degraded)
		// (the roughness at metre scales from the craters, as on the Moon,
		// not continuous noise, which looks like ripples there: the noise
		// falls off fast at small scales, H 1.2; the measured RMS slopes of
		// the lunar maria, about 3 degrees at 25 m and 6 at 1 m, LOLA,
		// Rosenburg 2011, met with the craters)
		p.gain = 0.3, p.ridge = 0.3, p.floor = 0.05, p.hurst = 1.2;
		p.craters = 4.0, p.craterDepth = 1.0, p.craterFresh = 0.25;
		p.pebbles = 1.2; // (clods and fragments in the regolith)
	} else {
		p.gain = 0.35, p.ridge = 1.0, p.floor = 0.02;
	}
	return p;
}

TerrainDetail::TerrainDetail (const Planet *pl, const Params &p): planet (pl), prm (p)
{
	radius = pl->Size();
	// finest mesh: tiles of level L span pi/2^(L-4) in latitude, with 32
	// quads per edge; by default nodes about 1.2 m apart, at least the
	// planet's own resolution limit
	if (prm.level <= 0)
		prm.level = (int)std::lround (4.0 + std::log2 (radius * Pi / (32.0 * 0.02)));
	prm.level = std::clamp (prm.level, (int)pl->MaxPatchLevel(), 28);
	double s = radius * Pi / std::ldexp (1.0, prm.level - 4) / 32.0;
	// (planets without elevation data: the spacing of the coarsest tile
	// level with nodes 2 km apart at most)
	flatSpacing = radius * Pi / 256.0;
	while (flatSpacing > 2000.0) flatSpacing *= 0.5;
	lambdaMin = 4.0 * s;
}

void TerrainDetail::InitBases () const
{
	basesDone = true;
	for (DWORD i = 0; i < planet->nBase(); i++) {
		const Base *b = planet->GetBase (i);
		double lng, lat;
		b->EquPos (lng, lat);
		double r = b->FootprintRadius () + 300.0;
		flats.push_back ({ Vector (cos (lat) * cos (lng), sin (lat), cos (lat) * sin (lng)), r, r * 1.5 + 1000.0 });
	}
}

double TerrainDetail::Fade (const Vector &dir) const
{
	if (!basesDone) InitBases ();
	double f = 1.0;
	for (const Flat &fl : flats) {
		double d = (dir - fl.dir).length() * radius;
		if (d < fl.r1) f = std::min (f, SmoothStep (fl.r0, fl.r1, d));
	}
	return f;
}

double TerrainDetail::Height (double lat, double lng, double elev, double spacing, double relief, double lmin,
	double *amplitude, double *fade) const
{
	if (amplitude) *amplitude = 0.0;
	if (fade) *fade = 0.0;
	if (spacing <= 0.0) return 0.0;
	Vector dir (cos (lat) * cos (lng), sin (lat), cos (lat) * sin (lng));
	double f = Fade (dir);
	if (f <= 0.0) return 0.0;
	if (fade) *fade = f;
	// plains get the floor's relief (not the sea)
	if (prm.floor > 0.0 && (!prm.sea || elev > 1.0)) relief = std::max (relief, prm.floor * spacing);
	double A = prm.gain * std::max (relief, 0.0) * f; // amplitude at the data's node spacing
	if (amplitude) *amplitude = A;
	lmin = std::max (lmin, lambdaMin);
	if (lmin >= spacing) return 0.0;

	// Fractal noise continuing the spectrum of the data below its node
	// spacing: amplitude relief*(lambda/spacing)^H, phased in over an octave
	// below the spacing and out over an octave above lmin
	double h = 0.0;
	if (A > 0.0) {
		int j1 = std::min (JMAX, (int)std::floor (std::log2 (spacing)));
		for (int j = j1; j >= JMIN; j--) {
			double lambda = std::ldexp (1.0, j);
			if (lambda < 0.5 * lmin) break;
			double u = std::log2 (spacing / lambda); // octaves below the data
			double w = SmoothStep (0.5, 1.5, u) * SmoothStep (0.5 * lmin, lmin, lambda);
			if (w <= 0.0) continue;
			double a = A * std::pow (lambda / spacing, prm.hurst);
			Vector p = LatticePoint (j - JMIN, dir, radius / lambda);
			h += a * w * Shape (Noise (p.x, p.y, p.z, (uint32_t)(j - JMIN)), j, prm.ridge);
		}
	}
	// craters below the data's resolution (their typical diameter, a third
	// of the cell size), in the meshes when they are fine enough for the
	// rims (CRATER_G)
	if (prm.craters > 0.0) {
		int k1 = std::min (KMAX, (int)std::floor (std::log2 (spacing / CRATER_D)));
		for (int k = k1; k >= KMIN; k--) {
			double s = std::ldexp (1.0, k), lambda = CRATER_D * s, lg = CRATER_G * s;
			if (lg < 0.5 * lmin) break;
			double w = SmoothStep (0.5, 1.5, std::log2 (spacing / lambda)) * SmoothStep (0.5 * lmin, lmin, lg);
			if (w <= 0.0) continue;
			// (densities above 1: several lattices)
			int nl = (int)std::ceil (prm.craters);
			for (int l = 0; l < nl; l++)
				h += w * f * CraterLayer (dir, k, l, radius, prm.craters / nl, prm.craterDepth);
		}
	}
	// pebbles and clods (only where the mesh resolves them: PEBBLE_G)
	if (prm.pebbles > 0.0) {
		int nl = (int)std::ceil (prm.pebbles);
		for (int k = -1; k >= -4; k--) {
			double s = std::ldexp (1.0, k), lg = PEBBLE_G * s;
			if (lg < 0.5 * lmin) break;
			double w = SmoothStep (0.5 * lmin, lmin, lg) * SmoothStep (0.5, 1.5, std::log2 (spacing / s));
			if (w <= 0.0) continue;
			for (int l = 0; l < nl; l++)
				h += w * f * PebbleLayer (dir, k, l, radius, prm.pebbles / nl);
		}
	}
	return h;
}

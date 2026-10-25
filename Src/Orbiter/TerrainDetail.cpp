// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "TerrainDetail.h"
#include "Planet.h"
#include "Base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

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

	// the octaves' shape: gradient noise, with a ridged part (sharp crests,
	// rounded hollows, as in eroded rock) in octaves of 8 m and more
	inline double Ridge (int j) { return std::clamp ((j - 3) / 3.0, 0.0, 1.0); }
	inline double Shape (double n, int j)
	{
		double r = Ridge (j);
		return (1.0 - 0.4 * r) * n + 0.8 * r * (0.35 - std::fabs (n));
	}

	inline double SmoothStep (double a, double b, double x)
	{
		double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	// octaves: wavelengths 2^j m, each with its own orientation (one of the
	// 24 rotations of the cube: exact) and offset, so that the lattices
	// don't line up
	constexpr int JMIN = -4, JMAX = 20;
	Vector OctavePoint (int j, const Vector &d, double scale)
	{
		static const int P[6][3] = { {0,1,2}, {1,2,0}, {2,0,1}, {0,2,1}, {1,0,2}, {2,1,0} };
		static const double S[4][3] = { {1,1,1}, {1,-1,-1}, {-1,1,-1}, {-1,-1,1} };
		int k = (j - JMIN) % 24;
		const int *p = P[k % 6];
		double sg = (k % 6) < 3 ? 1.0 : -1.0; // (odd permutations: all signs flipped)
		const double *s = S[k / 6];
		int m = j - JMIN;
		return Vector (sg * s[0] * d.data[p[0]] * scale + ((m * 37) & 63) / 64.0,
		               sg * s[1] * d.data[p[1]] * scale + ((m * 23) & 63) / 64.0,
		               sg * s[2] * d.data[p[2]] * scale + ((m * 53) & 63) / 64.0);
	}

} // namespace

bool TerrainDetail::clientSupport = false;

TerrainDetail::TerrainDetail (const Planet *p, double g): planet (p), gain (g)
{
	radius = p->Size();
	// node spacing of the finest mesh: tiles of level L span pi/2^(L-4) in
	// latitude, with 32 quads per edge
	double s = radius * Pi / std::ldexp (1.0, (int)p->MaxPatchLevel() - 4) / 32.0;
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

double TerrainDetail::Height (double lat, double lng, double spacing, double relief, double lmin, double *amplitude) const
{
	if (amplitude) *amplitude = 0.0;
	if (gain <= 0.0 || relief <= 0.0 || spacing <= 0.0) return 0.0;
	Vector dir (cos (lat) * cos (lng), sin (lat), cos (lat) * sin (lng));
	double f = Fade (dir);
	if (f <= 0.0) return 0.0;
	double A = gain * relief * f; // amplitude at the data's node spacing
	if (amplitude) *amplitude = A;
	lmin = std::max (lmin, lambdaMin);
	if (lmin >= spacing) return 0.0;

	// Fractal noise continuing the spectrum of the data below its node
	// spacing: amplitude relief*(lambda/spacing)^H, phased in over an octave
	// below the spacing and out over an octave above lmin
	double h = 0.0;
	int j1 = std::min (JMAX, (int)std::floor (std::log2 (spacing)));
	for (int j = j1; j >= JMIN; j--) {
		double lambda = std::ldexp (1.0, j);
		if (lambda < 0.5 * lmin) break;
		double u = std::log2 (spacing / lambda); // octaves below the data
		double w = SmoothStep (0.5, 1.5, u) * SmoothStep (0.5 * lmin, lmin, lambda);
		if (w <= 0.0) continue;
		double a = A * std::pow (lambda / spacing, hurst);
		Vector p = OctavePoint (j, dir, radius / lambda);
		h += a * w * Shape (Noise (p.x, p.y, p.z, (uint32_t)(j - JMIN)), j);
	}
	return h;
}

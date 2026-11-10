// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ProcTerrain.h"
#include "Config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

	const double PI_ = 3.14159265358979323846;

	inline uint32_t Pcg (uint32_t v)
	{
		uint32_t state = v * 747796405u + 2891336453u;
		uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}

	// the graphics client's value noise (OVP/VulkanClient/ProcSurface.cpp:
	// Rand, Smooth, VNoise, Fbm), ported 1:1 for GroundAt; both must change together
	inline double CRand (int32_t x, int32_t y, int32_t z, uint32_t seed)
	{
		return (Pcg ((uint32_t)x + Pcg ((uint32_t)y + Pcg ((uint32_t)z + Pcg (seed)))) >> 8) * (2.0 / 16777216.0) - 1.0;
	}
	inline double CSmooth (double t) { return t * t * (3.0 - 2.0 * t); }

	double CVNoise (double x, double y, double z, uint32_t seed)
	{
		double fx = std::floor (x), fy = std::floor (y), fz = std::floor (z);
		int32_t ix = (int32_t)fx, iy = (int32_t)fy, iz = (int32_t)fz;
		double u = CSmooth (x - fx), v = CSmooth (y - fy), w = CSmooth (z - fz);
		auto L = [](double a, double b, double t) { return a + (b - a) * t; };
		return L (L (L (CRand (ix, iy, iz, seed), CRand (ix+1, iy, iz, seed), u), L (CRand (ix, iy+1, iz, seed), CRand (ix+1, iy+1, iz, seed), u), v),
			L (L (CRand (ix, iy, iz+1, seed), CRand (ix+1, iy, iz+1, seed), u), L (CRand (ix, iy+1, iz+1, seed), CRand (ix+1, iy+1, iz+1, seed), u), v), w);
	}

	double CFbm (const double p[3], double f0, double fmax, uint32_t seed, double gain = 0.5)
	{
		double sum = 0.0, amp = 1.0, norm = 0.0;
		double q[3] = { p[0], p[1], p[2] };
		for (int k = 0; k < 20 && f0 <= fmax; k++, f0 *= 2.03, amp *= gain) {
			double w = std::min (1.0, 2.0 * (fmax / f0 - 1.0));
			sum += w * amp * CVNoise (q[0] * f0 + 17.0 * k, q[1] * f0 - 5.0 * k, q[2] * f0 + 3.0 * k, seed + 101u * k);
			norm += amp;
			const double c = 0.45, sn = 0.89, t = (1.0 - c) / 3.0, r = sn / std::sqrt (3.0);
			double x = q[0], y = q[1], z = q[2];
			q[0] = (c + t) * x + (t - r) * y + (t + r) * z;
			q[1] = (t + r) * x + (c + t) * y + (t - r) * z;
			q[2] = (t - r) * x + (t + r) * y + (c + t) * z;
		}
		return norm > 0.0 ? sum / norm * 1.6 : 0.0;
	}

	inline uint32_t Hash (int32_t x, int32_t y, int32_t z, uint32_t seed)
	{
		return Pcg ((uint32_t)x + Pcg ((uint32_t)y + Pcg ((uint32_t)z + Pcg (seed))));
	}

	inline double Unit (uint32_t h) { return (h >> 8) * (1.0 / 16777216.0); } // [0, 1)

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
	inline double Smoothstep (double a, double b, double x)
	{
		double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	// gradient noise, about -1 to 1
	double Noise (double x, double y, double z, uint32_t seed)
	{
		double fx = std::floor (x), fy = std::floor (y), fz = std::floor (z);
		int32_t ix = (int32_t)fx, iy = (int32_t)fy, iz = (int32_t)fz;
		x -= fx, y -= fy, z -= fz;
		double u = Smoother (x), v = Smoother (y), w = Smoother (z);
		double n000 = Grad (Hash (ix,   iy,   iz,   seed), x,       y,       z);
		double n100 = Grad (Hash (ix+1, iy,   iz,   seed), x - 1.0, y,       z);
		double n010 = Grad (Hash (ix,   iy+1, iz,   seed), x,       y - 1.0, z);
		double n110 = Grad (Hash (ix+1, iy+1, iz,   seed), x - 1.0, y - 1.0, z);
		double n001 = Grad (Hash (ix,   iy,   iz+1, seed), x,       y,       z - 1.0);
		double n101 = Grad (Hash (ix+1, iy,   iz+1, seed), x - 1.0, y,       z - 1.0);
		double n011 = Grad (Hash (ix,   iy+1, iz+1, seed), x,       y - 1.0, z - 1.0);
		double n111 = Grad (Hash (ix+1, iy+1, iz+1, seed), x - 1.0, y - 1.0, z - 1.0);
		return Lerp (Lerp (Lerp (n000, n100, u), Lerp (n010, n110, u), v),
			Lerp (Lerp (n001, n101, u), Lerp (n011, n111, u), v), w);
	}

	// octaves while their wavelength R/f [m] is at least lmin, the last
	// faded in over its first half (continuous in lmin)
	inline double OctaveWeight (double R, double f, double lmin)
	{
		double l = R / f;
		return std::clamp (2.0 * (l / lmin - 1.0), 0.0, 1.0);
	}

	const int MAX_OCTAVES = 16;
	const double CONTINENT_F0 = 1.2;   // [1/R]
	const double CONTINENT_LMAX = 256; // (finest continent wavelength: R/256)
	const double RIDGE_F0 = 2.5;
	const double CRATER_CELL0 = 0.35;  // largest crater cell (unit sphere)
	const int CRATER_SCALES = 12;
}

const char *ProcTerrain::ClassName (int cls)
{
	static const char *name[] = { "Rock", "Ocean", "Ice", "Desert", "Lava", "Gas" };
	return cls >= 0 && cls <= GAS ? name[cls] : "Rock";
}

bool ProcTerrain::Read (std::istream &cfg, Params &prm)
{
	char cbuf[256];
	if (!GetItemString (cfg, "SurfaceClass", cbuf)) return false;
	prm = Params ();
	for (int c = ROCK; c <= GAS; c++)
		if (!_strnicmp (trim_string (cbuf), ClassName (c), strlen (ClassName (c)))) prm.cls = c;
	int i;
	if (GetItemInt (cfg, "SurfaceSeed", i)) prm.seed = (uint32_t)i;
	GetItemReal (cfg, "SurfaceRelief", prm.relief);
	GetItemReal (cfg, "SurfaceMountains", prm.mountains);
	GetItemReal (cfg, "SurfaceSea", prm.sea);
	GetItemReal (cfg, "SurfaceCraters", prm.craters);
	GetItemReal (cfg, "SurfaceOpenWater", prm.openWater);
	if (GetItemString (cfg, "SurfaceColours", cbuf))
		sscanf (cbuf, "%lf%lf%lf%lf%lf%lf", prm.colLow, prm.colLow + 1, prm.colLow + 2, prm.colHigh, prm.colHigh + 1, prm.colHigh + 2);
	if (GetItemInt (cfg, "SurfaceMaxLevel", i)) prm.maxLevel = std::clamp (i, 5, 16);
	prm.sea = std::clamp (prm.sea, 0.0, 0.98);
	prm.craters = std::clamp (prm.craters, 0.0, 1.0);
	return true;
}

ProcTerrain::ProcTerrain (const Params &p, double radius)
: prm(p), R(radius)
{
	// octave rotations (random axes and angles, from the seed) and offsets:
	// no lattice directions shared by the octaves
	for (int k = 0; k < MAX_OCTAVES; k++) {
		uint32_t h = Pcg (prm.seed * 2654435761u + 977u * (uint32_t)k);
		double z = 2.0 * Unit (h) - 1.0, a = 2.0 * PI_ * Unit (Pcg (h ^ 0x68e31da4u));
		double s = std::sqrt (std::max (0.0, 1.0 - z * z));
		double ax[3] = { s * std::cos (a), s * std::sin (a), z };
		double t = 2.0 * PI_ * Unit (Pcg (h ^ 0xb5297a4du)), c = std::cos (t), si = std::sin (t), C = 1.0 - c;
		double *m = rot[k];
		m[0] = c + ax[0]*ax[0]*C;        m[1] = ax[0]*ax[1]*C - ax[2]*si; m[2] = ax[0]*ax[2]*C + ax[1]*si;
		m[3] = ax[1]*ax[0]*C + ax[2]*si; m[4] = c + ax[1]*ax[1]*C;        m[5] = ax[1]*ax[2]*C - ax[0]*si;
		m[6] = ax[2]*ax[0]*C - ax[1]*si; m[7] = ax[2]*ax[1]*C + ax[0]*si; m[8] = c + ax[2]*ax[2]*C;
		for (int j = 0; j < 3; j++) ofs[k][j] = 100.0 * Unit (Pcg (h + 31u * (uint32_t)(j + 1)));
	}
	// the datum: the continents' quantile SurfaceSea, over points spread
	// evenly on the sphere (the same in the core and the clients)
	const int N = 4096;
	std::vector<double> c (N);
	double sum = 0.0, sum2 = 0.0;
	for (int i = 0; i < N; i++) {
		double y = 1.0 - (i + 0.5) * 2.0 / N, r = std::sqrt (1.0 - y * y), phi = i * 2.399963229728653;
		double p[3] = { r * std::cos (phi), y, r * std::sin (phi) };
		c[i] = Continent (p, 0.0);
		sum += c[i], sum2 += c[i] * c[i];
	}
	cs = std::sqrt (std::max (1e-12, sum2 / N - (sum / N) * (sum / N)));
	std::sort (c.begin (), c.end ());
	c0 = prm.sea > 0.0 ? c[std::min (N - 1, (int)(prm.sea * N))] : sum / N;
}

double ProcTerrain::Continent (const double p[3], double lmin) const
{
	// fractal noise of large wavelengths, its domain warped (coasts and
	// basins not round)
	double q[3] = { p[0], p[1], p[2] };
	for (int j = 0; j < 3; j++)
		q[j] += 0.25 * Noise (1.7 * p[0] + 11.0 * j, 1.7 * p[1] + 3.0, 1.7 * p[2] - 7.0 * j, prm.seed ^ 0x3c6ef372u);
	lmin = std::max (lmin, R / CONTINENT_LMAX);
	double sum = 0.0, amp = 1.0, f = CONTINENT_F0;
	for (int k = 0; k < MAX_OCTAVES; k++, f *= 2.0, amp *= 0.55) {
		double w = OctaveWeight (R, f, lmin);
		if (w <= 0.0) break;
		const double *m = rot[k];
		double x = (m[0]*q[0] + m[1]*q[1] + m[2]*q[2]) * f + ofs[k][0];
		double y = (m[3]*q[0] + m[4]*q[1] + m[5]*q[2]) * f + ofs[k][1];
		double z = (m[6]*q[0] + m[7]*q[1] + m[8]*q[2]) * f + ofs[k][2];
		sum += w * amp * Noise (x, y, z, prm.seed);
	}
	return sum;
}

double ProcTerrain::Ridges (const double p[3], double lmin) const
{
	// ridged multifractal: sharp crests, the finer octaves strongest on the
	// crests of the coarser
	double sum = 0.0, amp = 1.0, f = RIDGE_F0, wgt = 1.0;
	for (int k = 0; k < MAX_OCTAVES; k++, f *= 2.0, amp *= 0.5) {
		double w = OctaveWeight (R, f, lmin);
		if (w <= 0.0) break;
		const double *m = rot[MAX_OCTAVES - 1 - k];
		double x = (m[0]*p[0] + m[1]*p[1] + m[2]*p[2]) * f + ofs[k][2];
		double y = (m[3]*p[0] + m[4]*p[1] + m[5]*p[2]) * f + ofs[k][0];
		double z = (m[6]*p[0] + m[7]*p[1] + m[8]*p[2]) * f + ofs[k][1];
		double n = 1.0 - std::fabs (Noise (x, y, z, prm.seed ^ 0xa54ff53au));
		n = n * n * wgt;
		wgt = std::clamp (2.0 * n, 0.0, 1.0);
		sum += w * amp * n;
	}
	return sum - 0.3;
}

double ProcTerrain::Craters (const double p[3], double lmin) const
{
	// cells of a lattice in space at each scale (halving): a crater in a
	// cell by chance, its centre in the cell's middle half, so that each
	// point need only look at its own cell and the nearer neighbour on each
	// axis; the sphere cuts the crater's ball (a smaller crater where it
	// cuts it off-centre). At each scale four times as many: the same share
	// of the ground by size (equilibrium)
	double sum = 0.0, cell = CRATER_CELL0;
	for (int j = 0; j < CRATER_SCALES; j++, cell *= 0.5) {
		double Dmax = 0.6 * cell * R;
		if (Dmax < 2.0 * lmin) break;
		double g[3] = { p[0] / cell, p[1] / cell, p[2] / cell };
		int32_t b[3], o[3];
		for (int a = 0; a < 3; a++) {
			double fl = std::floor (g[a]);
			b[a] = (int32_t)fl;
			o[a] = g[a] - fl < 0.5 ? -1 : 1;
		}
		uint32_t seed = prm.seed + 0x9e3779b9u * (uint32_t)(j + 1);
		for (int n = 0; n < 8; n++) {
			int32_t c[3] = { b[0] + ((n & 1) ? o[0] : 0), b[1] + ((n & 2) ? o[1] : 0), b[2] + ((n & 4) ? o[2] : 0) };
			uint32_t h = Hash (c[0], c[1], c[2], seed);
			if (Unit (h) >= prm.craters) continue;
			uint32_t h1 = Pcg (h), h2 = Pcg (h1), h3 = Pcg (h2), h4 = Pcg (h3), h5 = Pcg (h4);
			double cx = (c[0] + 0.25 + 0.5 * Unit (h1)) * cell - p[0];
			double cy = (c[1] + 0.25 + 0.5 * Unit (h2)) * cell - p[1];
			double cz = (c[2] + 0.25 + 0.5 * Unit (h3)) * cell - p[2];
			double r = cell * (0.12 + 0.18 * Unit (h4));
			double x = std::sqrt (cx * cx + cy * cy + cz * cz) / r;
			if (x >= 1.6) continue;
			double D = 2.0 * r * R;
			// (depth a fifth of the diameter, less for the large; degraded
			// by age; rims a fifth of the depth; large ones flat-floored)
			double depth = D < 15e3 ? 0.2 * D : 3000.0 * std::pow (D / 15e3, 0.25);
			depth *= 0.3 + 0.7 * Unit (h5);
			double rim = 0.2 * depth;
			double fade = std::clamp (D / (2.0 * lmin) - 1.0, 0.0, 1.0);
			double h0;
			if (x < 1.0) {
				double fl = std::clamp ((D - 15e3) / 100e3, 0.0, 0.5);
				double xx = std::max (x - fl, 0.0) / (1.0 - fl);
				h0 = depth * (xx * xx - 1.0) + rim;
			} else {
				double t = 1.0 - (x - 1.0) / 0.6;
				h0 = rim * t * t * t;
			}
			sum += fade * h0;
		}
	}
	return sum;
}

double ProcTerrain::Height (double x, double y, double z, double lmin, bool seabed) const
{
	if (prm.cls == GAS) return 0.0;
	double p[3] = { x, y, z };
	double c = (Continent (p, lmin) - c0) / cs;
	// (basins deeper below the datum where there are seas)
	double e = prm.relief * c;
	if (prm.sea > 0.0) e *= c < 0.0 ? 1.5 : 0.7;
	// ranges: on land where there are seas, else in patches
	double mask = prm.sea > 0.0 ? Smoothstep (-0.1, 0.8, c)
		: Smoothstep (-0.3, 0.6, Noise (1.6 * x + 17.0, 1.6 * y, 1.6 * z - 5.0, prm.seed ^ 0x510e527fu));
	if (prm.mountains > 0.0 && mask > 0.0) e += prm.mountains * mask * Ridges (p, lmin);
	if (prm.craters > 0.0) e += Craters (p, lmin);
	// (an ocean world's seas: their surface the ground, as Earth's)
	if (prm.cls == OCEAN && !seabed) e = std::max (e, 0.0);
	return e;
}

double ProcTerrain::LambdaOf (int lvl) const
{
	// (two node spacings of the tiles of level lvl: 256 intervals across
	// 180 / 2^(lvl-4) degrees of latitude)
	return 2.0 * PI_ * R / (std::ldexp (1.0, lvl - 4) * 256.0);
}

void ProcTerrain::Tile (int lvl, int ilat, int ilng, int trunc, double res, int16_t *elev, bool seabed) const
{
	const int G = 256, S = G + 3;
	int nlat = 1 << (lvl - 4), nlng = 2 << (lvl - 4);
	double latmax = PI_ * (0.5 - (double)ilat / nlat), dlat = PI_ / nlat / G, latmin = latmax - dlat * G;
	double lngmin = -PI_ + 2.0 * PI_ * ilng / nlng, dlng = 2.0 * PI_ / nlng / G;
	double lmin = LambdaOf (trunc > 0 && trunc < lvl ? trunc : lvl);
	std::vector<double> cl (S), sl (S);
	for (int c = 0; c < S; c++) {
		double lng = lngmin + (c - 1) * dlng;
		cl[c] = std::cos (lng), sl[c] = std::sin (lng);
	}
	for (int r = 0; r < S; r++) {
		double lat = latmin + (r - 1) * dlat, cb = std::cos (lat), sb = std::sin (lat);
		for (int c = 0; c < S; c++) {
			double h = Height (cb * cl[c], sb, cb * sl[c], lmin, seabed) / res;
			elev[r * S + c] = (int16_t)std::clamp (std::lround (h), -32767l, 32767l);
		}
	}
}

double ProcTerrain::TexelAngle () const
{
	int lvlTex = std::min (prm.maxLevel + 2, 16);
	return PI_ / std::ldexp (1.0, lvlTex - 4) / 256.0;
}

ProcTerrain::Ground ProcTerrain::GroundAt (double x, double y, double z, bool tidalLock) const
{
	Ground g;
	// (the client's finest colour tile level)
	const int lvlTex = std::min (prm.maxLevel + 2, 16);
	const double dlat = PI_ / std::ldexp (1.0, lvlTex - 4);
	const double tx = dlat / 256.0, fmax = 256.0 / (2.0 * dlat);
	const uint32_t seed = prm.seed * 2246822519u + 7u;
	const double p[3] = { x, y, z };
	g.n1 = CVNoise (p[0] * 3.1, p[1] * 3.1, p[2] * 3.1, seed) * 0.7 + CVNoise (p[0] * 7.3, p[1] * 7.3, p[2] * 7.3, seed + 1) * 0.3;
	g.nf = CFbm (p, 24.0, fmax, seed + 7);
	g.theta = tidalLock ? std::acos (std::clamp (x, -1.0, 1.0)) : std::fabs (std::asin (std::clamp (y, -1.0, 1.0)));
	const double openW = prm.openWater * (PI_ / 180.0);
	g.edge = g.theta - (openW + 0.06 * g.n1 + 0.015 * g.nf);
	if (prm.cls == OCEAN) {
		g.seabed = Height (x, y, z, LambdaOf (prm.maxLevel), true);
		g.sea = 1.0 - Smoothstep (-1.0, 1.0, g.seabed);
		g.coast = 1.0 - Smoothstep (0.0, 150.0, g.seabed);
		g.snow = std::max (Smoothstep (0.05, 0.2, g.edge + 0.04 * g.nf),
			Smoothstep (3500.0, 5500.0, g.seabed + 600.0 * g.nf - 1500.0 * std::cos (g.theta)));
	} else if (prm.cls == ICE && openW > 0.0) {
		g.sea = 1.0 - Smoothstep (-tx, tx, g.edge);
	}
	return g;
}

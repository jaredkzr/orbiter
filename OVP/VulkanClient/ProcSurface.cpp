// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ProcSurface.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

	const double PI_ = 3.14159265358979323846;
	const int N = 256;          // texels across a tile
	const int NG = 259;          // elevation nodes across a tile (with its border)
	const size_t CACHE = 24;    // elevation tiles kept
	enum { ROCK, OCEAN, ICE, DESERT, LAVA, GAS };

	inline uint32_t Pcg (uint32_t v)
	{
		uint32_t state = v * 747796405u + 2891336453u;
		uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}
	inline double Rand (int32_t x, int32_t y, int32_t z, uint32_t seed)
	{
		return (Pcg ((uint32_t)x + Pcg ((uint32_t)y + Pcg ((uint32_t)z + Pcg (seed)))) >> 8) * (2.0 / 16777216.0) - 1.0;
	}
	inline double Smooth (double t) { return t * t * (3.0 - 2.0 * t); }
	inline double Smoothstep (double a, double b, double x) { return Smooth (std::clamp ((x - a) / (b - a), 0.0, 1.0)); }

	// value noise, -1 to 1
	double VNoise (double x, double y, double z, uint32_t seed)
	{
		double fx = std::floor (x), fy = std::floor (y), fz = std::floor (z);
		int32_t ix = (int32_t)fx, iy = (int32_t)fy, iz = (int32_t)fz;
		double u = Smooth (x - fx), v = Smooth (y - fy), w = Smooth (z - fz);
		auto L = [](double a, double b, double t) { return a + (b - a) * t; };
		return L (L (L (Rand (ix, iy, iz, seed), Rand (ix+1, iy, iz, seed), u), L (Rand (ix, iy+1, iz, seed), Rand (ix+1, iy+1, iz, seed), u), v),
			L (L (Rand (ix, iy, iz+1, seed), Rand (ix+1, iy, iz+1, seed), u), L (Rand (ix, iy+1, iz+1, seed), Rand (ix+1, iy+1, iz+1, seed), u), v), w);
	}

	// fractal value noise from frequency f0 [1/radius] up to fmax
	double Fbm (const double p[3], double f0, double fmax, uint32_t seed, double gain = 0.5)
	{
		double sum = 0.0, amp = 1.0, norm = 0.0;
		for (int k = 0; k < 20 && f0 <= fmax; k++, f0 *= 2.03, amp *= gain) {
			double w = std::min (1.0, 2.0 * (fmax / f0 - 1.0)); // (the last octave faded in)
			sum += w * amp * VNoise (p[0] * f0 + 17.0 * k, p[1] * f0 - 5.0 * k, p[2] * f0 + 3.0 * k, seed + 101u * k);
			norm += amp;
		}
		return norm > 0.0 ? sum / norm * 1.6 : 0.0;
	}

	inline uint8_t Srgb (double v)
	{
		v = std::clamp (v, 0.0, 1.0);
		v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow (v, 1.0 / 2.4) - 0.055;
		return (uint8_t)std::lround (v * 255.0);
	}

	void DdsHeader (std::vector<uint8_t> &dds, bool alpha)
	{
		dds.assign (128 + (size_t)N * N * 4, 0);
		uint8_t *hd = dds.data();
		auto put = [&](size_t at, uint32_t v) { memcpy (hd + at, &v, 4); };
		memcpy (hd, "DDS ", 4);
		put (4, 124);
		put (8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x8);
		put (12, N), put (16, N), put (20, N * 4);
		put (76, 32);
		put (80, 0x40 | (alpha ? 0x1 : 0));
		put (88, 32);
		put (92, 0x000000FF), put (96, 0x0000FF00), put (100, 0x00FF0000), put (104, alpha ? 0xFF000000 : 0);
		put (108, 0x1000);
	}

	struct Rgb { double r, g, b; };
	inline Rgb Mix (const Rgb &a, const Rgb &b, double t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
	inline Rgb Scale (const Rgb &a, double s) { return { a.r * s, a.g * s, a.b * s }; }
}

std::shared_ptr<ProcPlanet> ProcPlanet::Create (OBJHANDLE hPlanet)
{
	PROCSURFACEPARAM prm;
	if (!hPlanet || !oapiProcSurfaceParams (hPlanet, &prm)) return nullptr;
	std::shared_ptr<ProcPlanet> pp (new ProcPlanet);
	pp->hPlanet = hPlanet;
	pp->prm = prm;
	pp->radius = oapiGetSize (hPlanet);
	if (const double *r = (const double*)oapiGetObjectParam (hPlanet, OBJPRM_PLANET_ELEVRESOLUTION))
		if (*r > 0.0) pp->res = *r;
	// (colour tiles two levels beyond the elevation: about 70 m texels on
	// an Earth-sized world; finer, the terrain detail's shading)
	pp->texLevel = std::min (prm.maxLevel + 2, 16);
	return pp;
}

std::shared_ptr<const std::vector<float>> ProcPlanet::Elev (int lvl, int ilat, int ilng)
{
	{
		std::lock_guard<std::mutex> lock (mtx);
		for (auto it = cache.begin (); it != cache.end (); ++it)
			if (it->lvl == lvl && it->ilat == ilat && it->ilng == ilng) {
				cache.splice (cache.begin (), cache, it);
				return it->h;
			}
	}
	std::vector<int16_t> e ((size_t)NG * NG);
	// (the sea floor: the water's colour by its depth)
	oapiProcElevationTile (hPlanet, lvl, ilat, ilng, std::min (lvl, prm.maxLevel), res, e.data (), true);
	auto h = std::make_shared<std::vector<float>> ((size_t)NG * NG);
	for (size_t i = 0; i < e.size (); i++) (*h)[i] = (float)(e[i] * res);
	std::lock_guard<std::mutex> lock (mtx);
	cache.push_front (Cached { lvl, ilat, ilng, h });
	if (cache.size () > CACHE) cache.pop_back ();
	return h;
}

void ProcPlanet::ElevTile (int lvl, int ilat, int ilng, std::vector<uint8_t> &elv)
{
	// (the elevation file's layout, as the core's tiles: the core's own
	// values, in its unit)
	std::vector<int16_t> e ((size_t)NG * NG);
	oapiProcElevationTile (hPlanet, lvl, ilat, ilng, 0, res, e.data ());
	const int32_t HDR = 100;
	elv.assign (HDR + e.size () * 2, 0);
	uint8_t *d = elv.data ();
	int32_t dtype = -16, n = NG;
	double scale = res, offset = 0.0;
	memcpy (d, "ELE\x01", 4);
	memcpy (d + 4, &HDR, 4);
	memcpy (d + 8, &dtype, 4);
	memcpy (d + 12, &n, 4);
	memcpy (d + 16, &n, 4);
	memcpy (d + 28, &scale, 8);
	memcpy (d + 36, &offset, 8);
	memcpy (d + HDR, e.data (), e.size () * 2);
}

void ProcPlanet::Texels (int lvl, int ilat, int ilng, std::vector<uint8_t> &out, bool mask)
{
	auto hp = Elev (lvl, ilat, ilng);
	const std::vector<float> &h = *hp;
	const int nlat = 1 << (lvl - 4), nlng = 2 << (lvl - 4);
	const double latmax = PI_ * (0.5 - (double)ilat / nlat), dlat = PI_ / nlat, latmin = latmax - dlat;
	const double lngmin = -PI_ + 2.0 * PI_ * ilng / nlng, dlng = 2.0 * PI_ / nlng;
	const double de = dlat / (NG - 3), dle = dlng / (NG - 3); // node spacing [rad]
	const double R = radius;
	// (the colour noise: octaves down to two texels; frequencies per radius)
	const double fmax = N / (2.0 * dlat);
	const uint32_t seed = prm.seed * 2246822519u + 7u;
	const double relief = std::max (300.0, prm.relief + prm.mountains);
	const Rgb low { prm.colLow[0], prm.colLow[1], prm.colLow[2] }, high { prm.colHigh[0], prm.colHigh[1], prm.colHigh[2] };
	const double openW = prm.openWater;
	const int cls = prm.surfClass;
	uint8_t *o = out.data () + 128;
	for (int y = 0; y < N; y++) {
		double lat = latmax - (y + 0.5) / N * dlat, cb = std::cos (lat), sb = std::sin (lat);
		double gr = (lat - latmin) / de + 1.0;
		int r0 = std::clamp ((int)std::floor (gr), 0, NG - 2);
		double tr = gr - r0;
		for (int x = 0; x < N; x++, o += 4) {
			double lng = lngmin + (x + 0.5) / N * dlng;
			double gc = (lng - lngmin) / dle + 1.0;
			int c0 = std::clamp ((int)std::floor (gc), 0, NG - 2);
			double tc = gc - c0;
			auto H = [&](int r, int c) { return (double)h[(size_t)std::clamp (r, 0, NG - 1) * NG + std::clamp (c, 0, NG - 1)]; };
			double e = (H (r0, c0) * (1 - tc) + H (r0, c0 + 1) * tc) * (1 - tr) + (H (r0 + 1, c0) * (1 - tc) + H (r0 + 1, c0 + 1) * tc) * tr;
			// slope from the nodes about the texel
			double sx = (H (r0, c0 + 1) - H (r0, c0) + H (r0 + 1, c0 + 1) - H (r0 + 1, c0)) * 0.5 / (dle * R * std::max (cb, 1e-3));
			double sy = (H (r0 + 1, c0) - H (r0, c0) + H (r0 + 1, c0 + 1) - H (r0, c0 + 1)) * 0.5 / (de * R);
			double slope = std::sqrt (sx * sx + sy * sy);
			double p[3] = { cb * std::cos (lng), sb, cb * std::sin (lng) };
			double n1 = VNoise (p[0] * 3.1, p[1] * 3.1, p[2] * 3.1, seed) * 0.7 + VNoise (p[0] * 7.3, p[1] * 7.3, p[2] * 7.3, seed + 1) * 0.3;
			double nf = Fbm (p, 24.0, fmax, seed + 7);
			// the climate: angle from the substellar point (tidally locked),
			// else latitude
			double theta = prm.tidalLock ? std::acos (std::clamp (cb * std::cos (lng), -1.0, 1.0)) : std::fabs (lat);
			double edge = theta - (openW + 0.06 * n1 + 0.015 * nf);
			bool water = false;
			Rgb col { 0, 0, 0 }, glow { 0, 0, 0 };
			double u = e / relief;
			auto Ground = [&](double contrast) {
				double t = Smoothstep (-1.2, 1.4, u + 0.35 * n1 + 0.15 * nf);
				Rgb c = Mix (low, high, t);
				c = Scale (c, (1.0 + contrast * (0.16 * n1 + 0.12 * nf)) * (1.0 + 0.25 * Smoothstep (0.15, 0.7, slope)));
				return c;
			};
			switch (cls) {
			case OCEAN: {
				bool frozen = edge > 0.0;
				if (e < 0.0) {
					if (!frozen) {
						water = true;
						double d = Smoothstep (0.0, 2500.0, -e);
						col = Mix ({ 0.035, 0.10, 0.11 }, { 0.008, 0.022, 0.045 }, d);
						// (the ice's edge: floes and slush)
						double fl = Smoothstep (-0.05, 0.0, edge) * Smoothstep (-0.2, 0.4, nf);
						if (fl > 0.5) water = false, col = Mix ({ 0.45, 0.50, 0.55 }, { 0.78, 0.82, 0.86 }, Smoothstep (0.5, 1.0, fl));
					} else {
						// (sea ice: thinner and darker near its edge, snow-covered
						// further in; leads and pressure ridges)
						col = Mix ({ 0.55, 0.62, 0.68 }, { 0.86, 0.88, 0.91 }, Smoothstep (0.0, 0.25, edge + 0.08 * nf));
						double lead = 1.0 - Smoothstep (0.0, 0.04, std::fabs (VNoise (p[0] * 23.0 + 2.0 * n1, p[1] * 23.0, p[2] * 23.0, seed + 13)));
						col = Mix (col, { 0.30, 0.36, 0.42 }, 0.6 * lead * (1.0 - Smoothstep (0.1, 0.6, edge)));
						col = Scale (col, 1.0 + 0.08 * nf + 0.06 * n1);
					}
				} else {
					col = Ground (1.0);
					// (coasts: sediment; snow beyond the ice edge and on the
					// heights)
					col = Mix (col, Scale (low, 1.3), 0.4 * (1.0 - Smoothstep (0.0, 150.0, e)));
					double snow = std::max (Smoothstep (0.05, 0.2, edge + 0.04 * nf), Smoothstep (3500.0, 5500.0, e + 600.0 * nf - 1500.0 * std::cos (theta)));
					snow *= 1.0 - 0.6 * Smoothstep (0.5, 1.0, slope);
					col = Mix (col, { 0.84, 0.86, 0.90 }, snow);
				}
				break; }
			case ICE: {
				col = Mix (high, low, 0.35 * (1.0 - Smoothstep (-1.0, 0.8, u)));
				// lineae: dark bands along the noise's zero lines
				double q = VNoise (p[0] * 9.0 + n1, p[1] * 9.0, p[2] * 9.0 - n1, seed + 3) + 0.3 * VNoise (p[0] * 37.0, p[1] * 37.0, p[2] * 37.0, seed + 4);
				double line = 1.0 - Smoothstep (0.0, 0.035, std::fabs (q));
				col = Mix (col, Scale (low, 0.75), 0.75 * line);
				col = Scale (col, 1.0 + 0.06 * nf + 0.05 * n1);
				if (openW > 0.0 && edge < 0.0) {
					water = edge < -0.02;
					col = water ? Rgb { 0.01, 0.025, 0.045 } : Rgb { 0.35, 0.40, 0.46 };
				}
				break; }
			case LAVA: {
				col = Scale (Ground (1.2), 0.6);
				double q = std::fabs (VNoise (p[0] * 14.0 + n1, p[1] * 14.0, p[2] * 14.0, seed + 5));
				double flow = (1.0 - Smoothstep (0.0, 0.05, q)) * Smoothstep (0.0, 0.5, n1 + 0.3);
				glow = Scale ({ 1.0, 0.32, 0.05 }, flow);
				break; }
			case DESERT:
				col = Ground (1.6);
				break;
			default:
				col = Ground (1.0);
				break;
			}
			if (mask) {
				o[0] = Srgb (glow.r), o[1] = Srgb (glow.g), o[2] = Srgb (glow.b);
				o[3] = water ? 0 : 255;
			} else {
				o[0] = Srgb (col.r), o[1] = Srgb (col.g), o[2] = Srgb (col.b), o[3] = 255;
			}
		}
	}
}

void ProcPlanet::SurfTile (int lvl, int ilat, int ilng, std::vector<uint8_t> &dds)
{
	DdsHeader (dds, false);
	Texels (lvl, ilat, ilng, dds, false);
}

void ProcPlanet::MaskTile (int lvl, int ilat, int ilng, std::vector<uint8_t> &dds)
{
	DdsHeader (dds, true);
	Texels (lvl, ilat, ilng, dds, true);
}

void ProcPlanet::CloudTile (int lvl, int ilat, int ilng, std::vector<uint8_t> &dds) const
{
	DdsHeader (dds, true);
	const int nlat = 1 << (lvl - 4), nlng = 2 << (lvl - 4);
	const double latmax = PI_ * (0.5 - (double)ilat / nlat), dlat = PI_ / nlat;
	const double lngmin = -PI_ + 2.0 * PI_ * ilng / nlng, dlng = 2.0 * PI_ / nlng;
	const double fmax = N / (2.0 * dlat);
	const uint32_t seed = prm.seed * 2654435761u + 23u;
	uint8_t *o = dds.data () + 128;
	for (int y = 0; y < N; y++) {
		double lat = latmax - (y + 0.5) / N * dlat, cb = std::cos (lat), sb = std::sin (lat);
		for (int x = 0; x < N; x++, o += 4) {
			double lng = lngmin + (x + 0.5) / N * dlng;
			double p[3] = { cb * std::cos (lng), sb, cb * std::sin (lng) };
			double cover;
			if (prm.tidalLock) {
				// (the substellar point's convection: a deck of cells; beyond,
				// clouds drawn out east and west by the winds from it, thinning
				// towards the terminator; a little on the night side)
				double theta = std::acos (std::clamp (p[0], -1.0, 1.0));
				double cells = Fbm (p, 6.0, fmax, seed, 0.6);
				double q[3] = { p[0] * 0.5, p[1] * 2.5, p[2] * 0.5 };
				double streak = Fbm (q, 5.0, fmax, seed + 5, 0.55);
				double deck = Smoothstep (0.75, 0.2, theta + 0.3 * cells) * (0.55 + 0.6 * cells);
				double rest = 0.18 - 0.12 * Smoothstep (0.8, 1.8, theta) + 0.6 * streak;
				cover = std::max (deck, rest);
			} else {
				// (bands: the equator's convergence, the mid-latitude storms)
				double band = 0.35 + 0.3 * std::exp (-lat * lat / 0.02) + 0.25 * std::exp (-std::pow (std::fabs (lat) - 0.9, 2.0) / 0.03);
				cover = band + 0.6 * Fbm (p, 5.0, fmax, seed, 0.55);
			}
			cover = Smoothstep (0.35, 0.85, cover);
			o[0] = o[1] = o[2] = 245;
			o[3] = (uint8_t)std::lround (255.0 * std::clamp (cover, 0.0, 1.0));
		}
	}
}

void ProcPlanet::GasMap (int w, int h, std::vector<uint8_t> &rgba) const
{
	// belts and zones: bands in latitude, their edges wavy (turbulence),
	// darker and bluer towards the poles
	rgba.assign ((size_t)w * h * 4, 255);
	const uint32_t seed = prm.seed * 2246822519u + 11u;
	const Rgb belt { prm.colLow[0], prm.colLow[1], prm.colLow[2] }, zone { prm.colHigh[0], prm.colHigh[1], prm.colHigh[2] };
	double phase = 6.28 * (Pcg (seed) >> 8) / 16777216.0;
	for (int y = 0; y < h; y++) {
		double lat = PI_ * (0.5 - (y + 0.5) / h), cb = std::cos (lat), sb = std::sin (lat);
		for (int x = 0; x < w; x++) {
			double lng = -PI_ + 2.0 * PI_ * (x + 0.5) / w;
			double p[3] = { cb * std::cos (lng), sb, cb * std::sin (lng) };
			double q[3] = { p[0] * 0.6, p[1] * 3.0, p[2] * 0.6 };
			double turb = Fbm (q, 4.0, 160.0, seed, 0.55);
			double band = 0.5 + 0.5 * std::sin (lat * 13.0 + phase + 1.2 * turb + 0.6 * std::sin (lat * 5.0));
			band = Smoothstep (0.25, 0.75, band);
			Rgb c = Mix (belt, zone, band);
			double fine = Fbm (p, 40.0, 600.0, seed + 9);
			c = Scale (c, 1.0 + 0.05 * fine + 0.08 * turb);
			double pole = Smoothstep (0.85, 1.35, std::fabs (lat));
			c = Mix (c, { belt.r * 0.75, belt.g * 0.8, belt.b * 0.92 }, pole);
			uint8_t *o = rgba.data () + ((size_t)y * w + x) * 4;
			o[0] = Srgb (c.r), o[1] = Srgb (c.g), o[2] = Srgb (c.b), o[3] = 255;
		}
	}
}

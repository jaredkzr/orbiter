// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ScatterAssets.h"
#include "Scene.h"
#include "Shadows.h"
#include "VkTexture.h"
#include "VulkanClient.h"
#include "VesselAPI.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

namespace {

	constexpr double TWOPI = 6.283185307179586;

	// ------------------------------------------------------------------
	// random numbers and noise

	struct Rng {
		uint32_t s;
		explicit Rng (uint32_t seed): s (seed * 747796405u + 2891336453u) {}
		uint32_t Next () { s = s * 747796405u + 2891336453u; uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u; return (w >> 22u) ^ w; }
		double U () { return (Next () & 0xffffff) / 16777216.0; }       // 0..1
		double R (double a, double b) { return a + (b - a) * U (); }
	};

	uint32_t Hash3 (int x, int y, int z, uint32_t seed)
	{
		uint32_t h = seed * 0x9E3779B9u ^ (uint32_t)x * 0x85EBCA6Bu ^ (uint32_t)y * 0xC2B2AE35u ^ (uint32_t)z * 0x27D4EB2Fu;
		h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
		return h;
	}

	// gradient noise, about -1 to 1
	double Noise (double x, double y, double z, uint32_t seed)
	{
		int ix = (int)std::floor (x), iy = (int)std::floor (y), iz = (int)std::floor (z);
		double fx = x - ix, fy = y - iy, fz = z - iz;
		auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
		auto grad = [&](int dx, int dy, int dz) {
			uint32_t h = Hash3 (ix + dx, iy + dy, iz + dz, seed) % 12u;
			double gx = (h < 4 || h == 8 || h == 10) ? ((h & 1) ? -1.0 : 1.0) : (h < 8 ? ((h & 1) ? -1.0 : 1.0) : 0.0);
			double gy = h < 4 ? ((h & 2) ? -1.0 : 1.0) : (h >= 8 ? ((h & 1) ? -1.0 : 1.0) : 0.0);
			double gz = h >= 4 ? ((h & 2) ? -1.0 : 1.0) : 0.0;
			if (h >= 8) gx = 0.0;
			return gx * (fx - dx) + gy * (fy - dy) + gz * (fz - dz);
		};
		double u = fade (fx), v = fade (fy), w = fade (fz);
		auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
		return lerp (lerp (lerp (grad (0,0,0), grad (1,0,0), u), lerp (grad (0,1,0), grad (1,1,0), u), v),
		             lerp (lerp (grad (0,0,1), grad (1,0,1), u), lerp (grad (0,1,1), grad (1,1,1), u), v), w);
	}

	double Fbm (VECTOR3 p, int oct, uint32_t seed)
	{
		double s = 0.0, a = 0.5;
		for (int i = 0; i < oct; i++) {
			s += a * Noise (p.x, p.y, p.z, seed + i * 101);
			p = p * 2.03;
			a *= 0.5;
		}
		return s;
	}

	// ------------------------------------------------------------------
	// geometry

	// Triangles are wound as in Orbiter meshes: clockwise seen from the front
	// in Orbiter's left-handed frames, i.e. the cross product of their edges
	// (b - a) x (c - a) points out of the front
	struct Builder {
		std::vector<NTVERTEX> v;
		std::vector<uint16_t> i;
		uint16_t Add (const VECTOR3 &p, const VECTOR3 &n, double tu, double tv)
		{
			NTVERTEX x;
			x.x = (float)p.x, x.y = (float)p.y, x.z = (float)p.z;
			x.nx = (float)n.x, x.ny = (float)n.y, x.nz = (float)n.z;
			x.tu = (float)tu, x.tv = (float)tv;
			v.push_back (x);
			return (uint16_t)(v.size() - 1);
		}
		// a triangle facing along n (outward)
		void Tri (uint16_t a, uint16_t b, uint16_t c, const VECTOR3 &out)
		{
			VECTOR3 pa = _V(v[a].x, v[a].y, v[a].z), pb = _V(v[b].x, v[b].y, v[b].z), pc = _V(v[c].x, v[c].y, v[c].z);
			if (dotp (crossp (pb - pa, pc - pa), out) < 0.0) std::swap (b, c);
			i.insert (i.end(), { a, b, c });
		}
		void Append (const Builder &o)
		{
			uint16_t base = (uint16_t)v.size();
			v.insert (v.end(), o.v.begin(), o.v.end());
			for (uint16_t k : o.i) i.push_back (base + k);
		}
	};

	// unit icosphere: positions and triangles
	void Icosphere (int level, std::vector<VECTOR3> &pos, std::vector<std::array<int,3>> &tri)
	{
		const double t = (1.0 + std::sqrt (5.0)) / 2.0;
		pos = { {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
		        {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1} };
		for (auto &p : pos) p = unit (p);
		tri = { {0,11,5}, {0,5,1}, {0,1,7}, {0,7,10}, {0,10,11}, {1,5,9}, {5,11,4}, {11,10,2}, {10,7,6}, {7,1,8},
		        {3,9,4}, {3,4,2}, {3,2,6}, {3,6,8}, {3,8,9}, {4,9,5}, {2,4,11}, {6,2,10}, {8,6,7}, {9,8,1} };
		for (int l = 0; l < level; l++) {
			std::map<std::pair<int,int>, int> mid;
			auto midpoint = [&](int a, int b) {
				auto key = std::make_pair (std::min (a, b), std::max (a, b));
				auto it = mid.find (key);
				if (it != mid.end()) return it->second;
				pos.push_back (unit ((pos[a] + pos[b]) * 0.5));
				return mid[key] = (int)pos.size() - 1;
			};
			std::vector<std::array<int,3>> nt;
			for (auto &f : tri) {
				int a = midpoint (f[0], f[1]), b = midpoint (f[1], f[2]), c = midpoint (f[2], f[0]);
				nt.push_back ({ f[0], a, c }); nt.push_back ({ f[1], b, a });
				nt.push_back ({ f[2], c, b }); nt.push_back ({ a, b, c });
			}
			tri.swap (nt);
		}
	}

	// A closed surface from a deformed unit sphere (shape: unit direction ->
	// position), with smooth normals and spherical texture coordinates
	// (the seam split), appended to b
	template<typename F>
	void Blob (Builder &b, int level, F shape, double uScale = 1.0, double vScale = 1.0)
	{
		std::vector<VECTOR3> dir;
		std::vector<std::array<int,3>> tri;
		Icosphere (level, dir, tri);
		std::vector<VECTOR3> p (dir.size()), n (dir.size(), _V(0, 0, 0));
		for (size_t k = 0; k < dir.size(); k++) p[k] = shape (dir[k]);
		VECTOR3 c = _V(0, 0, 0);
		for (auto &q : p) c += q;
		c = c / (double)p.size();
		for (auto &f : tri) {
			VECTOR3 fn = crossp (p[f[1]] - p[f[0]], p[f[2]] - p[f[0]]);
			if (dotp (fn, p[f[0]] - c) < 0.0) fn = -fn;
			for (int k = 0; k < 3; k++) n[f[k]] += fn;
		}
		Builder s;
		for (size_t k = 0; k < dir.size(); k++) {
			double u = std::atan2 (dir[k].z, dir[k].x) / TWOPI + 0.5;
			double v = std::acos (std::clamp (dir[k].y, -1.0, 1.0)) / 3.141592653589793;
			s.Add (p[k], unit (n[k]), u * uScale, v * vScale);
		}
		for (auto &f : tri) {
			int idx[3] = { f[0], f[1], f[2] };
			double u0 = s.v[idx[0]].tu, u1 = s.v[idx[1]].tu, u2 = s.v[idx[2]].tu;
			double umax = std::max ({ u0, u1, u2 }), umin = std::min ({ u0, u1, u2 });
			if (umax - umin > 0.5 * uScale) { // across the seam: duplicates on the far side
				for (int k = 0; k < 3; k++)
					if (s.v[idx[k]].tu < 0.5 * uScale) {
						NTVERTEX x = s.v[idx[k]];
						x.tu += (float)uScale;
						s.v.push_back (x);
						idx[k] = (int)s.v.size() - 1;
					}
			}
			s.Tri ((uint16_t)idx[0], (uint16_t)idx[1], (uint16_t)idx[2], p[f[0]] - c);
		}
		b.Append (s);
	}

	// a tapered cylinder from a to b (radii ra, rb), sides around, texture
	// u around, v along (vScale)
	void Cylinder (Builder &b, const VECTOR3 &a, const VECTOR3 &e, double ra, double rb, int sides, double vScale)
	{
		VECTOR3 ax = unit (e - a);
		VECTOR3 t1 = unit (crossp (ax, std::fabs (ax.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0)));
		VECTOR3 t2 = crossp (ax, t1);
		Builder s;
		for (int k = 0; k <= sides; k++) {
			double ang = TWOPI * k / sides;
			VECTOR3 d = t1 * std::cos (ang) + t2 * std::sin (ang);
			s.Add (a + d * ra, d, (double)k / sides, 0.0);
			s.Add (e + d * rb, d, (double)k / sides, vScale);
		}
		for (int k = 0; k < sides; k++) {
			uint16_t p0 = 2 * k, p1 = 2 * k + 1, p2 = 2 * k + 2, p3 = 2 * k + 3;
			VECTOR3 out = _V(s.v[p0].nx, s.v[p0].ny, s.v[p0].nz);
			s.Tri (p0, p2, p1, out);
			s.Tri (p1, p2, p3, out);
		}
		b.Append (s);
	}

	// A ribbed column (cactus): from a along unit d, length len, radius
	// r * profile(t) (t = 0 to 1 along it), ribs ridges around (radius up
	// by a share ribDepth on them), closed by a dome of its radius at the
	// end; texture u one rib per unit (ridge at 0.5), v vPerM per metre
	template<typename F>
	void RibbedColumn (Builder &b, const VECTOR3 &a, const VECTOR3 &d, double len, double r, F profile,
		int ribs, int sidesPerRib, int rings, double ribDepth, double vPerM)
	{
		VECTOR3 t1 = unit (crossp (d, std::fabs (d.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0)));
		VECTOR3 t2 = crossp (d, t1);
		int n = ribs * sidesPerRib;
		const int DOME = 4;
		double rEnd = r * profile (1.0);
		double body = std::max (0.0, len - rEnd);
		// rings: along the body, then the dome (to a point)
		std::vector<std::pair<double,double>> ring; // (distance along, radius)
		for (int j = 0; j <= rings; j++) {
			double s = body * j / rings;
			ring.push_back ({ s, r * profile (len > 0 ? s / len : 0.0) });
		}
		for (int j = 1; j <= DOME; j++) {
			double ph = 1.5707963 * j / DOME;
			ring.push_back ({ body + rEnd * std::sin (ph), j == DOME ? 1e-4 : rEnd * std::cos (ph) });
		}
		auto P = [&](int j, double th) {
			double m = 1.0 + ribDepth * std::cos (ribs * th);
			return a + d * ring[j].first + (t1 * std::cos (th) + t2 * std::sin (th)) * (ring[j].second * m);
		};
		Builder s;
		int nr = (int)ring.size();
		for (int j = 0; j < nr; j++)
			for (int k = 0; k <= n; k++) {
				double th = TWOPI * k / n, dth = TWOPI / n * 0.5;
				VECTOR3 p = P (j, th);
				VECTOR3 dt = P (j, th + dth) - P (j, th - dth);
				VECTOR3 ds = j == 0 ? P (1, th) - p : j == nr - 1 ? p - P (j - 1, th) : P (j + 1, th) - P (j - 1, th);
				VECTOR3 nn = crossp (ds, dt);
				VECTOR3 radial = t1 * std::cos (th) + t2 * std::sin (th);
				if (j == nr - 1) nn = d;
				else if (dotp (nn, radial + d * 0.01) < 0.0) nn = -nn;
				s.Add (p, unit (nn), (double)k / sidesPerRib + 0.5, ring[j].first * vPerM);
			}
		for (int j = 0; j + 1 < nr; j++)
			for (int k = 0; k < n; k++) {
				uint16_t p0 = (uint16_t)(j * (n + 1) + k), p1 = p0 + 1, p2 = (uint16_t)(p0 + n + 1), p3 = p2 + 1;
				VECTOR3 out = _V(s.v[p0].nx, s.v[p0].ny, s.v[p0].nz);
				s.Tri (p0, p1, p2, out);
				s.Tri (p1, p3, p2, out);
			}
		b.Append (s);
	}

	// ------------------------------------------------------------------
	// textures (sRGB)

	struct Tex {
		uint32_t w, h;
		std::vector<uint8_t> px;
		Tex (uint32_t w_, uint32_t h_): w (w_), h (h_), px ((size_t)w_ * h_ * 4) {}
		void Set (uint32_t x, uint32_t y, double r, double g, double b, double a = 1.0)
		{
			uint8_t *p = &px[((size_t)y * w + x) * 4];
			auto enc = [](double c) { return (uint8_t)std::lround (std::pow (std::clamp (c, 0.0, 1.0), 1.0 / 2.2) * 255.0); };
			p[0] = enc (r), p[1] = enc (g), p[2] = enc (b), p[3] = (uint8_t)std::lround (std::clamp (a, 0.0, 1.0) * 255.0);
		}
	};

	// a point on a cylinder for texture coordinates (u periodic)
	VECTOR3 Cyl (double u, double v, double su, double sv)
	{
		double a = u * TWOPI;
		return _V(std::cos (a) * su / TWOPI, std::sin (a) * su / TWOPI, v * sv);
	}

	Tex RockTexture (vkscatter::RockStyle style, uint32_t seed)
	{
		Tex t (256, 256);
		VECTOR3 base = style == vkscatter::RockStyle::REGOLITH ? _V(0.20, 0.19, 0.18)
			: style == vkscatter::RockStyle::MARS ? _V(0.36, 0.20, 0.12) : _V(0.30, 0.29, 0.27);
		for (uint32_t y = 0; y < t.h; y++)
			for (uint32_t x = 0; x < t.w; x++) {
				double u = (x + 0.5) / t.w, v = (y + 0.5) / t.h;
				double l = 1.0 + 0.35 * Fbm (Cyl (u, v, 6, 3), 4, seed) + 0.15 * Fbm (Cyl (u, v, 40, 20), 3, seed + 7);
				double grain = Noise (Cyl (u, v, 160, 80).x, Cyl (u, v, 160, 80).y, Cyl (u, v, 160, 80).z, seed + 9);
				l *= 1.0 + 0.12 * grain;
				double crack = 1.0 - std::fabs (Fbm (Cyl (u, v, 10, 5), 3, seed + 3));
				if (crack > 0.9) l *= 0.55 + 0.45 * (1.0 - (crack - 0.9) / 0.1);
				VECTOR3 c = base * l;
				if (style == vkscatter::RockStyle::GRANITE) {
					// lichen patches, and dark mineral speckles
					double li = Fbm (Cyl (u, v, 14, 7), 3, seed + 11);
					if (li > 0.22) c = c * 0.4 + _V(0.32, 0.33, 0.14) * 0.6 * (0.8 + 0.4 * grain);
					if (Noise (Cyl (u, v, 300, 150).x, Cyl (u, v, 300, 150).y, Cyl (u, v, 300, 150).z, seed + 5) > 0.55) c = c * 0.35;
				} else if (style == vkscatter::RockStyle::MARS) {
					// dust settled on the upper parts (v small: top)
					double dust = std::clamp (0.6 - v + 0.3 * Fbm (Cyl (u, v, 8, 4), 2, seed + 13), 0.0, 1.0);
					c = c * (1.0 - 0.6 * dust) + _V(0.48, 0.28, 0.16) * 0.6 * dust;
				}
				t.Set (x, y, c.x, c.y, c.z);
			}
		return t;
	}

	// cellular noise on the texture cylinder: distance to the nearest and
	// second nearest cell point, and the nearest cell's hash
	void Worley (const VECTOR3 &p, uint32_t seed, double &d1, double &d2, uint32_t &id)
	{
		int ix = (int)std::floor (p.x), iy = (int)std::floor (p.y), iz = (int)std::floor (p.z);
		d1 = d2 = 1e9, id = 0;
		for (int dz = -1; dz <= 1; dz++)
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					uint32_t h = Hash3 (ix + dx, iy + dy, iz + dz, seed);
					VECTOR3 c = _V(ix + dx + (h & 0xff) / 255.0, iy + dy + ((h >> 8) & 0xff) / 255.0, iz + dz + ((h >> 16) & 0xff) / 255.0);
					double d = length (c - p);
					if (d < d1) d2 = d1, d1 = d, id = h;
					else if (d < d2) d2 = d;
				}
	}

	enum class RockTex { GRANITE, MOON, MOON_BRECCIA, MARS_BASALT, MARS_VENTIFACT, MARS_SANDSTONE };

	Tex TypedRockTexture (RockTex type, uint32_t seed)
	{
		if (type == RockTex::GRANITE) return RockTexture (vkscatter::RockStyle::GRANITE, seed);
		Tex t (512, 256);
		for (uint32_t y = 0; y < t.h; y++)
			for (uint32_t x = 0; x < t.w; x++) {
				double u = (x + 0.5) / t.w, v = (y + 0.5) / t.h; // (v 0: top)
				VECTOR3 c;
				double grain = Noise (Cyl (u, v, 220, 110).x, Cyl (u, v, 220, 110).y, Cyl (u, v, 220, 110).z, seed + 9);
				switch (type) {
				case RockTex::MOON: case RockTex::MOON_BRECCIA: {
					// neutral grey, rough: pits, lighter speckles; breccia:
					// angular clasts of other greys in a darker matrix
					double l = 1.0 + 0.22 * Fbm (Cyl (u, v, 8, 4), 3, seed) + 0.12 * grain;
					c = _V(0.165, 0.16, 0.155) * l;
					if (type == RockTex::MOON_BRECCIA) {
						double d1, d2;
						uint32_t id;
						Worley (Cyl (u, v, 26, 13), seed + 4, d1, d2, id);
						double clast = (id & 0xff) / 255.0;
						if (clast < 0.45 && d2 - d1 > 0.08) c = _V(0.165, 0.16, 0.155) * (clast < 0.15 ? 0.75 : 1.12 + 0.4 * clast) * l;
						else c = c * 0.92;
					}
					double pit = Noise (Cyl (u, v, 160, 80).x, Cyl (u, v, 160, 80).y, Cyl (u, v, 160, 80).z, seed + 5);
					if (pit > 0.45) c = c * 0.45;
					if (grain > 0.6) c = c * 1.35;
					} break;
				case RockTex::MARS_BASALT: {
					// dark grey-blue, vesicular (pits), dust on the upper faces and in the pits
					double l = 1.0 + 0.15 * Fbm (Cyl (u, v, 10, 5), 3, seed) + 0.1 * grain;
					c = _V(0.085, 0.09, 0.10) * l;
					double pit = Noise (Cyl (u, v, 120, 60).x, Cyl (u, v, 120, 60).y, Cyl (u, v, 120, 60).z, seed + 5);
					double dust = std::clamp (0.4 - v + 0.35 * Fbm (Cyl (u, v, 6, 3), 3, seed + 13), 0.0, 1.0);
					if (v < 0.08) dust = std::max (dust, 0.75); // (the faces pointing up)
					if (pit > 0.4) dust = std::max (dust, 0.8);
					c = c * (1.0 - 0.75 * dust) + _V(0.42, 0.25, 0.14) * (0.75 * dust);
					if (pit > 0.4 && dust < 0.8) c = c * 0.5;
					} break;
				case RockTex::MARS_VENTIFACT: {
					// grey-tan, fluted by the wind (streaks along one direction), crisp
					// (flutes along the wind: long in u, narrow in v)
					double flute = Noise (Cyl (u, v, 3, 90).x, Cyl (u, v, 3, 90).y, Cyl (u, v, 3, 90).z, seed + 2);
					double l = 1.0 + 0.3 * flute + 0.12 * Fbm (Cyl (u, v, 6, 3), 2, seed) + 0.06 * grain;
					c = _V(0.26, 0.22, 0.18) * l;
					if (flute < -0.35) c = c * 0.6;
					double dust = std::clamp (0.35 - v + 0.3 * Fbm (Cyl (u, v, 5, 3), 2, seed + 13), 0.0, 1.0);
					c = c * (1.0 - 0.5 * dust) + _V(0.42, 0.26, 0.15) * (0.5 * dust);
					} break;
				case RockTex::MARS_SANDSTONE: {
					// tan-orange with fine laminations (layers along v)
					// (beds of differing tone down the sides, dark partings between them)
					double vb = v * 9.0 + 0.6 * Fbm (Cyl (u, v, 3, 2), 2, seed);
					double bed = vb - std::floor (vb);
					uint32_t bh = Hash3 ((int)std::floor (vb), 0, 0, seed + 3);
					double l = 0.8 + 0.35 * ((bh & 0xff) / 255.0) + 0.1 * Fbm (Cyl (u, v, 8, 4), 3, seed + 1) + 0.07 * grain;
					c = _V(0.36, 0.23, 0.14) * l;
					if (bed < 0.07 && v > 0.08) c = c * 0.45;
					if (v < 0.08) c = c * 1.05 + _V(0.03, 0.01, 0.0); // (the dusty top)
					} break;
				default: break;
				}
				t.Set (x, y, c.x, c.y, c.z);
			}
		return t;
	}

	Tex GrassTexture (uint32_t seed)
	{
		Tex t (64, 256);
		for (uint32_t y = 0; y < t.h; y++)
			for (uint32_t x = 0; x < t.w; x++) {
				double u = (x + 0.5) / t.w, v = (y + 0.5) / t.h; // v 0: base, 1: tip
				VECTOR3 c = _V(0.03, 0.07, 0.015) * (1.0 - v) + _V(0.16, 0.22, 0.05) * v;
				c = c * (1.0 + 0.25 * Noise (u * 24, v * 3, 0.5, seed)); // (streaks along the blade)
				if (v > 0.85) c = c * (1.0 - (v - 0.85) / 0.15) + _V(0.30, 0.27, 0.12) * ((v - 0.85) / 0.15); // dry tips
				t.Set (x, y, c.x, c.y, c.z);
			}
		return t;
	}

	Tex BarkTexture (uint32_t seed, const VECTOR3 &colour, bool birch)
	{
		Tex t (128, 256);
		for (uint32_t y = 0; y < t.h; y++)
			for (uint32_t x = 0; x < t.w; x++) {
				double u = (x + 0.5) / t.w, v = (y + 0.5) / t.h;
				VECTOR3 c;
				if (birch) {
					// white bark with dark horizontal marks
					double m = Fbm (Cyl (u, v * 6.0, 4, 1), 3, seed);
					c = colour * (0.9 + 0.15 * Fbm (Cyl (u, v, 30, 30), 2, seed + 1));
					if (m > 0.25) c = c * 0.18;
				} else {
					// furrows along the trunk
					double f = Fbm (Cyl (u, v * 0.15, 16, 1), 3, seed);
					double l = 0.7 + 0.5 * f + 0.15 * Noise (Cyl (u, v, 60, 60).x, Cyl (u, v, 60, 60).y, Cyl (u, v, 60, 60).z, seed + 2);
					if (std::fabs (f) < 0.05) l *= 0.5;
					c = colour * l;
				}
				t.Set (x, y, c.x, c.y, c.z);
			}
		return t;
	}

	// Cactus skin: one rib across u (ridge in the middle, grooves at the
	// edges), areoles with spines along the ridge every 1/8 of v; pads
	// (ribbed false): areoles in a staggered grid
	Tex CactusTexture (uint32_t seed, const VECTOR3 &colour, bool ribbed)
	{
		Tex t (64, 256);
		for (uint32_t y = 0; y < t.h; y++)
			for (uint32_t x = 0; x < t.w; x++) {
				double u = (x + 0.5) / t.w, v = (y + 0.5) / t.h;
				double l = 0.85 + 0.15 * Fbm (Cyl (u, v, 2, 6), 3, seed);
				if (ribbed) l *= 0.55 + 0.45 * std::pow (std::sin (3.14159 * u), 0.7); // (grooves darker)
				VECTOR3 c = colour * l;
				// areoles: tan felt with pale spines
				double au = ribbed ? 0.5 : (std::fmod (std::floor (v * 8.0), 2.0) ? 0.25 : 0.75);
				double av = (std::floor (v * 8.0) + 0.5) / 8.0;
				double du = (u - au) * 64.0 / 256.0 * 8.0, dv = (v - av) * 8.0; // (in areole spacings)
				double r = std::sqrt (du * du + dv * dv);
				if (r < 0.09) c = _V(0.45, 0.38, 0.28);
				else if (r < 0.38) {
					double ang = std::atan2 (dv, du);
					double sp = std::fabs (std::sin (ang * 4.0 + seed));
					if (sp > 0.93) c = _V(0.62, 0.58, 0.48);
				}
				t.Set (x, y, c.x, c.y, c.z);
			}
		return t;
	}

	// Foliage cards (alpha-tested): leaves on twigs, or needle sprays,
	// painted procedurally
	enum class LeafShape { BROAD, SMALL, NEEDLE };

	struct Canvas {
		int n;
		std::vector<VECTOR3> col;
		std::vector<float> a;
		explicit Canvas (int size): n (size), col ((size_t)size * size, _V(0, 0, 0)), a ((size_t)size * size, 0.0f) {}
		void Plot (int x, int y, const VECTOR3 &c)
		{
			if (x < 0 || y < 0 || x >= n || y >= n) return;
			col[(size_t)y * n + x] = c, a[(size_t)y * n + x] = 1.0f;
		}
		// a filled leaf: ellipse (length l, width w) at p along direction d, colour c
		void Leaf (const VECTOR3 &p, const VECTOR3 &d, double l, double w, const VECTOR3 &c, double pointed)
		{
			VECTOR3 s = _V(-d.y, d.x, 0);
			int r = (int)std::ceil (std::max (l, w) * n) + 2;
			int cx = (int)(p.x * n), cy = (int)(p.y * n);
			for (int y = cy - r; y <= cy + r; y++)
				for (int x = cx - r; x <= cx + r; x++) {
					VECTOR3 q = _V((x + 0.5) / n - p.x, (y + 0.5) / n - p.y, 0);
					double along = dotp (q, d) / (0.5 * l), across = dotp (q, s) / (0.5 * w);
					// (narrower towards the tip: pointed leaves)
					double width = std::sqrt (std::max (0.0, 1.0 - along * along)) * (1.0 - pointed * std::max (0.0, along) * 0.5);
					if (std::fabs (along) <= 1.0 && std::fabs (across) <= width) {
						double shade = 0.85 + 0.15 * along - 0.2 * std::fabs (across) * (across > 0.0 ? 1.0 : 0.6); // (midrib, one half darker)
						Plot (x, y, c * shade);
					}
				}
		}
		void Line (VECTOR3 p, const VECTOR3 &q, double width, const VECTOR3 &c)
		{
			double len = length (q - p) * n;
			int steps = std::max (1, (int)(len * 2));
			for (int i = 0; i <= steps; i++) {
				VECTOR3 m = p + (q - p) * ((double)i / steps);
				int rw = std::max (0, (int)(width * n * 0.5));
				for (int dy = -rw; dy <= rw; dy++)
					for (int dx = -rw; dx <= rw; dx++) Plot ((int)(m.x * n) + dx, (int)(m.y * n) + dy, c);
			}
		}
	};

	// a twig with leaves (or a needle spray) along it, from the card's
	// bottom centre upwards, branching
	void PaintSpray (Canvas &cv, Rng &r, LeafShape shape, const VECTOR3 &colour)
	{
		VECTOR3 twig = _V(0.16, 0.11, 0.06);
		auto leaf = [&](const VECTOR3 &p, const VECTOR3 &d, double scale) {
			VECTOR3 c = colour * r.R (0.75, 1.25);
			c.y *= r.R (0.95, 1.08);
			if (shape == LeafShape::BROAD) cv.Leaf (p + d * (0.06 * scale), d, 0.13 * scale, 0.07 * scale, c, 0.6);
			else if (shape == LeafShape::SMALL) cv.Leaf (p + d * (0.035 * scale), d, 0.07 * scale, 0.04 * scale, c, 0.8);
			else cv.Line (p, p + d * (0.075 * scale), 0.003, c * 0.8);
		};
		std::function<void(VECTOR3, VECTOR3, double, int)> grow = [&](VECTOR3 p, VECTOR3 d, double len, int level) {
			int n = (int)(len * (shape == LeafShape::NEEDLE ? 140 : shape == LeafShape::SMALL ? 22 : 12));
			for (int i = 0; i < n; i++) {
				VECTOR3 q = p + d * (len / n);
				cv.Line (p, q, shape == LeafShape::NEEDLE ? 0.004 : 0.01, twig);
				// leaves or needles to both sides
				for (int sgn = -1; sgn <= 1; sgn += 2) {
					double ang = r.R (0.5, 1.1) * sgn;
					VECTOR3 ld = unit (d * std::cos (ang) + _V(-d.y, d.x, 0) * std::sin (ang));
					if (shape == LeafShape::NEEDLE || r.U () < 0.7) leaf (q, ld, r.R (0.75, 1.2));
				}
				// side branches
				if (level < 2 && r.U () < 0.18) {
					double ang = r.R (0.4, 0.9) * (r.U () < 0.5 ? -1 : 1);
					grow (q, unit (d * std::cos (ang) + _V(-d.y, d.x, 0) * std::sin (ang)), len * r.R (0.35, 0.55), level + 1);
				}
				p = q;
				d = unit (d + _V(r.R (-0.08, 0.08), r.R (-0.08, 0.08), 0));
			}
			leaf (p, d, 1.1); // (at the tip)
		};
		int nTwig = shape == LeafShape::NEEDLE ? 4 : 2;
		for (int k = 0; k < nTwig; k++)
			grow (_V(0.5 + r.R (-0.05, 0.05), 0.04, 0), unit (_V(r.R (-0.35, 0.35), 1.0, 0)), r.R (0.75, 0.9), 0);
	}

	// mip levels that keep the coverage of the alpha test (alpha scaled so
	// that the same share of each level passes 0.5)
	struct MipChain { uint32_t w, h, nMip; std::vector<uint8_t> data; std::vector<size_t> ofs; };
	MipChain CardMips (const Canvas &cv)
	{
		MipChain m;
		m.w = m.h = (uint32_t)cv.n;
		std::vector<VECTOR3> col = cv.col;
		std::vector<float> a = cv.a;
		// colour bleeds into transparent texels (no dark fringes when filtered)
		for (int pass = 0; pass < 8; pass++) {
			std::vector<VECTOR3> c2 = col;
			std::vector<float> filled = a;
			for (int y = 0; y < cv.n; y++)
				for (int x = 0; x < cv.n; x++) {
					size_t i = (size_t)y * cv.n + x;
					if (filled[i] > 0.0f) continue;
					VECTOR3 s = _V(0, 0, 0);
					int k = 0;
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++) {
							int xx = x + dx, yy = y + dy;
							if (xx < 0 || yy < 0 || xx >= cv.n || yy >= cv.n) continue;
							size_t j = (size_t)yy * cv.n + xx;
							if (a[j] > 0.0f || (pass > 0 && length (col[j]) > 0.0)) s += col[j], k++;
						}
					if (k) c2[i] = s / (double)k;
				}
			col.swap (c2);
		}
		double cover0 = 0.0;
		for (float v : a) cover0 += v >= 0.5f ? 1.0 : 0.0;
		cover0 /= a.size();
		int size = cv.n;
		m.nMip = 0;
		while (true) {
			// level: alpha scaled to the coverage of level 0
			std::vector<float> sorted = a;
			std::sort (sorted.begin(), sorted.end());
			size_t k = (size_t)std::clamp ((1.0 - cover0) * sorted.size(), 0.0, (double)sorted.size() - 1);
			float thr = std::max (sorted[k], 1e-3f);
			double scale = 0.5 / thr;
			m.ofs.push_back (m.data.size());
			for (size_t i = 0; i < a.size(); i++) {
				auto enc = [](double c) { return (uint8_t)std::lround (std::pow (std::clamp (c, 0.0, 1.0), 1.0 / 2.2) * 255.0); };
				m.data.push_back (enc (col[i].x)), m.data.push_back (enc (col[i].y)), m.data.push_back (enc (col[i].z));
				m.data.push_back ((uint8_t)std::lround (std::clamp (a[i] * scale, 0.0, 1.0) * 255.0));
			}
			m.nMip++;
			if (size == 1) break;
			int s2 = size / 2;
			std::vector<VECTOR3> c2 ((size_t)s2 * s2);
			std::vector<float> a2 ((size_t)s2 * s2);
			for (int y = 0; y < s2; y++)
				for (int x = 0; x < s2; x++) {
					VECTOR3 cs = _V(0, 0, 0);
					float as = 0.0f;
					for (int dy = 0; dy < 2; dy++)
						for (int dx = 0; dx < 2; dx++) {
							size_t j = (size_t)(2 * y + dy) * size + 2 * x + dx;
							cs += col[j], as += a[j];
						}
					c2[(size_t)y * s2 + x] = cs * 0.25, a2[(size_t)y * s2 + x] = as * 0.25f;
				}
			col.swap (c2), a.swap (a2), size = s2;
		}
		return m;
	}


	// ------------------------------------------------------------------
	// assets

	// Rock shapes: an ellipsoid (radii relative to half the size), made
	// irregular by noise, cut by fracture planes, roughened. Sharp edges:
	// normals are only smoothed between faces within smoothDeg.
	struct RockShape {
		VECTOR3 radMin, radMax;
		int cutMin, cutMax;        // random fracture planes
		double elevMin, elevMax;   // sine of their normals' elevation
		double offMin, offMax;     // their distance (share of the extent)
		int facets;                // wind-cut facets (upward, around: pyramids, wedges)
		double facetElevMin, facetElevMax, facetOffMin, facetOffMax;
		double sharp;              // share of the material beyond a plane removed (1: flat faces)
		double lumps, rough;       // noise before and after the cuts (share of the size)
		double smoothDeg;
		double bury;               // share of the height below ground
		double knobs = 0.0;        // mid-scale relief after the cuts (share of the size)
		double pits = 0.0;         // depth of pits (vesicles, impact pits; share of the size)
	};

	using Cut = std::pair<VECTOR3, double>;

	Builder Rock (const RockShape &sh, uint32_t seed, double size, const std::vector<Cut> *extra = nullptr,
		const VECTOR3 *scaleOverride = nullptr)
	{
		Rng r (seed);
		VECTOR3 rad = _V(r.R (sh.radMin.x, sh.radMax.x), r.R (sh.radMin.y, sh.radMax.y), r.R (sh.radMin.z, sh.radMax.z)) * (0.5 * size);
		if (scaleOverride) rad = *scaleOverride;
		auto extent = [&](const VECTOR3 &n) {
			return std::sqrt (std::pow (n.x * rad.x, 2) + std::pow (n.y * rad.y, 2) + std::pow (n.z * rad.z, 2));
		};
		std::vector<Cut> cuts;
		int ncut = sh.cutMin + (int)(r.U () * (sh.cutMax - sh.cutMin + 1));
		for (int k = 0; k < ncut; k++) {
			double a = r.U () * TWOPI, e = std::asin (r.R (sh.elevMin, sh.elevMax));
			VECTOR3 n = _V(std::cos (e) * std::cos (a), std::sin (e), std::cos (e) * std::sin (a));
			cuts.push_back ({ n, extent (n) * r.R (sh.offMin, sh.offMax) });
		}
		// (facets < 0: 2 to -facets of them, mostly across the long axis:
		// wedges and keels)
		int nf = sh.facets >= 0 ? sh.facets : 2 + (int)(r.U () * (-sh.facets - 1));
		double a0 = r.U () * TWOPI;
		for (int k = 0; k < nf; k++) {
			double a = sh.facets >= 0 ? a0 + TWOPI * k / nf + r.R (-0.35, 0.35)
				: (k % 2 ? 3.14159 : 0.0) + r.R (-0.7, 0.7) + (k >= 2 ? r.U () * TWOPI : 0.0);
			double e = std::asin (r.R (sh.facetElevMin, sh.facetElevMax));
			VECTOR3 n = _V(std::cos (e) * std::cos (a), std::sin (e), std::cos (e) * std::sin (a));
			cuts.push_back ({ n, extent (n) * r.R (sh.facetOffMin, sh.facetOffMax) });
		}
		if (extra) cuts.insert (cuts.end(), extra->begin(), extra->end());
		uint32_t ns = r.Next ();
		std::vector<VECTOR3> dir;
		std::vector<std::array<int,3>> tri;
		Icosphere (4, dir, tri);
		std::vector<VECTOR3> p (dir.size());
		for (size_t k = 0; k < dir.size(); k++) {
			VECTOR3 d = dir[k];
			VECTOR3 q = _V(d.x * rad.x, d.y * rad.y, d.z * rad.z);
			q += d * (size * sh.lumps * Fbm (d * 2.0, 3, ns));
			for (auto &c : cuts) {
				double t = dotp (q, c.first) - c.second;
				if (t > 0.0) q -= c.first * (t * sh.sharp);
			}
			q += d * (size * (sh.rough * Fbm (d * 14.0, 2, ns + 50) + sh.knobs * Fbm (d * 6.0, 2, ns + 70)));
			if (sh.pits > 0.0) {
				double pn = Noise (d.x * 22.0, d.y * 22.0, d.z * 22.0, ns + 90);
				if (pn > 0.35) q -= d * (size * sh.pits * std::min (1.0, (pn - 0.35) / 0.15));
			}
			p[k] = q;
		}
		VECTOR3 c = _V(0, 0, 0);
		for (auto &q : p) c += q;
		c = c / (double)p.size();
		std::vector<VECTOR3> fn (tri.size());
		std::vector<std::vector<int>> vf (p.size());
		for (size_t f = 0; f < tri.size(); f++) {
			VECTOR3 n = crossp (p[tri[f][1]] - p[tri[f][0]], p[tri[f][2]] - p[tri[f][0]]);
			if (dotp (n, p[tri[f][0]] - c) < 0.0) n = -n;
			fn[f] = length (n) > 1e-12 ? unit (n) : unit (p[tri[f][0]] - c);
			for (int k = 0; k < 3; k++) vf[tri[f][k]].push_back ((int)f);
		}
		const double COS_SMOOTH = std::cos (sh.smoothDeg * 3.141592653589793 / 180.0);
		double py0 = 1e9, py1 = -1e9;
		for (auto &q : p) py0 = std::min (py0, q.y), py1 = std::max (py1, q.y);
		double ph = std::max (py1 - py0, 1e-6);
		// Texture coordinates: faces pointing up flat, in the texture's top
		// band (dust, the weathered surface); the others around the rock,
		// height down the texture (layers, dust on the upper parts)
		Builder b;
		for (size_t f = 0; f < tri.size(); f++) {
			uint16_t idx[3];
			bool top = fn[f].y > 0.75;
			double us[3];
			for (int k = 0; k < 3; k++) us[k] = std::atan2 (p[tri[f][k]].z - c.z, p[tri[f][k]].x - c.x) / TWOPI + 0.5;
			double umax = std::max ({ us[0], us[1], us[2] });
			for (int k = 0; k < 3; k++) {
				int v = tri[f][k];
				VECTOR3 n = _V(0, 0, 0);
				for (int g : vf[v]) if (dotp (fn[g], fn[f]) > COS_SMOOTH) n += fn[g];
				double tu, tv;
				if (top) {
					tu = 0.5 + (p[v].x - c.x) / size;
					tv = 0.02 + 0.05 * ((p[v].z - c.z) / size + 0.5);
				} else {
					double u = us[k];
					if (umax - u > 0.5) u += 1.0; // (across the seam)
					tu = u * 2.0;
					tv = 0.08 + 0.92 * (py1 - p[v].y) / ph;
				}
				idx[k] = b.Add (p[v], unit (n), tu, tv);
			}
			b.Tri (idx[0], idx[1], idx[2], fn[f]);
		}
		double ymin = 1e9, ymax = -1e9;
		for (auto &v : b.v) ymin = std::min (ymin, (double)v.y), ymax = std::max (ymax, (double)v.y);
		float sink = (float)(ymin + sh.bury * (ymax - ymin));
		for (auto &v : b.v) v.y -= sink;
		return b;
	}

	// shapes: Earth's weathered granite; the Moon's blocks, breccia and
	// fragments; Mars' wind-cut ventifacts, sandstone slabs, basalt blocks
	// and float
	const RockShape GRANITE   = { {1, 0.45, 0.6}, {1, 0.85, 1.0}, 6, 12, -0.7, 0.98, 0.5, 0.82, 0, 0, 0, 0, 0, 0.85, 0.035, 0.006, 40, 0.2 };
	const RockShape MOON_BLOCK = { {1, 0.6, 0.7}, {1, 0.9, 1.0}, 8, 14, -0.6, 0.98, 0.55, 0.85, 0, 0, 0, 0, 0, 1.0, 0.045, 0.014, 32, 0.15, 0.025, 0.012 };
	const RockShape MOON_BRECCIA = { {1, 0.7, 0.75}, {1, 0.95, 1.0}, 4, 8, -0.5, 0.95, 0.6, 0.9, 0, 0, 0, 0, 0, 0.92, 0.09, 0.022, 40, 0.25, 0.035, 0.015 };
	const RockShape MOON_FRAGMENT = { {1, 0.45, 0.55}, {1, 0.8, 1.0}, 8, 12, -0.8, 0.98, 0.4, 0.7, 0, 0, 0, 0, 0, 1.0, 0.02, 0.01, 25, 0.1 };
	const RockShape MARS_VENTIFACT = { {1, 0.5, 0.45}, {1, 0.8, 0.65}, 3, 5, -0.6, 0.4, 0.55, 0.85, -3, 0.3, 0.75, 0.25, 0.5, 1.0, 0.015, 0.004, 18, 0.2 };
	const RockShape MARS_SLAB = { {1, 0.14, 0.6}, {1, 0.24, 1.0}, 6, 9, -0.12, 0.12, 0.5, 0.8, 1, 0.97, 0.99, 0.6, 0.8, 1.0, 0.006, 0.003, 12, 0.3 };
	const RockShape MARS_BASALT = { {1, 0.6, 0.7}, {1, 0.9, 1.0}, 7, 12, -0.5, 0.98, 0.5, 0.82, 0, 0, 0, 0, 0, 1.0, 0.025, 0.012, 25, 0.18, 0.01, 0.008 };
	const RockShape MARS_FLOAT = { {1, 0.25, 0.6}, {1, 0.4, 1.0}, 6, 9, -0.3, 0.6, 0.45, 0.8, 0, 0, 0, 0, 0, 1.0, 0.012, 0.006, 22, 0.3 };

	// a boulder split by a fracture through it, the pieces apart (as the
	// Apollo 17 station 6 boulder)
	Builder SplitBoulder (const RockShape &sh, uint32_t seed, double size)
	{
		Rng r (seed + 1);
		double a = r.U () * TWOPI, e = r.R (-0.15, 0.25);
		VECTOR3 n = unit (_V(std::cos (a) * std::cos (e), std::sin (e), std::sin (a) * std::cos (e)));
		double gap = size * 0.06;
		VECTOR3 rad = _V(1.0, r.R (sh.radMin.y, sh.radMax.y), r.R (sh.radMin.z, sh.radMax.z)) * (0.5 * size);
		Builder b;
		for (int side = 0; side < 2; side++) {
			VECTOR3 nn = side ? -n : n;
			// (each piece cut short of the centre: a plane through the
			// centre would fold the surface beyond it; then moved apart)
			double ext = std::sqrt (std::pow (nn.x * rad.x, 2) + std::pow (nn.y * rad.y, 2) + std::pow (nn.z * rad.z, 2));
			std::vector<Cut> extra = { { nn, 0.08 * ext } };
			Builder piece = Rock (sh, seed, size, &extra, &rad);
			VECTOR3 shift = -nn * (0.08 * ext + gap * 0.5 + size * r.R (0.0, 0.04));
			for (auto &v : piece.v) v.x += (float)shift.x, v.z += (float)shift.z;
			b.Append (piece);
		}
		return b;
	}

	// a tuft of grass blades (two-sided); coarser levels of detail: seg
	// segments per blade, every n-th blade (wider, the same tuft otherwise)
	Builder Grass (uint32_t seed, int SEG = 4, int every = 1)
	{
		Rng r (seed);
		Builder b;
		int n = 14 + (int)(r.U () * 12);
		for (int k = 0; k < n; k++) {
			double a = r.U () * TWOPI, rr = 0.12 * std::sqrt (r.U ());
			VECTOR3 base = _V(rr * std::cos (a), 0.0, rr * std::sin (a));
			double h = r.R (0.22, 0.55), w = r.R (0.008, 0.016);
			double yaw = r.U () * TWOPI, bend = r.R (0.15, 0.55);
			// leaning outwards from the tuft's centre, randomly
			VECTOR3 lean = unit (_V(std::cos (a) + r.R (-0.5, 0.5), 0.0, std::sin (a) + r.R (-0.5, 0.5)));
			VECTOR3 side = _V(std::cos (yaw), 0.0, std::sin (yaw));
			VECTOR3 face = unit (crossp (side, _V(0, 1, 0)));
			if (k % every) continue; // (after all random numbers of the blade: the same tuft)
			w *= std::sqrt ((double)every);
			Builder s;
			for (int i = 0; i <= SEG; i++) {
				double t = (double)i / SEG;
				VECTOR3 c = base + lean * (bend * h * t * t) + _V(0, h * t * (1.0 - 0.25 * bend * t), 0);
				double ww = w * (1.0 - t * 0.9);
				VECTOR3 n = unit (face * 0.4 + _V(0, 1, 0) * 0.6);
				s.Add (c - side * ww, n, 0.0, t);
				s.Add (c + side * ww, n, 1.0, t);
			}
			Builder back = s; // the other side
			for (auto &v : back.v) {
				VECTOR3 n = unit (-face * 0.4 + _V(0, 1, 0) * 0.6);
				v.nx = (float)n.x, v.ny = (float)n.y, v.nz = (float)n.z;
			}
			for (int i = 0; i < SEG; i++) {
				uint16_t a0 = 2 * i, a1 = 2 * i + 1, b0 = 2 * i + 2, b1 = 2 * i + 3;
				s.Tri (a0, b0, a1, face), s.Tri (a1, b0, b1, face);
				back.Tri (a0, b0, a1, -face), back.Tri (a1, b0, b1, -face);
			}
			b.Append (s);
			b.Append (back);
		}
		return b;
	}

	// ------------------------------------------------------------------
	// trees and shrubs: branches grown procedurally, foliage on cards

	// a double-sided foliage card centred at c, spanning u and v (half
	// sizes), lit as part of a crown around centre (normals outward)
	void Card (Builder &b, const VECTOR3 &c, const VECTOR3 &u, const VECTOR3 &v, const VECTOR3 &centre, double crownR)
	{
		VECTOR3 face = unit (crossp (u, v));
		VECTOR3 radial = c - centre;
		double rl = length (radial);
		radial = rl > 1e-6 ? radial / rl : _V(0, 1, 0);
		// (more outward at the crown's surface, more the card's own facing inside)
		double f = std::clamp (rl / std::max (crownR, 1e-3), 0.3, 1.0);
		for (int side = 0; side < 2; side++) {
			VECTOR3 fs = side ? -face : face;
			VECTOR3 n = unit (radial * f + fs * (1.0 - f) * 0.6 + _V(0, 0.25, 0));
			uint16_t k0 = b.Add (c - u - v, n, 0.0, 1.0), k1 = b.Add (c + u - v, n, 1.0, 1.0);
			uint16_t k2 = b.Add (c + u + v, n, 1.0, 0.0), k3 = b.Add (c - u + v, n, 0.0, 0.0);
			b.Tri (k0, k1, k2, fs), b.Tri (k0, k2, k3, fs);
		}
	}

	struct Branch { VECTOR3 a, b; double ra, rb; };

	// grow a branch from p along d: bending with its growth and towards the
	// light (tropism), children at random points, foliage at the ends
	struct Grower {
		Rng &r;
		int maxLevel;
		double spread;     // child angle [rad]
		double tropism;    // upward tendency
		double childLen;   // child length / parent
		int children;      // per branch (about)
		std::vector<Branch> branches;
		std::vector<std::pair<VECTOR3, VECTOR3>> leafPoints; // position, branch direction
		void Grow (VECTOR3 p, VECTOR3 d, double len, double rad, int level)
		{
			const int STEPS = 4;
			std::vector<VECTOR3> pts = { p };
			for (int s = 0; s < STEPS; s++) {
				d = unit (d + _V(r.R (-0.25, 0.25), r.R (-0.15, 0.15), r.R (-0.25, 0.25)) + _V(0, tropism * 0.3, 0));
				VECTOR3 q = p + d * (len / STEPS);
				double r0 = rad * (1.0 - 0.6 * s / STEPS), r1 = rad * (1.0 - 0.6 * (s + 1) / STEPS);
				branches.push_back ({ p, q, r0, r1 });
				p = q;
				pts.push_back (p);
				if (level >= maxLevel - 1 && s >= 1) leafPoints.push_back ({ p, d });
			}
			if (level >= maxLevel) { leafPoints.push_back ({ p, d }); return; }
			int n = children + (r.U () < 0.5 ? 1 : 0);
			for (int k = 0; k < n; k++) {
				double t = r.R (0.35, 1.0);
				int s = std::min (STEPS - 1, (int)(t * STEPS));
				VECTOR3 base = pts[s] + (pts[s + 1] - pts[s]) * (t * STEPS - s);
				VECTOR3 side = unit (crossp (d, std::fabs (d.y) < 0.95 ? _V(0, 1, 0) : _V(1, 0, 0)));
				double az = r.U () * TWOPI;
				VECTOR3 perp = side * std::cos (az) + crossp (d, side) * std::sin (az);
				double ang = spread * r.R (0.7, 1.3);
				VECTOR3 cd = unit (d * std::cos (ang) + perp * std::sin (ang));
				Grow (base, cd, len * childLen * r.R (0.8, 1.15), rad * 0.6, level + 1);
			}
		}
	};

	// (lod: coarser levels of detail leave out thinner branches, with fewer sides)
	void BranchMesh (Builder &b, const std::vector<Branch> &br, double minRad, int lod = 0)
	{
		minRad *= lod == 0 ? 1.0 : lod == 1 ? 3.0 : 8.0;
		for (const Branch &x : br) {
			if (x.ra < minRad) continue; // (thin twigs: the cards show them)
			int sides = x.ra > 0.15 ? 9 : x.ra > 0.05 ? 6 : 4;
			if (lod) sides = std::max (3, sides - 2 * lod);
			Cylinder (b, x.a, x.b, x.ra, x.rb, sides, length (x.b - x.a) / 1.5);
		}
	}

	// cards around the leaf points of a grower: clusters facing about randomly
	void Foliage (Builder &b, Rng &r, const std::vector<std::pair<VECTOR3, VECTOR3>> &pts, double cardSize, int perPoint,
		const VECTOR3 &centre, double crownR)
	{
		for (auto &lp : pts) {
			for (int k = 0; k < perPoint; k++) {
				VECTOR3 c = lp.first + _V(r.R (-1, 1), r.R (-0.6, 0.8), r.R (-1, 1)) * (cardSize * 0.35);
				// card up along the branch, more or less, rotated about it
				VECTOR3 up = unit (lp.second + _V(r.R (-0.6, 0.6), r.R (0.0, 0.8), r.R (-0.6, 0.6)));
				VECTOR3 side = unit (crossp (up, _V(r.R (-1, 1), r.R (-1, 1), r.R (-1, 1)) + up * 0.01));
				double s = cardSize * r.R (0.75, 1.25);
				Card (b, c + up * (0.4 * s), side * (0.5 * s), up * (0.5 * s), centre, crownR);
			}
		}
	}

	enum class Species { OAK, POPLAR, BIRCH, ACACIA, SPRUCE, FIR, PINE, SHRUB_ROUND, SHRUB_SPREAD, SHRUB_TALL,
		CREOSOTE, SAGEBRUSH, SAGUARO, BARREL, PRICKLY_PEAR };
	inline bool IsCactus (Species sp) { return sp == Species::SAGUARO || sp == Species::BARREL || sp == Species::PRICKLY_PEAR; }

	// a tree or shrub of the species: bark and foliage parts
	// (lod: level of detail 0 to 2: the same plant with fewer branches; the
	// foliage cards are thinned out afterwards, see ThinCards)
	void Plant (Species sp, uint32_t seed, double height, Builder &bark, Builder &leaves, int lod = 0)
	{
		Rng r (seed);
		auto lean = [&]() { return unit (_V(r.R (-0.12, 0.12), 1.0, r.R (-0.12, 0.12))); };
		switch (sp) {
		case Species::OAK: case Species::POPLAR: case Species::BIRCH: case Species::ACACIA: {
			// trunk, then a branching crown
			double trunkH = height * (sp == Species::ACACIA ? r.R (0.45, 0.55) : sp == Species::POPLAR ? r.R (0.15, 0.22) : r.R (0.28, 0.38));
			double trunkR = height * (sp == Species::BIRCH ? 0.014 : sp == Species::POPLAR ? 0.018 : 0.03);
			Grower g{ r, 3,
				sp == Species::POPLAR ? 0.35 : sp == Species::ACACIA ? 1.1 : sp == Species::BIRCH ? 0.65 : 0.85,
				sp == Species::POPLAR ? 1.6 : sp == Species::ACACIA ? -0.2 : sp == Species::BIRCH ? 0.2 : 0.4,
				sp == Species::ACACIA ? 0.7 : 0.62,
				sp == Species::POPLAR || sp == Species::OAK ? 3 : 2 };
			VECTOR3 d = lean ();
			VECTOR3 top = _V(0, 0, 0);
			// trunk in a few bent segments
			for (int s = 0; s < 3; s++) {
				VECTOR3 q = top + d * (trunkH / 3.0);
				g.branches.push_back ({ top, q, trunkR * (1.0 - 0.15 * s), trunkR * (1.0 - 0.15 * (s + 1)) });
				top = q;
				d = unit (d + _V(r.R (-0.1, 0.1), 0.0, r.R (-0.1, 0.1)));
			}
			int nMain = sp == Species::ACACIA ? 3 + (int)(r.U () * 2) : 2 + (int)(r.U () * 3);
			double crownLen = (height - trunkH) * (sp == Species::ACACIA ? 0.75 : 0.7);
			for (int k = 0; k < nMain; k++) {
				double az = TWOPI * k / nMain + r.R (-0.5, 0.5);
				double el = sp == Species::POPLAR ? r.R (1.25, 1.45) : sp == Species::ACACIA ? r.R (0.5, 0.8) : r.R (0.7, 1.15);
				VECTOR3 md = unit (_V(std::cos (az) * std::cos (el), std::sin (el), std::sin (az) * std::cos (el)));
				g.Grow (top, md, crownLen * r.R (0.75, 1.1), trunkR * 0.7, 1);
			}
			// the leader continues up (not acacia)
			if (sp != Species::ACACIA) g.Grow (top, lean (), crownLen * 0.9, trunkR * 0.75, 1);
			BranchMesh (bark, g.branches, trunkR * 0.08, lod);
			VECTOR3 cc = _V(0, 0, 0);
			for (auto &lp : g.leafPoints) cc += lp.first;
			cc = cc / std::max ((double)g.leafPoints.size(), 1.0);
			double cr = 0.0;
			for (auto &lp : g.leafPoints) cr = std::max (cr, length (lp.first - cc));
			double card = height * (sp == Species::BIRCH ? 0.09 : sp == Species::POPLAR ? 0.08 : 0.11);
			Foliage (leaves, r, g.leafPoints, card, 3, cc, cr);
			} break;
		case Species::SPRUCE: case Species::FIR: case Species::PINE: {
			// a straight trunk with whorls of drooping branches, needle sprays along them
			double trunkR = height * 0.016;
			Cylinder (bark, _V(0, -0.3, 0), _V(0, height, 0), trunkR, trunkR * 0.15, 8 - 2 * lod, height / 3.0);
			double h0 = height * (sp == Species::PINE ? r.R (0.45, 0.6) : r.R (0.08, 0.15));
			double lMax = height * (sp == Species::PINE ? 0.2 : sp == Species::FIR ? 0.24 : 0.27);
			double y = h0;
			int whorl = 0;
			std::vector<std::pair<VECTOR3, VECTOR3>> pts;
			while (y < height * 0.97) {
				double f = (y - h0) / (height - h0); // 0 at the bottom of the crown
				double len = sp == Species::PINE ? lMax * (0.6 + 0.4 * std::sin (3.14159 * std::min (f * 1.2, 1.0))) * (1.0 - 0.5 * f)
					: lMax * (1.0 - f) + 0.2; // (conical)
				int n = 4 + (int)(r.U () * 3);
				double az0 = r.U () * TWOPI;
				for (int k = 0; k < n; k++) {
					double az = az0 + TWOPI * k / n + r.R (-0.3, 0.3);
					double droop = sp == Species::SPRUCE ? r.R (-0.45, -0.15) : sp == Species::FIR ? r.R (-0.1, 0.15) : r.R (0.0, 0.35);
					droop += (1.0 - f) * -0.1;
					VECTOR3 d = unit (_V(std::cos (az) * std::cos (droop), std::sin (droop), std::sin (az) * std::cos (droop)));
					double l = len * r.R (0.8, 1.15);
					VECTOR3 a = _V(0, y, 0), b = a + d * l;
					// a slight sag towards the tip
					VECTOR3 m = (a + b) * 0.5 - _V(0, l * (sp == Species::SPRUCE ? 0.12 : 0.05), 0);
					if (l > 0.6 && lod < 2) {
						Cylinder (bark, a, m, trunkR * 0.25, trunkR * 0.15, 4 - lod, 1.0);
						if (!lod) Cylinder (bark, m, b, trunkR * 0.15, trunkR * 0.06, 4, 1.0);
					}
					for (double t = 0.15; t <= 1.0; t += 0.17)
						pts.push_back ({ t < 0.5 ? a + (m - a) * (t / 0.5) : m + (b - m) * ((t - 0.5) / 0.5), unit (b - a) });
				}
				y += (sp == Species::PINE ? 0.55 : 0.32) * r.R (0.8, 1.2) + height * 0.008;
				whorl++;
			}
			pts.push_back ({ _V(0, height * 0.98, 0), _V(0, 1, 0) }); // (the top)
			double card = height * (sp == Species::PINE ? 0.06 : 0.045);
			Foliage (leaves, r, pts, card, 2, _V(0, (h0 + height) * 0.5, 0), (height - h0) * 0.5);
			} break;
		case Species::SAGUARO: {
			// a ribbed column with a few arms: out from it, bending up
			int spr = lod == 0 ? 2 : 1, rings = lod == 0 ? 10 : lod == 1 ? 6 : 3;
			int ribs = lod >= 2 ? 6 : 13;
			double rad = height * 0.035;
			auto straight = [](double) { return 1.0; };
			auto taper = [](double t) { return 1.0 - 0.15 * t; };
			RibbedColumn (bark, _V(0, -0.4, 0), _V(0, 1, 0), height + 0.4, rad, taper, ribs, spr, rings, 0.06, 2.5);
			int arms = (int)(r.U () * 4.0);
			double az0 = r.U () * TWOPI;
			for (int k = 0; k < arms; k++) {
				double az = az0 + TWOPI * k / std::max (arms, 1) + r.R (-0.5, 0.5);
				VECTOR3 out = _V(std::cos (az), 0, std::sin (az));
				double y0 = height * r.R (0.35, 0.6), ar = rad * r.R (0.65, 0.8);
				double reach = rad + ar * r.R (1.5, 2.5);
				VECTOR3 p0 = _V(0, y0, 0) + out * (rad * 0.5);
				// out (slightly up), the elbow, then up
				VECTOR3 d1 = unit (out + _V(0, 0.25, 0));
				RibbedColumn (bark, p0, d1, reach, ar, straight, ribs, spr, std::max (2, rings / 3), 0.06, 2.5);
				VECTOR3 p1 = p0 + d1 * (reach - ar * 0.6);
				VECTOR3 d2 = unit (out + _V(0, 1.4, 0));
				RibbedColumn (bark, p1, d2, ar * 2.5, ar, straight, ribs, spr, 2, 0.06, 2.5);
				VECTOR3 p2 = p1 + d2 * (ar * 1.6);
				double up = std::max (ar * 3.0, height * r.R (0.75, 0.92) - p2.y);
				RibbedColumn (bark, p2, unit (_V(0, 1, 0) + out * 0.05), up, ar, straight, ribs, spr, std::max (2, rings / 2), 0.06, 2.5);
			}
			} break;
		case Species::BARREL: {
			// a squat ribbed barrel, widest in the middle
			int spr = lod == 0 ? 2 : 1, ribs = lod >= 2 ? 8 : 18;
			auto bulge = [](double t) { return 0.75 + 0.25 * std::sin (3.14159 * std::min (t * 1.2, 1.0)); };
			double rad = height * 0.42;
			RibbedColumn (bark, _V(0, -0.1, 0), unit (_V(r.R (-0.1, 0.1), 1, r.R (-0.1, 0.1))), height + 0.1, rad, bulge, ribs, spr,
				lod == 0 ? 6 : 3, 0.09, 4.0);
			} break;
		case Species::PRICKLY_PEAR: {
			// flat oval pads, each growing one or two from its rim
			struct Pad { VECTOR3 c, up, face; double s; int depth; };
			std::vector<Pad> todo;
			int roots = 2 + (int)(r.U () * 3);
			for (int k = 0; k < roots; k++) {
				double az = r.U () * TWOPI;
				VECTOR3 up = unit (_V(std::cos (az) * 0.4, 1.0, std::sin (az) * 0.4));
				VECTOR3 face = unit (crossp (up, _V(std::cos (az + 1.5), 0, std::sin (az + 1.5))));
				double s = height * 0.28;
				todo.push_back ({ _V(std::cos (az) * 0.1, s * 0.8, std::sin (az) * 0.1), up, face, s, 0 });
			}
			int level = lod == 0 ? 2 : 1;
			while (!todo.empty()) {
				Pad p = todo.back ();
				todo.pop_back ();
				VECTOR3 side = unit (crossp (p.face, p.up));
				Blob (bark, level, [&](const VECTOR3 &u) {
					return p.c + side * (u.x * p.s * 0.75) + p.up * (u.y * p.s) + p.face * (u.z * p.s * 0.13);
				}, 1.0, 1.5);
				if (p.depth >= 2) continue;
				int kids = 1 + (int)(r.U () * 2.0);
				for (int k = 0; k < kids; k++) {
					double a = r.R (-1.0, 1.0);
					VECTOR3 dir = unit (p.up * std::cos (a) + side * std::sin (a));
					VECTOR3 up = unit (dir + _V(0, 0.4, 0));
					double t = r.R (-0.8, 0.8);
					VECTOR3 face = unit (crossp (up, side * std::cos (t) + p.face * std::sin (t)));
					double s = p.s * r.R (0.75, 0.95);
					todo.push_back ({ p.c + dir * (p.s * 0.95) + up * (s * 0.8), up, face, s, p.depth + 1 });
				}
			}
			} break;
		default: {
			// shrubs: stems from the ground, branching, foliage at their ends
			// (desert bushes: creosote sparse and upright, sagebrush low and round)
			bool creo = sp == Species::CREOSOTE, sage = sp == Species::SAGEBRUSH;
			Grower g{ r, 2, sp == Species::SHRUB_SPREAD || sage ? 1.0 : creo ? 0.5 : 0.7, sp == Species::SHRUB_TALL || creo ? 1.0 : 0.3, 0.6, creo ? 3 : 2 };
			int stems = creo ? 8 + (int)(r.U () * 5) : 4 + (int)(r.U () * 4);
			for (int k = 0; k < stems; k++) {
				double az = r.U () * TWOPI;
				double el = sp == Species::SHRUB_SPREAD ? r.R (0.3, 0.7) : sp == Species::SHRUB_TALL || creo ? r.R (1.1, 1.4) : r.R (0.6, 1.2);
				VECTOR3 d = unit (_V(std::cos (az) * std::cos (el), std::sin (el), std::sin (az) * std::cos (el)));
				g.Grow (_V(r.R (-0.1, 0.1), 0.0, r.R (-0.1, 0.1)), d, height * r.R (0.55, 0.75), height * 0.02, 1);
			}
			BranchMesh (bark, g.branches, height * 0.006, lod);
			Foliage (leaves, r, g.leafPoints, height * (creo ? 0.25 : 0.35), creo ? 1 : 2, _V(0, height * 0.45, 0), height * 0.6);
			} break;
		}
	}

	// every k-th foliage card (Card: 8 vertices, 12 indices), larger by
	// sqrt(k) about its centre: about the same cover with fewer triangles
	Builder ThinCards (const Builder &in, int k)
	{
		Builder out;
		size_t n = in.v.size() / 8;
		float s = (float)std::sqrt ((double)k);
		for (size_t c = 0; c < n; c += k) {
			uint16_t base = (uint16_t)out.v.size();
			float cx = 0, cy = 0, cz = 0;
			for (int j = 0; j < 4; j++) cx += in.v[c*8+j].x, cy += in.v[c*8+j].y, cz += in.v[c*8+j].z;
			cx /= 4, cy /= 4, cz /= 4;
			for (int j = 0; j < 8; j++) {
				NTVERTEX v = in.v[c*8+j];
				v.x = cx + (v.x - cx) * s, v.y = cy + (v.y - cy) * s, v.z = cz + (v.z - cz) * s;
				out.v.push_back (v);
			}
			for (int j = 0; j < 12; j++) out.i.push_back (base + (in.i[c*12+j] - (uint16_t)(c*8)));
		}
		return out;
	}

	// ------------------------------------------------------------------
	// impostors: plants far away as a picture on a quad

	/// CPU copy of a texture (sRGB RGBA8)
	struct CpuTex { uint32_t w = 0, h = 0; std::vector<uint8_t> px; };

	/// A plant seen from IMP_VIEWS directions around it (side frames of an
	/// atlas, IMP_FW x IMP_FH texels each, in a row) and from above (a
	/// frame of IMP_TOP texels below them): colour with coverage, and
	/// normals in each frame's tangent frame (x along +u, y along +v)
	const int IMP_N = 512, IMP_FW = 64, IMP_FH = 128, IMP_TOP = 128, IMP_VIEWS = 8;
	struct Impostor {
		Canvas albedo{ IMP_N };
		std::vector<uint8_t> normal;
		double w = 0, y0 = 0, y1 = 0; // side quads: width, bottom, top [m]
		double r = 0, yTop = 0;       // top quad: half size, height [m]
	};
	struct BakeSrc { const Builder *b; const CpuTex *tex; bool foliage; };

	Impostor BakeImpostor (const std::vector<BakeSrc> &src)
	{
		Impostor imp;
		imp.normal.assign ((size_t)IMP_N * IMP_N * 4, 0);
		for (size_t i = 0; i < (size_t)IMP_N * IMP_N; i++)
			imp.normal[i*4] = 128, imp.normal[i*4+1] = 128, imp.normal[i*4+2] = 255, imp.normal[i*4+3] = 255;
		double rmax = 0.0, y0 = 1e9, y1 = -1e9, ysum = 0.0;
		size_t nf = 0;
		for (auto &s : src)
			for (auto &v : s.b->v) {
				rmax = std::max (rmax, std::hypot ((double)v.x, (double)v.z));
				y0 = std::min (y0, (double)v.y), y1 = std::max (y1, (double)v.y);
				if (s.foliage) ysum += v.y, nf++;
			}
		// (frames 1:2: square texels)
		double W = std::max (2.0 * rmax, (y1 - y0) * 0.5), H = 2.0 * W;
		imp.w = W, imp.y0 = y0, imp.y1 = y0 + H, imp.r = std::max (rmax, 0.1);
		imp.yTop = nf ? ysum / nf : y0 + 0.6 * (y1 - y0);
		std::vector<float> depth ((size_t)IMP_N * IMP_N, -1e30f);

		for (int view = 0; view <= IMP_VIEWS; view++) {
			VECTOR3 R, U, V;
			double fx0, fy0, fw, fh, xmin, xr, ymax, yr;
			if (view < IMP_VIEWS) {
				double th = view * TWOPI / IMP_VIEWS;
				V = _V(std::sin (th), 0, std::cos (th)), U = _V(0, 1, 0), R = crossp (U, V);
				fx0 = view * IMP_FW, fy0 = 0, fw = IMP_FW, fh = IMP_FH;
				xmin = -0.5 * W, xr = W, ymax = imp.y1, yr = H;
			} else { // from above: u along +x, v along +z
				V = _V(0, 1, 0), R = _V(1, 0, 0), U = _V(0, 0, -1);
				fx0 = 0, fy0 = IMP_FH, fw = fh = IMP_TOP;
				xmin = -imp.r, xr = 2.0 * imp.r, ymax = imp.r, yr = 2.0 * imp.r;
			}
			for (auto &s : src) {
				const Builder &b = *s.b;
				for (size_t t = 0; t + 2 < b.i.size(); t += 3) {
					const NTVERTEX *v[3] = { &b.v[b.i[t]], &b.v[b.i[t+1]], &b.v[b.i[t+2]] };
					double X[3], Y[3], D[3];
					for (int k = 0; k < 3; k++) {
						VECTOR3 p = _V(v[k]->x, v[k]->y, v[k]->z);
						X[k] = fx0 + (dotp (p, R) - xmin) / xr * fw;
						Y[k] = fy0 + (ymax - dotp (p, U)) / yr * fh;
						D[k] = dotp (p, V);
					}
					double area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
					if (std::fabs (area) < 1e-12) continue;
					int x0 = std::max ((int)std::floor (std::min ({ X[0], X[1], X[2] })), (int)fx0);
					int x1 = std::min ((int)std::ceil (std::max ({ X[0], X[1], X[2] })), (int)(fx0 + fw) - 1);
					int yy0 = std::max ((int)std::floor (std::min ({ Y[0], Y[1], Y[2] })), (int)fy0);
					int yy1 = std::min ((int)std::ceil (std::max ({ Y[0], Y[1], Y[2] })), (int)(fy0 + fh) - 1);
					for (int y = yy0; y <= yy1; y++)
						for (int x = x0; x <= x1; x++) {
							double px = x + 0.5, py = y + 0.5;
							double w0 = ((X[1] - px) * (Y[2] - py) - (X[2] - px) * (Y[1] - py)) / area;
							double w1 = ((X[2] - px) * (Y[0] - py) - (X[0] - px) * (Y[2] - py)) / area;
							double w2 = 1.0 - w0 - w1;
							if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
							double d = w0 * D[0] + w1 * D[1] + w2 * D[2];
							size_t idx = (size_t)y * IMP_N + x;
							if (d <= depth[idx]) continue;
							double u = w0 * v[0]->tu + w1 * v[1]->tu + w2 * v[2]->tu;
							double vv = w0 * v[0]->tv + w1 * v[1]->tv + w2 * v[2]->tv;
							const CpuTex &tx = *s.tex;
							int tu = (((int)std::floor (u * tx.w)) % (int)tx.w + (int)tx.w) % (int)tx.w;
							int tv = (((int)std::floor (vv * tx.h)) % (int)tx.h + (int)tx.h) % (int)tx.h;
							const uint8_t *c = &tx.px[((size_t)tv * tx.w + tu) * 4];
							if (s.foliage && c[3] < 128) continue;
							depth[idx] = (float)d;
							VECTOR3 n = _V(w0 * v[0]->nx + w1 * v[1]->nx + w2 * v[2]->nx,
								w0 * v[0]->ny + w1 * v[1]->ny + w2 * v[2]->ny, w0 * v[0]->nz + w1 * v[1]->nz + w2 * v[2]->nz);
							VECTOR3 nt = _V(dotp (n, R), -dotp (n, U), dotp (n, V));
							if (nt.z < 0.0) nt = -nt; // (double-sided: towards the viewer)
							nt = unit (nt);
							// (the crown's inside darker: light from outside reaches it less)
							VECTOR3 p = _V(w0 * v[0]->x + w1 * v[1]->x + w2 * v[2]->x, 0.0, w0 * v[0]->z + w1 * v[1]->z + w2 * v[2]->z);
							double ao = s.foliage ? 0.55 + 0.45 * std::clamp (length (p) / (0.8 * imp.r), 0.0, 1.0) : 1.0;
							imp.albedo.col[idx] = _V(std::pow (c[0] / 255.0, 2.2), std::pow (c[1] / 255.0, 2.2), std::pow (c[2] / 255.0, 2.2)) * ao;
							imp.albedo.a[idx] = 1.0f;
							uint8_t *q = &imp.normal[idx * 4];
							q[0] = (uint8_t)std::lround ((nt.x * 0.5 + 0.5) * 255.0);
							q[1] = (uint8_t)std::lround ((nt.y * 0.5 + 0.5) * 255.0);
							q[2] = (uint8_t)std::lround ((nt.z * 0.5 + 0.5) * 255.0);
						}
				}
			}
		}
		return imp;
	}

	/// The impostor meshes' geometry of view k: a quad through the plant's
	/// axis facing that direction, and the top quad (each a part of its own)
	Builder ImpostorQuads (const Impostor &imp, int k)
	{
		Builder b;
		double th = k * TWOPI / IMP_VIEWS;
		VECTOR3 V = _V(std::sin (th), 0, std::cos (th)), U = _V(0, 1, 0), R = crossp (U, V);
		double u0 = (double)k * IMP_FW / IMP_N, u1 = (double)(k + 1) * IMP_FW / IMP_N, v1 = (double)IMP_FH / IMP_N;
		uint16_t a = b.Add (R * (-0.5 * imp.w) + U * imp.y0, V, u0, v1), c = b.Add (R * (0.5 * imp.w) + U * imp.y0, V, u1, v1);
		uint16_t d = b.Add (R * (0.5 * imp.w) + U * imp.y1, V, u1, 0.0), e = b.Add (R * (-0.5 * imp.w) + U * imp.y1, V, u0, 0.0);
		b.Tri (a, c, d, V), b.Tri (a, d, e, V);
		double tv0 = (double)IMP_FH / IMP_N, tv1 = (double)(IMP_FH + IMP_TOP) / IMP_N, tu1 = (double)IMP_TOP / IMP_N, r = imp.r;
		uint16_t t0 = b.Add (_V(-r, imp.yTop, -r), U, 0.0, tv0), t1 = b.Add (_V(r, imp.yTop, -r), U, tu1, tv0);
		uint16_t t2 = b.Add (_V(r, imp.yTop, r), U, tu1, tv1), t3 = b.Add (_V(-r, imp.yTop, r), U, 0.0, tv1);
		b.Tri (t0, t1, t2, U), b.Tri (t0, t2, t3, U);
		return b;
	}

	void Extent (const Builder &b, double &radius, double &height)
	{
		radius = 0.0, height = 0.0;
		for (auto &v : b.v) {
			radius = std::max (radius, std::hypot ((double)v.x, (double)v.z));
			height = std::max (height, (double)v.y);
		}
	}

} // namespace

namespace vkscatter {

// ======================================================================
// Library

Library::Library (vkc::Context *c, TextureManager *tm): ctx (c), texmgr (tm) {}

Library::~Library ()
{
	assets.clear ();
	for (VkTexture *t : textures) texmgr->Release (t);
}

VkTexture *Library::Texture (uint32_t w, uint32_t h, const std::vector<uint8_t> &rgba)
{
	VkTexture *t = texmgr->CreateRGBA (w, h, rgba.data());
	textures.push_back (t);
	return t;
}

bool Library::LoadRock (const std::string &stem, double rad, double h)
{
	for (const Asset &a : assets) if (a.name == stem) return true;
	Asset a;
	a.kind = Kind::ROCK, a.name = stem, a.radius = rad, a.height = h;
	for (int k = 0; k < 3; k++) {
		std::string m = stem + "_lod" + std::to_string (k);
		MESHHANDLE hm = oapiLoadMeshGlobal (m.c_str());
		if (!hm) { oapiWriteLogError ("Scatter: cannot load mesh %s", m.c_str()); break; }
		auto vm = std::make_unique<VkMesh> (ctx, texmgr, hm);
		if (k == 0) a.mesh = std::move (vm);
		else a.lods.push_back (std::move (vm));
	}
	if (!a.mesh) return false;
	assets.push_back (std::move (a));
	return true;
}

bool Library::LoadRocks (const char *body)
{
	// Config/Scatter/<body>_rocks.cfg: ROCK <mesh stem> <radius> <height> <triangles>...
	std::string path = std::string ("Config/Scatter/") + body + "_rocks.cfg";
	FILE *f = fopen (path.c_str(), "r");
	if (!f) return false;
	char line[512];
	int n = 0;
	while (fgets (line, sizeof(line), f)) {
		char stem[256];
		double rad, h;
		if (sscanf (line, "ROCK %255s %lf %lf", stem, &rad, &h) != 3) continue;
		if (LoadRock (stem, rad, h)) n++;
	}
	fclose (f);
	oapiWriteLogV ("Scatter: %d rocks of %s from %s", n, body, path.c_str());
	return n > 0;
}

void Library::Generate (const char *body, RockStyle style, bool vegetation, bool ownRocks)
{
	auto part = [&](Builder &b, VkTexture *tex, float roughness, float alphaTest = 0.0f) {
		VkMesh::Part p;
		p.vtx = std::move (b.v);
		p.idx = std::move (b.i);
		p.tex = tex;
		p.pbr.roughness = roughness;
		p.alphaTest = alphaTest;
		return p;
	};
	auto add = [&](Kind kind, const std::string &name, std::vector<VkMesh::Part> &&parts, double radius, double height) {
		for (auto &p : parts)
			if (p.vtx.size() > 65535) oapiWriteLogError ("Scatter: %s has %zu vertices in a part (16-bit indices)", name.c_str(), p.vtx.size());
		Asset a;
		a.kind = kind, a.name = name, a.radius = radius, a.height = height;
		a.mesh = std::make_unique<VkMesh> (ctx, texmgr, parts);
		assets.push_back (std::move (a));
	};
	auto cardTexture = [&](uint32_t seed, LeafShape shape, const VECTOR3 &colour, CpuTex *cpu) {
		Canvas cv (256);
		Rng r (seed);
		PaintSpray (cv, r, shape, colour);
		MipChain m = CardMips (cv);
		cpu->w = m.w, cpu->h = m.h;
		cpu->px.assign (m.data.begin(), m.data.begin() + (size_t)m.w * m.h * 4);
		VkTexture *t = texmgr->CreateRGBAMips (m.w, m.h, m.nMip, m.data.data(), m.ofs, m.data.size());
		textures.push_back (t);
		return t;
	};

	// rocks: made with Blender, or the types of the body, pebbles to
	// boulders, each with its own texture
	bool loaded = !ownRocks || LoadRocks (body); // (ownRocks: false when the core lists them, LoadRock)
	struct RockSpec { const RockShape *shape; RockTex tex; double size; bool split; };
	std::vector<RockSpec> rocks;
	if (style == RockStyle::GRANITE) {
		for (double sz : { 0.3, 0.5, 0.8, 1.2, 1.6, 2.2, 3.0, 4.0 }) rocks.push_back ({ &GRANITE, RockTex::GRANITE, sz, false });
	} else if (style == RockStyle::REGOLITH) {
		for (double sz : { 0.15, 0.3, 0.5 }) rocks.push_back ({ &MOON_FRAGMENT, RockTex::MOON, sz, false });
		for (double sz : { 0.9, 1.6, 2.6, 4.0 }) rocks.push_back ({ &MOON_BLOCK, RockTex::MOON, sz, false });
		for (double sz : { 1.2, 2.2, 3.5 }) rocks.push_back ({ &MOON_BRECCIA, RockTex::MOON_BRECCIA, sz, false });
		rocks.push_back ({ &MOON_BLOCK, RockTex::MOON_BRECCIA, 6.0, true });
	} else {
		for (double sz : { 0.12, 0.25, 0.4 }) rocks.push_back ({ &MARS_FLOAT, RockTex::MARS_SANDSTONE, sz, false });
		for (double sz : { 0.6, 1.1, 1.8 }) rocks.push_back ({ &MARS_VENTIFACT, RockTex::MARS_VENTIFACT, sz, false });
		for (double sz : { 0.8, 1.5, 2.5 }) rocks.push_back ({ &MARS_SLAB, RockTex::MARS_SANDSTONE, sz, false });
		for (double sz : { 0.7, 1.4, 2.4 }) rocks.push_back ({ &MARS_BASALT, RockTex::MARS_BASALT, sz, false });
	}
	for (size_t k = 0; k < rocks.size() && !loaded; k++) {
		const RockSpec &rs = rocks[k];
		Tex t = TypedRockTexture (rs.tex, 100 + (uint32_t)k);
		VkTexture *tex = Texture (t.w, t.h, t.px);
		uint32_t sd = 1000 + (uint32_t)k * 17;
		Builder b = rs.split ? SplitBoulder (*rs.shape, sd, rs.size) : Rock (*rs.shape, sd, rs.size);
		double rad, h;
		Extent (b, rad, h);
		std::vector<VkMesh::Part> parts;
		parts.push_back (part (b, tex, 0.9f));
		add (Kind::ROCK, "rock" + std::to_string (k), std::move (parts), rad, h);
	}
	if (!vegetation) return;

	Tex gt = GrassTexture (7);
	VkTexture *grassTex = Texture (gt.w, gt.h, gt.px);
	for (int k = 0; k < 6; k++) {
		Builder b = Grass (2000 + k * 31);
		double rad, h;
		Extent (b, rad, h);
		std::vector<VkMesh::Part> parts;
		parts.push_back (part (b, grassTex, 0.7f));
		add (Kind::GRASS, "grass" + std::to_string (k), std::move (parts), rad, h);
		// coarser: two and one segments per blade, then half the blades
		const int lseg[3] = { 2, 1, 1 }, levery[3] = { 1, 1, 2 };
		for (int l = 0; l < 3; l++) {
			Builder lb = Grass (2000 + k * 31, lseg[l], levery[l]);
			std::vector<VkMesh::Part> lp;
			lp.push_back (part (lb, grassTex, 0.7f));
			assets.back().lods.push_back (std::make_unique<VkMesh> (ctx, texmgr, lp));
		}
	}

	Tex bt = BarkTexture (17, _V(0.10, 0.07, 0.045), false);
	VkTexture *barkTex = Texture (bt.w, bt.h, bt.px);
	Tex wb = BarkTexture (29, _V(0.62, 0.60, 0.55), true);
	VkTexture *birchTex = Texture (wb.w, wb.h, wb.px);
	auto cpuOf = [](const Tex &t) { CpuTex c; c.w = t.w, c.h = t.h, c.px = t.px; return c; };
	CpuTex barkCpu = cpuOf (bt), birchCpu = cpuOf (wb);
	struct Spec { Species sp; Kind kind; const char *name; double height; LeafShape shape; VECTOR3 colour; };
	const Spec specs[] = {
		{ Species::SHRUB_ROUND,  Kind::SHRUB, "shrub_round",  1.4, LeafShape::SMALL,  _V(0.06, 0.10, 0.035) },
		{ Species::SHRUB_SPREAD, Kind::SHRUB, "shrub_spread", 1.1, LeafShape::BROAD,  _V(0.07, 0.11, 0.03) },
		{ Species::SHRUB_TALL,   Kind::SHRUB, "shrub_tall",   2.2, LeafShape::SMALL,  _V(0.05, 0.09, 0.03) },
		{ Species::SPRUCE,       Kind::CONIFER, "spruce",     16.0, LeafShape::NEEDLE, _V(0.025, 0.05, 0.025) },
		{ Species::FIR,          Kind::CONIFER, "fir",        12.0, LeafShape::NEEDLE, _V(0.03, 0.06, 0.03) },
		{ Species::PINE,         Kind::CONIFER, "pine",       14.0, LeafShape::NEEDLE, _V(0.045, 0.07, 0.03) },
		{ Species::OAK,          Kind::BROADLEAF, "oak",      11.0, LeafShape::BROAD, _V(0.06, 0.10, 0.03) },
		{ Species::POPLAR,       Kind::BROADLEAF, "poplar",   16.0, LeafShape::SMALL, _V(0.07, 0.12, 0.035) },
		{ Species::BIRCH,        Kind::BROADLEAF, "birch",    10.0, LeafShape::SMALL, _V(0.09, 0.14, 0.04) },
		{ Species::ACACIA,       Kind::BROADLEAF, "acacia",    7.0, LeafShape::SMALL, _V(0.08, 0.10, 0.035) },
		{ Species::CREOSOTE,     Kind::SHRUB, "creosote",     1.5, LeafShape::SMALL,  _V(0.07, 0.08, 0.03) },
		{ Species::SAGEBRUSH,    Kind::SHRUB, "sagebrush",    0.9, LeafShape::SMALL,  _V(0.15, 0.17, 0.12) },
		{ Species::SAGUARO,      Kind::CONIFER, "saguaro",    9.0, LeafShape::SMALL,  _V(0.10, 0.15, 0.08) },
		{ Species::BARREL,       Kind::SHRUB, "barrel",       0.8, LeafShape::SMALL,  _V(0.12, 0.17, 0.07) },
		{ Species::PRICKLY_PEAR, Kind::SHRUB, "pricklypear",  1.2, LeafShape::SMALL,  _V(0.12, 0.18, 0.06) },
	};
	// cactus skins: ribbed columns, pads
	Tex ct = CactusTexture (41, _V(0.10, 0.15, 0.08), true);
	VkTexture *cactusTex = Texture (ct.w, ct.h, ct.px);
	Tex pt = CactusTexture (43, _V(0.12, 0.18, 0.06), false);
	VkTexture *padTex = Texture (pt.w, pt.h, pt.px);
	CpuTex cactusCpu = cpuOf (ct);
	uint32_t seed = 5000;
	for (const Spec &s : specs) {
		bool cactus = IsCactus (s.sp);
		CpuTex leafCpu;
		VkTexture *leafTex = cactus ? nullptr : cardTexture (seed + 1, s.shape, s.colour, &leafCpu);
		VkTexture *bt = s.sp == Species::BIRCH ? birchTex : s.sp == Species::PRICKLY_PEAR ? padTex : cactus ? cactusTex : barkTex;
		Builder bark, leaves;
		Plant (s.sp, seed, s.height, bark, leaves);
		double rad, h, rb, hb;
		Extent (leaves, rad, h);
		Extent (bark, rb, hb);
		double crownLo = 1e9, crownHi = -1e9, crownR = 0.0; // (the crown: foliage, or the whole plant)
		for (auto &v : (leaves.v.empty() ? bark : leaves).v)
			crownLo = std::min (crownLo, (double)v.y), crownHi = std::max (crownHi, (double)v.y),
			crownR = std::max (crownR, std::hypot ((double)v.x, (double)v.z));
		// far away: impostors (trees and saguaros)
		std::unique_ptr<VkMesh> imp0;
		if (s.kind == Kind::CONIFER || s.kind == Kind::BROADLEAF) {
			const CpuTex *bc = s.sp == Species::BIRCH ? &birchCpu : cactus ? &cactusCpu : &barkCpu;
			Impostor imp = BakeImpostor ({ { &bark, bc, false }, { &leaves, &leafCpu, true } });
			MipChain m = CardMips (imp.albedo);
			VkTexture *it = texmgr->CreateRGBAMips (m.w, m.h, m.nMip, m.data.data(), m.ofs, m.data.size());
			texmgr->SetNormalMap (it, IMP_N, IMP_N, imp.normal.data());
			textures.push_back (it);
			Builder q = ImpostorQuads (imp, 0); // (the shader turns it to the other views)
			std::vector<VkMesh::Part> ip;
			ip.push_back (part (q, it, cactus ? 0.6f : 0.8f, 0.5f));
			imp0 = std::make_unique<VkMesh> (ctx, texmgr, ip);
		}
		std::vector<VkMesh::Part> parts;
		parts.push_back (part (bark, bt, cactus ? 0.6f : 0.9f));
		if (!leaves.i.empty()) parts.push_back (part (leaves, leafTex, 0.75f, 0.5f));
		if (leaves.v.empty()) rad = rb, h = hb;
		add (s.kind, s.name, std::move (parts), std::max (rad, rb), std::max (h, hb));
		assets.back().impostor = std::move (imp0);
		assets.back().crownLo = std::max (crownLo, 0.0), assets.back().crownHi = crownHi, assets.back().crownR = crownR;
		// coarser levels of detail: fewer branches, a third, a tenth and a
		// fortieth of the cards
		for (int lod = 1; lod <= 3; lod++) {
			Builder lb, ll;
			Plant (s.sp, seed, s.height, lb, ll, cactus ? lod : std::min (lod, 2));
			Builder thin = ThinCards (ll, lod == 1 ? 3 : lod == 2 ? 10 : 40);
			if (lod == 3 && !cactus) { // (the trunk alone, four-sided)
				lb = Builder ();
				double r0 = s.height * 0.02;
				Cylinder (lb, _V(0, -0.3, 0), _V(0, s.height * (s.kind == Kind::CONIFER ? 0.9 : 0.45), 0), r0, r0 * 0.4, 4, s.height / 3.0);
			}
			std::vector<VkMesh::Part> lp;
			if (!lb.i.empty()) lp.push_back (part (lb, bt, cactus ? 0.6f : 0.9f));
			if (!thin.i.empty()) lp.push_back (part (thin, leafTex, 0.75f, 0.5f));
			assets.back().lods.push_back (std::make_unique<VkMesh> (ctx, texmgr, lp));
		}
		seed += 97;
	}
}

// ======================================================================
// Gallery

bool Gallery::Enabled ()
{
	const char *e = getenv ("ORBITER_VK_SCATTER_GALLERY");
	return e && *e && *e != '0';
}

Gallery::Gallery (Scene *s, OBJHANDLE hp): scene (s), hPlanet (hp)
{
	char name[64];
	oapiGetObjectName (hPlanet, name, 64);
	RockStyle style = !strcmp (name, "Mars") ? RockStyle::MARS : !strcmp (name, "Earth") ? RockStyle::GRANITE : RockStyle::REGOLITH;
	lib = std::make_unique<Library> (scene->Client()->Ctx(), scene->Client()->TexMgr());
	lib->Generate (name, style, !strcmp (name, "Earth"));
}

Gallery::~Gallery () {}

void Gallery::Place ()
{
	// ORBITER_VK_SCATTER_GALLERY=<kind> (rock, grass, shrub, conifer,
	// broadleaf): the assets of that kind in arcs in front of the camera,
	// towards the focus vessel, the small ones nearer; otherwise rows of all
	// kinds north of the focus vessel
	OBJHANDLE hv = oapiGetFocusObject ();
	if (!hv) return;
	VECTOR3 gv, pp;
	MATRIX3 prot;
	oapiGetGlobalPos (hv, &gv);
	oapiGetGlobalPos (hPlanet, &pp);
	oapiGetRotationMatrix (hPlanet, &prot);
	double R = oapiGetSize (hPlanet);
	Rng rng (99);
	auto put = [&](const Asset *a, double lat, double lng) {
		double e = oapiSurfaceElevation (hPlanet, lng, lat);
		VECTOR3 up = _V(std::cos (lat) * std::cos (lng), std::sin (lat), std::cos (lat) * std::sin (lng));
		VECTOR3 ea = _V(-std::sin (lng), 0.0, std::cos (lng));
		VECTOR3 no = crossp (ea, up); // (north: the frame east, up, north)
		double yaw = rng.U () * TWOPI, c = std::cos (yaw), s = std::sin (yaw);
		VECTOR3 x = ea * c + no * s, z = no * c - ea * s;
		items.push_back ({ a, up * (R + e), _M(x.x, up.x, z.x, x.y, up.y, z.y, x.z, up.z, z.z) });
	};
	auto latlng = [](const VECTOR3 &p, double &lat, double &lng) { lat = std::asin (p.y / length (p)), lng = std::atan2 (p.z, p.x); };

	const char *sel = getenv ("ORBITER_VK_SCATTER_GALLERY");
	const char *names[] = { "rock", "grass", "shrub", "conifer", "broadleaf" };
	int kindSel = -1, itemSel = -1;
	for (int k = 0; k < 5; k++) {
		size_t l = strlen (names[k]);
		if (!strncmp (sel, names[k], l) && (sel[l] == '\0' || sel[l] == ':')) {
			kindSel = k;
			if (sel[l] == ':') itemSel = atoi (sel + l + 1); // (one asset, framed)
		}
	}
	if (kindSel >= 0) {
		VECTOR3 cam = tmul (prot, scene->CameraPos () - pp), ves = tmul (prot, gv - pp);
		double clat, clng;
		latlng (cam, clat, clng);
		VECTOR3 up = unit (cam);
		VECTOR3 fwd = ves - cam;
		fwd = unit (fwd - up * dotp (fwd, up));
		VECTOR3 right = crossp (up, fwd);
		std::vector<const Asset*> row;
		for (const Asset &a : lib->Assets()) if ((int)a.kind == kindSel) row.push_back (&a);
		std::sort (row.begin(), row.end(), [](const Asset *a, const Asset *b) { return a->radius < b->radius; });
		if (itemSel >= 0) {
			if (itemSel < (int)row.size()) {
				const Asset *a = row[itemSel];
				double d = std::max (2.6 * a->radius, 1.8 * a->height) + 1.0;
				double lat, lng;
				latlng (cam + fwd * d, lat, lng);
				put (a, lat, lng);
			}
			placed = true;
			return;
		}
		// arcs of up to four, each wide enough for its largest asset
		for (size_t i0 = 0; i0 < row.size(); i0 += 4) {
			size_t i1 = std::min (row.size(), i0 + 4);
			double w = 0.0, hmax = 0.0;
			for (size_t i = i0; i < i1; i++) w += 2.0 * row[i]->radius + 0.6, hmax = std::max (hmax, row[i]->height);
			double d = std::max ({ w / 0.75, hmax * 2.5, 3.0 }) + (i0 > 0 ? 3.0 : 0.0) * (double)(i0 / 4);
			double x = -0.5 * w;
			for (size_t i = i0; i < i1; i++) {
				x += row[i]->radius + 0.3;
				VECTOR3 p = cam + fwd * d + right * x;
				double lat, lng;
				latlng (p, lat, lng);
				put (row[i], lat, lng);
				x += row[i]->radius + 0.3;
			}
		}
		placed = true;
		return;
	}

	VECTOR3 lv = tmul (prot, gv - pp);
	double lat0, lng0;
	latlng (lv, lat0, lng0);
	double north = 12.0;
	const Kind kinds[] = { Kind::ROCK, Kind::GRASS, Kind::SHRUB, Kind::CONIFER, Kind::BROADLEAF };
	for (Kind kind : kinds) {
		std::vector<const Asset*> row;
		double rmax = 0.0, width = 0.0;
		for (const Asset &a : lib->Assets())
			if (a.kind == kind) row.push_back (&a), rmax = std::max (rmax, a.radius), width += 2.0 * a.radius + 1.5;
		if (row.empty()) continue;
		north += rmax + 1.0;
		double east = -0.5 * width;
		for (const Asset *a : row) {
			east += a->radius + 0.75;
			put (a, lat0 + north / R, lng0 + east / (R * std::cos (lat0)));
			east += a->radius + 0.75;
		}
		north += rmax + 1.0;
	}
	placed = true;
}

void Gallery::Render (RenderPass pass)
{
	if (!placed) Place ();
	VECTOR3 pp;
	MATRIX3 prot;
	oapiGetGlobalPos (hPlanet, &pp);
	oapiGetRotationMatrix (hPlanet, &prot);
	for (const Item &it : items) {
		VECTOR3 gpos = pp + mul (prot, it.pos);
		scene->Sunlight (gpos);
		double px = scene->PixelSize (std::max (it.asset->radius, it.asset->height), length (gpos - scene->CameraPos ()));
		scene->DrawMesh (it.asset->ForPixels (px), scene->ModelView (gpos, mul (prot, it.rot)), pass);
	}
}

void Gallery::RenderShadow (ShadowMap &sm)
{
	if (!placed) Place ();
	VECTOR3 pp;
	MATRIX3 prot;
	oapiGetGlobalPos (hPlanet, &pp);
	oapiGetRotationMatrix (hPlanet, &prot);
	for (const Item &it : items)
		sm.DrawMesh (it.asset->mesh.get(), Mat4::FromRotTrans (mul (prot, it.rot), pp + mul (prot, it.pos) - sm.Centre()));
}

} // namespace vkscatter

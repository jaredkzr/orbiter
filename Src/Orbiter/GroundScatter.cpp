// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "GroundScatter.h"
#include "Orbiter.h"
#include "Config.h"
#include "Planet.h"
#include "Base.h"
#include "Vessel.h"
#include "Psys.h"
#include "Mesh.h"
#include "elevmgr.h"
#include "ZTreeMgr.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

extern Orbiter *g_pOrbiter;
extern PlanetarySystem *g_psys;

namespace scatter {

bool GroundScatter::clientSupport = false;

namespace {

	const size_t MAX_CELLS = 16384; // cached cells (all layers)
	const int COVER_LEVEL = 14;     // finest surface tile level read for the ground cover (about 40 m per texel on Earth)
	const size_t COVER_TILES = 12;  // cached tiles per layer

	inline uint64_t Mix (uint64_t x)
	{
		x += 0x9e3779b97f4a7c15ull;
		x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
		x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
		return x ^ (x >> 31);
	}
	inline uint64_t Hash (uint64_t a, uint64_t b) { return Mix (a ^ Mix (b)); }

	/// uniform random numbers from a hash
	struct Rnd {
		uint64_t s;
		explicit Rnd (uint64_t seed): s (seed) {}
		double operator() () { s = Mix (s); return (double)(s >> 11) * (1.0 / 9007199254740992.0); }
	};

	inline double SmoothStep (double a, double b, double x)
	{
		double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	inline Vector Dir (double lat, double lng)
	{
		return Vector (cos (lat) * cos (lng), sin (lat), cos (lat) * sin (lng));
	}

	inline uint64_t CellKey (int layer, int row, int col)
	{
		return ((uint64_t)layer << 56) | ((uint64_t)(uint32_t)row << 28) | (uint64_t)(uint32_t)col;
	}

	/// DXT1 (BC1) blocks of a DDS image -> RGBA8 (r in the low byte)
	bool DecodeDDS (const uint8_t *data, size_t n, std::vector<uint32_t> &px, int &w, int &h)
	{
		if (n < 128 || memcmp (data, "DDS ", 4) || memcmp (data + 84, "DXT1", 4)) return false;
		uint32_t hh, ww;
		memcpy (&hh, data + 12, 4);
		memcpy (&ww, data + 16, 4);
		w = (int)ww, h = (int)hh;
		if (w < 4 || h < 4 || n < 128 + (size_t)(w / 4) * (h / 4) * 8) return false;
		px.assign ((size_t)w * h, 0);
		const uint8_t *b = data + 128;
		auto rgb = [](uint16_t c) {
			uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
			return Vector ((r << 3 | r >> 2), (g << 2 | g >> 4), (bl << 3 | bl >> 2));
		};
		auto pack = [](const Vector &c, uint32_t a) {
			return (uint32_t)c.x | (uint32_t)c.y << 8 | (uint32_t)c.z << 16 | a << 24;
		};
		for (int by = 0; by < h / 4; by++)
			for (int bx = 0; bx < w / 4; bx++, b += 8) {
				uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
				uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
				Vector e0 = rgb (c0), e1 = rgb (c1);
				uint32_t pal[4];
				pal[0] = pack (e0, 255), pal[1] = pack (e1, 255);
				if (c0 > c1) {
					pal[2] = pack ((e0 * 2.0 + e1) / 3.0, 255);
					pal[3] = pack ((e0 + e1 * 2.0) / 3.0, 255);
				} else {
					pal[2] = pack ((e0 + e1) * 0.5, 255);
					pal[3] = 0; // (transparent)
				}
				for (int k = 0; k < 16; k++)
					px[(size_t)(by * 4 + k / 4) * w + bx * 4 + k % 4] = pal[(bits >> (2 * k)) & 3];
			}
		return true;
	}

	/// a convex polytope scaled, rotated and moved (p' = R s p + pos)
	convex::Polytope Transform (const convex::Polytope &P, const Matrix &R, double s, const Vector &pos)
	{
		convex::Polytope Q = P;
		for (Vector &v : Q.v) v = mul (R, v * s) + pos;
		for (convex::Face &f : Q.f) {
			f.n = mul (R, f.n);
			f.d = dotp (f.n, Q.v[f.idx[0]]);
		}
		Q.centre = mul (R, P.centre * s) + pos;
		Q.radius = P.radius * s;
		Q.volume = P.volume * s * s * s;
		return Q;
	}

	/// a tapered trunk: octagonal frustum from below the ground to h
	convex::Polytope Trunk (double r, double h)
	{
		std::vector<Vector> pts;
		for (int i = 0; i < 8; i++) {
			double a = i * Pi / 4.0;
			pts.push_back (Vector (r * cos (a), -0.5, r * sin (a)));
			pts.push_back (Vector (0.5 * r * cos (a), h, 0.5 * r * sin (a)));
		}
		convex::Polytope P;
		convex::Build (pts, P);
		return P;
	}

} // namespace

// =======================================================================
// Cover

Cover::Cover (const Planet *p): planet (p), maxLevel (COVER_LEVEL)
{
	DWORD src = g_pOrbiter->Cfg()->CfgPRenderPrm.TileLoadFlags;
	const char *name[2] = { "Surf", "Mask" };
	ZTreeMgr::Layer lay[2] = { ZTreeMgr::LAYER_SURF, ZTreeMgr::LAYER_MASK };
	char cbuf[512];
	for (int i = 0; i < 2; i++) {
		if (src & 0x0002) {
			g_pOrbiter->Cfg()->PTexPath (cbuf, planet->Name());
			tree[i] = ZTreeMgr::CreateFromFile (cbuf, lay[i]);
		}
		if (src & 0x0001) {
			std::string f = std::string (planet->Name()) + "\\" + name[i];
			g_pOrbiter->Cfg()->PTexPath (cbuf, f.c_str());
			dir[i] = cbuf;
		}
	}
}

Cover::~Cover ()
{
	for (int i = 0; i < 2; i++) delete tree[i];
}

bool Cover::Load (int layer, int lvl, int ilat, int ilng, Tile &t)
{
	t.lvl = lvl, t.ilat = ilat, t.ilng = ilng;
	if (tree[layer]) {
		BYTE *buf = nullptr;
		DWORD n = tree[layer]->ReadData (lvl, ilat, ilng, &buf);
		if (n) {
			bool ok = DecodeDDS (buf, n, t.px, t.w, t.h);
			tree[layer]->ReleaseData (buf);
			if (ok) return true;
		}
	}
	if (!dir[layer].empty()) {
		char fname[64];
		snprintf (fname, sizeof(fname), "\\%02d\\%06d\\%06d.dds", lvl, ilat, ilng);
		std::string path = dir[layer] + fname;
		for (char &c : path) if (c == '\\') c = '/';
		std::ifstream ifs (path, std::ios::binary);
		if (ifs.good()) {
			std::vector<uint8_t> d ((std::istreambuf_iterator<char> (ifs)), std::istreambuf_iterator<char> ());
			if (DecodeDDS (d.data(), d.size(), t.px, t.w, t.h)) return true;
		}
	}
	return false;
}

const Cover::Tile *Cover::Get (int layer, double lat, double lng)
{
	if (!tree[layer] && dir[layer].empty()) return nullptr;
	// the finest tile up to maxLevel that has data
	for (int lvl = maxLevel; lvl >= 4; lvl--) {
		int nlat = 1 << (lvl - 4), nlng = 2 << (lvl - 4);
		int ilat = std::clamp ((int)((Pi05 - lat) / Pi * nlat), 0, nlat - 1);
		int ilng = std::clamp ((int)((lng + Pi) / Pi2 * nlng), 0, nlng - 1);
		for (Tile &t : cache[layer])
			if (t.lvl == lvl && t.ilat == ilat && t.ilng == ilng) { t.used = ++clock; return t.w ? &t : nullptr; }
		bool exists = tree[layer] ? tree[layer]->Idx (lvl, ilat, ilng) != (DWORD)-1 : true;
		if (!exists) continue;
		Tile t;
		if (!Load (layer, lvl, ilat, ilng, t)) {
			if (tree[layer]) continue;
			t.w = 0; // (no loose file either: remembered)
		}
		t.used = ++clock;
		if (cache[layer].size() >= COVER_TILES) {
			auto lru = std::min_element (cache[layer].begin(), cache[layer].end(), [](const Tile &a, const Tile &b) { return a.used < b.used; });
			*lru = std::move (t);
			return lru->w ? &*lru : nullptr;
		}
		cache[layer].push_back (std::move (t));
		if (cache[layer].back().w) return &cache[layer].back();
	}
	return nullptr;
}

bool Cover::Sample (double lat, double lng, Vector &rgb, double &water)
{
	const Tile *t = Get (0, lat, lng);
	if (!t) return false;
	auto texel = [&](const Tile *t) {
		int nlat = 1 << (t->lvl - 4), nlng = 2 << (t->lvl - 4);
		double u = (lng + Pi) / Pi2 * nlng - t->ilng;
		double v = (Pi05 - lat) / Pi * nlat - t->ilat;
		int x = std::clamp ((int)(u * t->w), 0, t->w - 1), y = std::clamp ((int)(v * t->h), 0, t->h - 1);
		return t->px[(size_t)y * t->w + x];
	};
	uint32_t c = texel (t);
	rgb = Vector ((c & 255) / 255.0, ((c >> 8) & 255) / 255.0, ((c >> 16) & 255) / 255.0);
	water = 0.0;
	if (const Tile *m = Get (1, lat, lng))
		water = (texel (m) >> 24) < 128 ? 1.0 : 0.0; // (mask alpha 0: water)
	return true;
}

// =======================================================================
// GroundScatter

GroundScatter::GroundScatter (const Planet *p): planet (p), R (p->Size())
{
	etile = std::make_unique<std::vector<ElevationTile>> (16);
	seed = 0x5ca7u;
	for (const char *c = planet->Name(); *c; c++) seed = seed * 31u + (uint8_t)*c;
	Load ();
}

GroundScatter::~GroundScatter () {}

void GroundScatter::LoadAssets (const std::string &file)
{
	std::ifstream ifs (g_pOrbiter->Cfg()->ConfigPathNoext (("Scatter/" + file).c_str()));
	if (!ifs.good()) {
		LOGOUT_WARN ("Ground scatter: cannot read Config/Scatter/%s", file.c_str());
		return;
	}
	std::string line;
	while (std::getline (ifs, line)) {
		size_t c = line.find (';');
		if (c != std::string::npos) line.erase (c);
		std::istringstream ss (line);
		std::string tok;
		if (!(ss >> tok)) continue;
		Asset a;
		if (tok == "ROCK") {
			a.kind = ROCK;
			if (!(ss >> a.name >> a.radius >> a.height)) continue;
			// collision hull: the convex hull of the coarsest level of detail
			std::string m = a.name + "_lod2";
			if (const Mesh *mesh = g_pOrbiter->LoadMeshGlobal (m.c_str())) {
				std::vector<Vector> pts;
				Mesh *mm = const_cast<Mesh*> (mesh);
				for (DWORD g = 0; g < mm->nGroup(); g++) {
					const GroupSpec *gs = mm->GetGroup (g);
					for (DWORD i = 0; i < gs->nVtx; i++) pts.push_back (Vector (gs->Vtx[i].x, gs->Vtx[i].y, gs->Vtx[i].z));
				}
				a.hasHull = convex::Build (pts, a.hull);
			}
		} else if (tok == "PLANT") {
			std::string kind;
			if (!(ss >> kind >> a.name >> a.height >> a.trunk)) continue;
			a.kind = kind == "TREE" ? TREE : kind == "SHRUB" ? SHRUB : GRASS;
			if (!(ss >> a.lat0 >> a.lat1)) a.lat0 = 0.0, a.lat1 = 90.0;
			a.radius = a.height * (a.kind == TREE ? 0.3 : 0.5);
			if (a.kind == TREE && a.trunk > 0.0) {
				a.hull = Trunk (a.trunk, 0.55 * a.height);
				a.hasHull = true;
			}
		} else if (tok == "ASSETS") {
			std::string f;
			if (ss >> f) LoadAssets (f);
			continue;
		} else if (tok == "LAYER") {
			Layer L;
			std::string kind, cov;
			int col;
			if (!(ss >> L.name >> kind >> L.minSize >> L.maxSize >> L.cell >> L.spacing >> L.density >> L.range >> col >> cov)) continue;
			L.kind = kind == "ROCK" ? ROCK : kind == "TREE" ? TREE : kind == "SHRUB" ? SHRUB : GRASS;
			L.collide = col != 0;
			L.cover = cov == "slope" ? Layer::SLOPE : cov == "bare" ? Layer::BARE : cov == "forest" ? Layer::FOREST :
				cov == "bush" ? Layer::BUSH : cov == "meadow" ? Layer::MEADOW : Layer::NONE;
			L.cell = std::max (L.cell, 1.0);
			L.spacing = std::clamp (L.spacing, 0.1, L.cell);
			layers.push_back (L);
			continue;
		} else continue;
		assets.push_back (std::move (a));
	}
}

void GroundScatter::Load ()
{
	if (!g_pOrbiter->Cfg()->CfgVisualPrm.bGroundScatter) return;
	if (g_pOrbiter->GetGraphicsClient() && !clientSupport) return;
	if (const char *ns = getenv ("ORBITER_NO_SCATTER"))
		if (*ns && *ns != '0') return;
	LoadAssets (std::string (planet->Name()) + ".cfg");
	if (layers.empty() || assets.empty()) return;

	for (Layer &L : layers) {
		for (size_t i = 0; i < assets.size(); i++) {
			const Asset &a = assets[i];
			if (a.kind != L.kind || a.Size() < L.minSize || a.Size() >= L.maxSize) continue;
			L.assets.push_back ((int)i);
			// (rocks: more small ones)
			L.weight.push_back (a.kind == ROCK ? pow (a.Size(), -1.5) : 1.0);
		}
	}
	cover = std::make_unique<Cover> (planet);
	active = true;
	int nObst = 0;
	for (const Layer &L : layers)
		for (int i : L.assets)
			if (L.collide && assets[i].hasHull) nObst++;
	LOGOUT ("Ground scatter of %s: %d assets, %d layers, %d obstacle types", planet->Name(), (int)assets.size(), (int)layers.size(), nObst);
}

int GroundScatter::Rows (const Layer &L) const
{
	return std::max (1, (int)(Pi * R / L.cell));
}

int GroundScatter::Cols (const Layer &L, int row) const
{
	int nr = Rows (L);
	double lat = -Pi05 + (row + 0.5) * Pi / nr;
	return std::max (1, (int)(Pi2 * R * cos (lat) / L.cell));
}

void GroundScatter::CellsNear (int layer, const Vector &p, double radius, std::vector<std::pair<int,int>> &out) const
{
	out.clear();
	if (layer < 0 || layer >= (int)layers.size()) return;
	const Layer &L = layers[layer];
	double r = p.length();
	if (r <= 0.0) return;
	double lat = asin (std::clamp (p.y / r, -1.0, 1.0)), lng = atan2 (p.z, p.x);
	double a = radius / R;
	int nr = Rows (L);
	double dth = Pi / nr;
	int r0 = std::max (0, (int)floor ((lat - a + Pi05) / dth)), r1 = std::min (nr - 1, (int)floor ((lat + a + Pi05) / dth));
	for (int row = r0; row <= r1; row++) {
		int nc = Cols (L, row);
		double la = std::max (fabs (-Pi05 + row * dth), fabs (-Pi05 + (row + 1) * dth));
		double cl = cos (std::min (la, Pi05));
		double dl = cl > 1e-6 ? a / cl : Pi;
		double w = Pi2 / nc;
		if (dl >= Pi) {
			for (int c = 0; c < nc; c++) out.push_back ({ row, c });
			continue;
		}
		int c0 = (int)floor ((lng - dl + Pi) / w), c1 = (int)floor ((lng + dl + Pi) / w);
		if (c1 - c0 + 1 >= nc) { c0 = 0, c1 = nc - 1; }
		for (int c = c0; c <= c1; c++) out.push_back ({ row, ((c % nc) + nc) % nc });
	}
}

double GroundScatter::Clearance (const Vector &dir) const
{
	if (!basesDone) {
		auto *self = const_cast<GroundScatter*> (this);
		self->basesDone = true;
		for (DWORD i = 0; i < planet->nBase(); i++) {
			const Base *b = planet->GetBase (i);
			double lng, lat;
			b->EquPos (lng, lat);
			double r = b->FootprintRadius () + 150.0;
			self->clear.push_back ({ Dir (lat, lng), r, r + 300.0 });
		}
		// (vessels that start landed on the planet)
		if (g_psys)
			for (Vessel *v : g_psys->GetVessels())
				if (v->GetStatus() == FLIGHTSTATUS_LANDED && v->ProxyPlanet() == planet) {
					Vector pl (tmul (planet->GRot(), v->GPos() - planet->GPos()));
					double r = v->Size() * 1.5 + 10.0;
					self->clear.push_back ({ pl.unit(), r, r * 1.2 });
				}
	}
	double f = 1.0;
	for (const Clearing &c : clear) {
		double d = (dir - c.dir).length() * R;
		if (d < c.r1) f = std::min (f, SmoothStep (c.r0, c.r1, d));
	}
	return f;
}

void GroundScatter::Clear (const Vector &p, double radius)
{
	if (!active) return;
	Clearance (Vector (0, 1, 0)); // (bases first)
	clear.push_back ({ p.unit(), radius, radius * 1.2 });
	cells.clear();
	generation++;
}

double GroundScatter::CoverFactor (const Layer &L, const Vector &c, double water, bool have) const
{
	if (L.cover == Layer::NONE || L.cover == Layer::SLOPE) return 1.0;
	if (!have) return L.cover == Layer::BARE ? 1.0 : 0.0; // (no surface tiles: no vegetation)
	if (water > 0.5) return 0.0;
	double sum = c.x + c.y + c.z + 1e-3;
	double lum = sum / 3.0;
	double green = c.y / sum;                         // greenness (1/3: grey)
	double sat = (std::max ({ c.x, c.y, c.z }) - std::min ({ c.x, c.y, c.z })) / (std::max ({ c.x, c.y, c.z }) + 1e-3);
	double snow = SmoothStep (0.55, 0.7, lum) * SmoothStep (0.25, 0.1, sat);
	double veg = SmoothStep (0.345, 0.385, green);     // green ground
	switch (L.cover) {
	case Layer::FOREST: // dark green
		return veg * SmoothStep (0.36, 0.2, lum) * (1.0 - snow);
	case Layer::BUSH:   // green, or dry scrub (light brown)
		return std::max (veg * SmoothStep (0.5, 0.25, lum), 0.35 * SmoothStep (0.2, 0.3, lum) * SmoothStep (0.6, 0.45, lum) * SmoothStep (0.15, 0.3, sat)) * (1.0 - snow);
	case Layer::MEADOW: // green, lighter
		return veg * SmoothStep (0.55, 0.3, lum) * (1.0 - snow);
	default:            // bare: not green, not snow
		return (1.0 - veg) * (1.0 - 0.8 * snow);
	}
}

void GroundScatter::Make (int layer, int row, int col, CellData &cd)
{
	const Layer &L = layers[layer];
	cd.inst.clear();
	if (L.assets.empty()) return;
	ElevationManager *emgr = planet->ElevMgr();
	int nr = Rows (L), nc = Cols (L, row);
	double dth = Pi / nr, dph = Pi2 / nc;
	double lat0 = -Pi05 + row * dth, lng0 = -Pi + col * dph;
	double clat = cos (lat0 + 0.5 * dth);
	double wm = R * clat * dph, hm = R * dth;
	int nx = std::max (1, (int)lround (wm / L.spacing)), ny = std::max (1, (int)lround (hm / L.spacing));
	double area = wm * hm / ((double)nx * ny);
	double coverMax = L.cover == Layer::SLOPE ? 3.0 : L.cover == Layer::BARE ? 2.0 : 1.0;
	double pmax = L.density * area * coverMax;
	uint64_t cs = Hash (Hash (seed, (uint64_t)layer), CellKey (layer, row, col));
	double wsum = 0.0;
	for (double w : L.weight) wsum += w;

	for (int j = 0; j < ny; j++)
		for (int i = 0; i < nx; i++) {
			Rnd rnd (Hash (cs, (uint64_t)(j * nx + i)));
			double u = rnd ();
			if (u >= pmax) continue;
			double lat = lat0 + (j + 0.1 + 0.8 * rnd ()) / ny * dth;
			double lng = lng0 + (i + 0.1 + 0.8 * rnd ()) / nx * dph;
			Vector dir (Dir (lat, lng));
			double p = L.density * area * Clearance (dir);
			if (u >= p * coverMax) continue;

			// ground cover
			Vector rgb;
			double water = 0.0;
			bool have = false;
			if (L.cover != Layer::NONE && L.cover != Layer::SLOPE)
				have = cover->Sample (lat, lng, rgb, water);
			p *= CoverFactor (L, rgb, water, have);
			if (u >= p * coverMax) continue;

			// the asset (plants: by latitude)
			double alat = fabs (lat) * DEG;
			double pick = rnd () * wsum, wlat = 0.0;
			int asset = -1;
			std::vector<double> w (L.weight);
			for (size_t k = 0; k < w.size(); k++) {
				const Asset &a = assets[L.assets[k]];
				if (a.kind != ROCK) w[k] *= SmoothStep (a.lat0 - 5.0, a.lat0, alat) * SmoothStep (a.lat1 + 5.0, a.lat1, alat);
				wlat += w[k];
			}
			if (wlat <= 0.0) continue;
			p *= wlat / wsum;
			pick *= wlat / wsum;
			for (size_t k = 0; k < w.size(); k++) {
				if (pick < w[k]) { asset = L.assets[k]; break; }
				pick -= w[k];
			}
			if (asset < 0) asset = L.assets.back();
			if (u >= p * coverMax) continue;

			// the ground: slope
			Vector nh (0, 1, 0);
			double elev = emgr ? emgr->Elevation (lat, lng, 0, etile.get(), &nh) : 0.0;
			double slope = 1.0 - std::clamp (nh.y, 0.0, 1.0); // (0: flat; 0.3: 45 deg)
			if (L.cover == Layer::SLOPE || L.cover == Layer::BARE)
				p *= std::min (coverMax, 0.4 + 6.0 * slope);
			else
				p *= SmoothStep (0.3, 0.12, slope);  // (vegetation: none on steep slopes)
			if (u >= p) continue;

			// the object's frame: up along the vertical, rocks tilted
			// towards the terrain normal; turned about it at random
			double slng = sin (lng), clng = cos (lng), slat = sin (lat), cla = cos (lat);
			Vector E (-slng, 0, clng), U (dir), N (-slat * clng, cla, -slat * slng);
			Vector nt (E * nh.x + U * nh.y + N * nh.z);
			double ta = rnd () * Pi2, tilt = (L.kind == ROCK ? 0.15 : 0.03) * rnd ();
			Vector up (L.kind == ROCK ? (U * 0.3 + nt * 0.7).unit() : U);
			up = (up + (E * cos (ta) + N * sin (ta)) * tilt).unit();
			Vector x ((E - up * dotp (E, up)).unit());
			Vector z (crossp (x, up));
			double yaw = rnd () * Pi2, cy = cos (yaw), sy = sin (yaw);
			Vector xr (x * cy + z * sy), zr (z * cy - x * sy);
			Instance in;
			in.asset = asset;
			in.rot = Matrix (xr.x, up.x, zr.x, xr.y, up.y, zr.y, xr.z, up.z, zr.z);
			in.scale = exp (log (0.75) + rnd () * (log (1.3) - log (0.75)));
			in.pos = dir * (R + elev);
			cd.inst.push_back (in);
		}
}

void GroundScatter::Trim ()
{
	if (cells.size() <= MAX_CELLS) return;
	std::vector<uint64_t> used;
	for (auto &c : cells) used.push_back (c.second.used);
	std::nth_element (used.begin(), used.begin() + used.size() / 4, used.end());
	uint64_t limit = used[used.size() / 4];
	for (auto it = cells.begin(); it != cells.end(); )
		if (it->second.used <= limit) it = cells.erase (it); else ++it;
}

const std::vector<Instance> &GroundScatter::Cell (int layer, int row, int col)
{
	static const std::vector<Instance> none;
	if (!active || layer < 0 || layer >= (int)layers.size()) return none;
	Trim ();
	uint64_t key = CellKey (layer, row, col);
	auto it = cells.find (key);
	if (it == cells.end()) {
		it = cells.emplace (key, CellData ()).first;
		Make (layer, row, col, it->second);
	}
	it->second.used = ++clock;
	return it->second.inst;
}

void GroundScatter::Obstacles (const Vector &p, double radius, std::vector<const Obstacle*> &out)
{
	out.clear();
	if (!active) return;
	std::vector<std::pair<int,int>> cl;
	for (int l = 0; l < (int)layers.size(); l++) {
		const Layer &L = layers[l];
		if (!L.collide) continue;
		double rmax = 0.0;
		for (int a : L.assets) rmax = std::max (rmax, assets[a].Size() * 1.3);
		CellsNear (l, p, radius + rmax, cl);
		for (auto &rc : cl) {
			Cell (l, rc.first, rc.second);
			CellData &cd = cells[CellKey (l, rc.first, rc.second)];
			if (!cd.obstDone) {
				cd.obstDone = true;
				for (const Instance &in : cd.inst) {
					const Asset &a = assets[in.asset];
					if (a.hasHull) cd.obst.push_back ({ Transform (a.hull, in.rot, in.scale, in.pos), in.asset });
				}
			}
			for (const Obstacle &o : cd.obst)
				if (o.hull.centre.dist (p) < radius + o.hull.radius) out.push_back (&o);
		}
	}
}

} // namespace scatter

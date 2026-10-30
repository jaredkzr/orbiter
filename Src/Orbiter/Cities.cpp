// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Cities.h"
#include <future>
#include <thread>
#include "GroundScatter.h"
#include "Orbiter.h"
#include "Config.h"
#include "Planet.h"
#include "Base.h"
#include "elevmgr.h"
#include "ZTreeMgr.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>

extern Orbiter *g_pOrbiter;

namespace cities {

bool CityMap::clientSupport = false;

namespace {

	const double CELL = 500.0;      // [m] cell size
	const size_t MAX_CELLS = 30000; // cached cells (all height filters)
	const int LIGHT_LEVEL = 10;     // night light tile level read (Earth: ~600 m per texel, global between 56 S and 56 N)
	const size_t LIGHT_TILES = 64;  // cached night light tiles (and absent ones)
	const double NEAR_CENTRES = 80e3; // [m] centres a point may belong to
	const double CORE_RANGE = 30e3; // [m] high-rise cores a point may be near

	inline uint64_t Mix (uint64_t x)
	{
		x += 0x9e3779b97f4a7c15ull;
		x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
		x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
		return x ^ (x >> 31);
	}
	inline uint64_t Hash (uint64_t a, uint64_t b) { return Mix (a ^ Mix (b)); }

	struct Rnd {
		uint64_t s;
		explicit Rnd (uint64_t seed): s (seed) {}
		double operator() () { s = Mix (s); return (double)(s >> 11) * (1.0 / 9007199254740992.0); }
		double operator() (double a, double b) { return a + (b - a) * (*this) (); }
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
	inline void LatLng (const Vector &d, double &lat, double &lng)
	{
		lat = asin (std::clamp (d.y, -1.0, 1.0));
		lng = atan2 (d.z, d.x);
	}
	inline Vector East (double lng) { return Vector (-sin (lng), 0, cos (lng)); }
	inline Vector North (double lat, double lng) { return Vector (-sin (lat) * cos (lng), cos (lat), -sin (lat) * sin (lng)); }

	inline uint64_t CellKey (int row, int col, double minHeight)
	{
		return ((uint64_t)std::clamp ((int)(minHeight * 4.0), 0, 65535) << 40) | ((uint64_t)(uint32_t)row << 20) | (uint64_t)(uint32_t)col;
	}

	/// smooth value noise in -1..1 (lattice spacing 1)
	double Noise (uint64_t seed, double x, double y)
	{
		double fx = floor (x), fy = floor (y);
		int64_t ix = (int64_t)fx, iy = (int64_t)fy;
		double u = x - fx, v = y - fy;
		u = u * u * (3.0 - 2.0 * u), v = v * v * (3.0 - 2.0 * v);
		auto h = [&](int64_t a, int64_t b) {
			return (double)(Hash (Hash (seed, (uint64_t)a), (uint64_t)b) >> 11) * (2.0 / 9007199254740992.0) - 1.0;
		};
		double a = h (ix, iy), b = h (ix + 1, iy), c = h (ix, iy + 1), d = h (ix + 1, iy + 1);
		return (a + (b - a) * u) * (1.0 - v) + (c + (d - c) * u) * v;
	}

	/// a box: footprint w x d about the origin, from 1 m below the ground to h
	convex::Polytope Box (const Building &b)
	{
		std::vector<Vector> pts;
		for (int i = 0; i < 8; i++) {
			Vector l ((i & 1 ? 0.5 : -0.5) * b.w, i & 2 ? (double)b.h : -1.0, (i & 4 ? 0.5 : -0.5) * b.d);
			pts.push_back (mul (b.rot, l) + b.pos);
		}
		convex::Polytope P;
		convex::Build (pts, P);
		return P;
	}

} // namespace

/// A street grid: a centre's frame on the tangent plane (gnomonic), its
/// streets and warp
struct Grid {
	int64_t id;     // owner id (Owner)
	Vector C, X, Y; // centre, axes
	double R;
	const Style *st;
	int style;
	uint64_t seed;
	// plane coordinates [m] <-> directions
	void ToPlane (const Vector &d, double &x, double &y) const
	{
		double k = dotp (d, C);
		Vector q = d * (R / std::max (k, 1e-6));
		x = dotp (q, X), y = dotp (q, Y);
	}
	Vector ToDir (double x, double y) const { return (C * R + X * x + Y * y).unit(); }
	// street coordinates (s, t) -> plane (warped)
	void Warp (double s, double t, double &x, double &y) const
	{
		x = s, y = t;
		if (st->warp > 0.0) {
			double k = 1.0 / st->warpScale;
			x += st->warp * Noise (seed ^ 0x51, s * k, t * k);
			y += st->warp * Noise (seed ^ 0x52, s * k, t * k);
		}
	}
	// direction of the centre of block (i, j)
	Vector BlockCentre (int64_t i, int64_t j) const
	{
		double x, y;
		Warp (0.5 * (Line (0, i) + Line (0, i + 1)), 0.5 * (Line (1, j) + Line (1, j + 1)), x, y);
		return ToDir (x, y);
	}
	// position of street line k along an axis (0: s, 1: t)
	double Line (int axis, int64_t k) const
	{
		double b = axis ? st->blockT : st->blockS;
		double j = (double)(Hash (seed ^ (0x100 + axis), (uint64_t)k) >> 11) * (1.0 / 9007199254740992.0) - 0.5;
		return (k + st->jitter * j) * b;
	}
};

/// A block's zoning inputs
struct Zone {
	double clr, core, hMax, dtBuilt, lit, built;
	Rnd rb{0}; // (its random numbers, after the one that decided it is built up)
};

// =======================================================================
// Lights

Lights::Lights (const Planet *p): planet (p)
{
	DWORD src = g_pOrbiter->Cfg()->CfgPRenderPrm.TileLoadFlags;
	char cbuf[512];
	if (src & 0x0002) {
		g_pOrbiter->Cfg()->PTexPath (cbuf, planet->Name());
		tree = ZTreeMgr::CreateFromFile (cbuf, ZTreeMgr::LAYER_MASK);
	}
	if (src & 0x0001) {
		std::string f = std::string (planet->Name()) + "\\Mask";
		g_pOrbiter->Cfg()->PTexPath (cbuf, f.c_str());
		dir = cbuf;
	}
}

Lights::~Lights ()
{
	delete tree;
}

const Lights::Tile *Lights::Get (double lat, double lng, bool load, bool &miss)
{
	// the finest tile up to LIGHT_LEVEL that has data
	miss = false;
	for (int lvl = LIGHT_LEVEL; lvl >= 4; lvl--) {
		int nlat = 1 << (lvl - 4), nlng = 2 << (lvl - 4);
		int ilat = std::clamp ((int)((Pi05 - lat) / Pi * nlat), 0, nlat - 1);
		int ilng = std::clamp ((int)((lng + Pi) / Pi2 * nlng), 0, nlng - 1);
		uint64_t key = ((uint64_t)lvl << 48) | ((uint64_t)ilat << 24) | (uint64_t)ilng;
		auto it = cache.find (key);
		if (it != cache.end()) {
			uint64_t c = clock.load (std::memory_order_relaxed); // (no stores to shared lines when unchanged)
			if (it->second->used.load (std::memory_order_relaxed) != c) it->second->used.store (c, std::memory_order_relaxed);
			if (it->second->n) return it->second.get();
			if (it->second->lvl < 0) continue; // (none at this level: a coarser one)
			return nullptr;
		}
		if (!load) { miss = true; return nullptr; }
		auto t = std::make_unique<Tile> ();
		t->lvl = lvl, t->ilat = ilat, t->ilng = ilng;
		t->used = ++clock;
		std::vector<uint32_t> px;
		int w = 0, h = 0;
		bool ok = false;
		if (tree && tree->Idx (lvl, ilat, ilng) == (DWORD)-1) {
			t->lvl = -1; // (absent: noted)
		} else if (tree) {
			BYTE *buf = nullptr;
			DWORD n = tree->ReadData (lvl, ilat, ilng, &buf);
			if (n) {
				ok = scatter::DecodeDXT1 (buf, n, px, w, h);
				tree->ReleaseData (buf);
			}
			if (!ok) t->lvl = -1;
		} else if (!dir.empty()) {
			char fname[64];
			snprintf (fname, sizeof(fname), "\\%02d\\%06d\\%06d.dds", lvl, ilat, ilng);
			std::string path = dir + fname;
			for (char &c : path) if (c == '\\') c = '/';
			std::ifstream ifs (path, std::ios::binary);
			if (ifs.good()) {
				std::vector<uint8_t> d ((std::istreambuf_iterator<char> (ifs)), std::istreambuf_iterator<char> ());
				ok = scatter::DecodeDXT1 (d.data(), d.size(), px, w, h);
			}
		}
		if (ok && w == h && w >= 8) {
			// lit texels: lights (yellow to white; the dim blue speckle on some
			// deserts and salt flats is not); their share of the land texels
			// (rivers and coasts do not thin a city out) in 2x2 texels,
			// smoothed (gaussian, sigma 1)
			int n = w / 2;
			std::vector<float> a ((size_t)n * n, 0.0f), land ((size_t)n * n, 0.0f), b ((size_t)n * n);
			for (int y = 0; y < w; y++)
				for (int x = 0; x < w; x++) {
					uint32_t c = px[(size_t)y * w + x];
					if ((c >> 24) < 128) continue; // (water)
					uint32_t r = c & 255, bl = (c >> 16) & 255;
					size_t i = (size_t)(y / 2) * n + x / 2;
					land[i] += 0.25f;
					if (r > 16 && r * 10 >= bl * 6) a[i] += 0.25f;
				}
			const float k[7] = { 0.0044f, 0.054f, 0.242f, 0.399f, 0.242f, 0.054f, 0.0044f };
			auto blur = [&](std::vector<float> &img) {
				for (int pass = 0; pass < 2; pass++) {
					std::vector<float> &in = pass ? b : img, &out = pass ? img : b;
					for (int y = 0; y < n; y++)
						for (int x = 0; x < n; x++) {
							float s = 0.0f;
							for (int i = -3; i <= 3; i++) {
								int xx = pass ? x : std::clamp (x + i, 0, n - 1), yy = pass ? std::clamp (y + i, 0, n - 1) : y;
								s += k[i + 3] * in[(size_t)yy * n + xx];
							}
							out[(size_t)y * n + x] = s;
						}
				}
			};
			blur (a);
			blur (land);
			for (size_t i = 0; i < a.size(); i++)
				a[i] = land[i] > 0.1f ? std::min (1.0f, a[i] / land[i]) : 0.0f;
			t->lit = std::move (a);
			t->n = n;
		}
		if (cache.size() >= LIGHT_TILES) { // (the least recently used out)
			auto lru = cache.begin();
			for (auto c = cache.begin(); c != cache.end(); ++c)
				if (c->second->used < lru->second->used) lru = c;
			cache.erase (lru);
		}
		Tile *r = (cache[key] = std::move (t)).get();
		if (r->n) return r;
		if (r->lvl < 0) continue;
		return nullptr;
	}
	return nullptr;
}

double Lights::Sample (double lat, double lng)
{
	bool miss;
	auto sample = [&](const Tile *t) {
		if (!t) return -1.0;
		int nlat = 1 << (t->lvl - 4), nlng = 2 << (t->lvl - 4);
		double u = ((lng + Pi) / Pi2 * nlng - t->ilng) * t->n - 0.5;
		double v = ((Pi05 - lat) / Pi * nlat - t->ilat) * t->n - 0.5;
		int x0 = (int)floor (u), y0 = (int)floor (v);
		double fu = u - x0, fv = v - y0;
		auto at = [&](int x, int y) { return (double)t->lit[(size_t)std::clamp (y, 0, t->n - 1) * t->n + std::clamp (x, 0, t->n - 1)]; };
		return (at (x0, y0) * (1 - fu) + at (x0 + 1, y0) * fu) * (1 - fv) + (at (x0, y0 + 1) * (1 - fu) + at (x0 + 1, y0 + 1) * fu) * fv;
	};
	{
		std::shared_lock<std::shared_mutex> lock (mtx);
		const Tile *t = Get (lat, lng, false, miss);
		if (!miss) return sample (t);
	}
	std::unique_lock<std::shared_mutex> lock (mtx);
	return sample (Get (lat, lng, true, miss));
}

// =======================================================================
// CityMap

CityMap::CityMap (const Planet *p): planet (p), R (p->Size())
{
	etile = std::make_unique<std::vector<ElevationTile>> (16);
	seed = 0xc17u;
	for (const char *c = planet->Name(); *c; c++) seed = seed * 31u + (uint8_t)*c;
	Load ();
}

CityMap::~CityMap () {}

void CityMap::Load ()
{
	if (!g_pOrbiter->Cfg()->CfgVisualPrm.bCities) return;
	if (g_pOrbiter->GetGraphicsClient() && !clientSupport) return;
	if (const char *nc = getenv ("ORBITER_NO_CITIES"))
		if (*nc && *nc != '0') return;
	std::string cfgName = std::string ("Cities/") + planet->Name() + ".cfg";
	std::ifstream ifs (g_pOrbiter->Cfg()->ConfigPathNoext (cfgName.c_str()));
	if (!ifs.good()) return; // (a planet without cities)
	std::string line, centresFile, downtownsFile;
	while (std::getline (ifs, line)) {
		size_t c = line.find (';');
		if (c != std::string::npos) line.erase (c);
		std::istringstream ss (line);
		std::string tok;
		if (!(ss >> tok)) continue;
		if (tok == "CENTRES") ss >> centresFile;
		else if (tok == "DOWNTOWNS") ss >> downtownsFile;
		else if (tok == "STYLE") {
			Style s;
			ss >> s.name;
			std::string kv;
			while (ss >> kv) {
				size_t eq = kv.find ('=');
				if (eq == std::string::npos) continue;
				std::string k = kv.substr (0, eq), v = kv.substr (eq + 1);
				double x = atof (v.c_str());
				auto range = [&](double r[2]) { size_t co = v.find (':'); r[0] = x; r[1] = co != std::string::npos ? atof (v.c_str() + co + 1) : x; };
				if (k == "block") { s.blockS = x; size_t co = v.find (':'); s.blockT = co != std::string::npos ? atof (v.c_str() + co + 1) : x; }
				else if (k == "jitter") s.jitter = x;
				else if (k == "street") s.street = x;
				else if (k == "warp") { s.warp = x; size_t co = v.find (':'); if (co != std::string::npos) s.warpScale = atof (v.c_str() + co + 1); }
				else if (k == "spread") s.spread = x;
				else if (k == "house") { s.houseW = x; size_t co = v.find (':'); if (co != std::string::npos) s.houseD = atof (v.c_str() + co + 1); }
				else if (k == "houselot") s.houseLot = x;
				else if (k == "housestoreys") s.houseStoreys = x;
				else if (k == "storey") s.storey = x;
				else if (k == "low") range (s.lowStoreys);
				else if (k == "mid") range (s.midStoreys);
				else if (k == "perimeter") s.perimeter = x;
				else if (k == "highrise") s.highrise = x;
				else if (k == "industrial") s.industrial = x;
				else if (k == "park") s.park = x;
				else if (k == "towers") s.towerScale = x;
			}
			styles.push_back (s);
		} else if (tok == "REGION") {
			std::string name;
			Region r;
			if (!(ss >> name >> r.lat0 >> r.lat1 >> r.lng0 >> r.lng1)) continue;
			r.style = -1;
			for (size_t i = 0; i < styles.size(); i++) if (styles[i].name == name) r.style = (int)i;
			if (r.style < 0) { LOGOUT_WARN ("Cities of %s: region of unknown style %s", planet->Name(), name.c_str()); continue; }
			regions.push_back (r);
		}
	}
	if (styles.empty()) styles.push_back (Style ());
	if (centresFile.empty()) return;
	std::ifstream cf (g_pOrbiter->Cfg()->ConfigPathNoext (("Cities/" + centresFile).c_str()));
	if (!cf.good()) { LOGOUT_WARN ("Cities of %s: cannot read Config/Cities/%s", planet->Name(), centresFile.c_str()); return; }
	auto frame = [&](Centre &c, double th) {
		Vector E (East (c.lng)), N (North (c.lat, c.lng));
		c.X = E * cos (th) + N * sin (th);
		c.Y = N * cos (th) - E * sin (th);
	};
	while (std::getline (cf, line)) {
		if (line.empty() || line[0] == ';') continue;
		std::istringstream ss (line);
		Centre c;
		double peak;
		int main;
		if (!(ss >> c.lat >> c.lng >> c.share >> peak >> c.metro >> main)) continue;
		c.main = main != 0;
		c.lat *= RAD, c.lng *= RAD;
		c.dir = Dir (c.lat, c.lng);
		c.style = StyleAt (c.lat, c.lng);
		c.seed = Hash (seed, (uint64_t)centres.size() + 1);
		const Style &st = styles[c.style];
		frame (c, st.spread < 0.0 ? Rnd (c.seed ^ 0x7e7a) () * Pi : (Rnd (c.seed ^ 0x7e7a) () - 0.5) * 2.0 * st.spread * RAD);
		centres.push_back (c);
	}

	// downtowns: known city centres (where their skylines are); the
	// catalogue's centres about them yield (removed near them, no main
	// centre within reach)
	if (!downtownsFile.empty()) {
		std::ifstream df (g_pOrbiter->Cfg()->ConfigPathNoext (("Cities/" + downtownsFile).c_str()));
		if (!df.good()) LOGOUT_WARN ("Cities of %s: cannot read Config/Cities/%s", planet->Name(), downtownsFile.c_str());
		std::vector<Centre> dts;
		while (std::getline (df, line)) {
			size_t c0 = line.find (';');
			if (c0 != std::string::npos) line.erase (c0);
			std::istringstream ss (line);
			Centre c;
			std::string grid;
			if (!(ss >> c.lat >> c.lng >> c.hMax >> c.rCore >> grid)) continue;
			c.lat *= RAD, c.lng *= RAD;
			c.dir = Dir (c.lat, c.lng);
			c.style = StyleAt (c.lat, c.lng);
			c.main = true;
			c.downtown = true;
			c.share = c.metro = 0.0;
			c.reach = std::max (4.0 * c.rCore, 6000.0);
			c.seed = Hash (seed, 0xd0000000ull + dts.size());
			const Style &st = styles[c.style];
			double th = grid != "*" ? -atof (grid.c_str()) * RAD : // (grid: bearing of its streets [deg east of north])
				st.spread < 0.0 ? Rnd (c.seed ^ 0x7e7a) () * Pi : (Rnd (c.seed ^ 0x7e7a) () - 0.5) * 2.0 * st.spread * RAD;
			frame (c, th);
			dts.push_back (c);
		}
		for (Centre &c : centres)
			for (const Centre &d : dts) {
				double dist = (c.dir - d.dir).length() * R;
				if (dist < std::max (3.0 * d.rCore, 5000.0)) { c.metro = -1.0; break; } // (removed)
				if (dist < 30e3) c.main = false;
			}
		centres.erase (std::remove_if (centres.begin(), centres.end(), [](const Centre &c) { return c.metro < 0.0; }), centres.end());
		nDowntown = (int)dts.size();
		centres.insert (centres.end(), dts.begin(), dts.end());
	}
	for (size_t i = 0; i < centres.size(); i++) {
		Centre &c = centres[i];
		if (!c.downtown) {
			c.reach = 2000.0 * pow (c.metro + 1.0, 0.4);
			c.rCore = 500.0 * pow (std::max (c.metro, 1.0), 0.3) * (c.main ? 1.0 : 0.7);
			c.hMax = 6.0 * pow (std::max (c.metro, 1.0), 0.55) * (c.main ? 1.0 : 0.45) * styles[c.style].towerScale;
		}
		int key = (int)floor (c.lat * DEG + 90.0) * 1000 + (int)floor (c.lng * DEG + 180.0);
		bins[key].push_back ((int)i);
	}
	if (centres.empty()) return;
	lights = std::make_unique<Lights> (planet);
	cover = std::make_unique<scatter::Cover> (planet);
	active = true;
	LOGOUT ("Cities of %s: %d centres (%d downtowns), %d styles, %d regions", planet->Name(), (int)centres.size(), nDowntown, (int)styles.size(), (int)regions.size());
}

int CityMap::StyleAt (double lat, double lng) const
{
	double la = lat * DEG, lo = lng * DEG;
	for (const Region &r : regions)
		if (la >= r.lat0 && la <= r.lat1 && lo >= r.lng0 && lo <= r.lng1) return r.style;
	return 0;
}

void CityMap::Near (const Vector &dir, double radius, std::vector<int> &out) const
{
	out.clear();
	double lat, lng;
	LatLng (dir, lat, lng);
	double dl = radius / R * DEG;
	double dg = dl / std::max (cos (lat), 0.01);
	int la0 = (int)floor (lat * DEG - dl + 90.0), la1 = (int)floor (lat * DEG + dl + 90.0);
	int lo0 = (int)floor (lng * DEG - dg + 180.0), lo1 = (int)floor (lng * DEG + dg + 180.0);
	if (lo1 - lo0 >= 359) lo0 = 0, lo1 = 359;
	double c2 = radius / R;
	c2 *= c2;
	for (int a = std::max (la0, 0); a <= std::min (la1, 179); a++)
		for (int o = lo0; o <= lo1; o++) {
			auto it = bins.find (a * 1000 + ((o % 360) + 360) % 360);
			if (it == bins.end()) continue;
			for (int i : it->second)
				if ((centres[i].dir - dir).length2() < c2) out.push_back (i);
		}
}

int CityMap::Owner (const Vector &dir) const
{
	thread_local std::vector<int> near;
	Near (dir, NEAR_CENTRES, near);
	int best = -1;
	double bd = 1e30;
	for (int i : near) {
		double d = (centres[i].dir - dir).length() * R / centres[i].reach;
		if (d < bd || (d == bd && i < best)) bd = d, best = i;
	}
	return best;
}

void CityMap::Core (const Vector &dir, double &core, double &hMax, double &built) const
{
	thread_local std::vector<int> near; // (Built: any thread)
	Near (dir, CORE_RANGE, near);
	core = 0.0, hMax = 0.0, built = 0.0;
	double best = 0.0;
	for (int i : near) {
		const Centre &c = centres[i];
		double d = (c.dir - dir).length() * R;
		double f = exp (-(d / c.rCore) * (d / c.rCore));
		if (f * c.hMax > best) best = f * c.hMax, core = f, hMax = c.hMax;
		if (c.downtown) built = std::max (built, SmoothStep (3.5 * c.rCore, 1.5 * c.rCore, d)); // (built up, lights or not)
	}
}

double CityMap::CellSize () const { return active ? CELL : 0.0; }

int CityMap::Rows () const
{
	return std::max (1, (int)(Pi * R / CELL));
}

int CityMap::Cols (int row) const
{
	int nr = Rows ();
	double lat = -Pi05 + (row + 0.5) * Pi / nr;
	return std::max (1, (int)(Pi2 * R * cos (lat) / CELL));
}

void CityMap::CellsNear (const Vector &p, double radius, std::vector<std::pair<int,int>> &out) const
{
	out.clear();
	if (!active) return;
	double r = p.length();
	if (r <= 0.0) return;
	double lat = asin (std::clamp (p.y / r, -1.0, 1.0)), lng = atan2 (p.z, p.x);
	double a = radius / R;
	int nr = Rows ();
	double dth = Pi / nr;
	int r0 = std::max (0, (int)floor ((lat - a + Pi05) / dth)), r1 = std::min (nr - 1, (int)floor ((lat + a + Pi05) / dth));
	for (int row = r0; row <= r1; row++) {
		int nc = Cols (row);
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

double CityMap::Clearance (const Vector &dir) const
{
	if (!basesDone) {
		auto *self = const_cast<CityMap*> (this);
		self->basesDone = true;
		for (DWORD i = 0; i < planet->nBase(); i++) {
			const Base *b = planet->GetBase (i);
			double lng, lat;
			b->EquPos (lng, lat);
			double r = b->FootprintRadius () + 300.0;
			self->clear.push_back ({ Dir (lat, lng), r, r + 500.0 });
		}
		for (auto &c : planet->ScatterClearings())
			self->clear.push_back ({ c.first.unit(), c.second, c.second * 1.2 + 20.0 });
	}
	double f = 1.0;
	for (const Clearing &c : clear) {
		double d = (dir - c.dir).length() * R;
		if (d < c.r1) f = std::min (f, SmoothStep (c.r0, c.r1, d));
	}
	return f;
}

void CityMap::Clear (const Vector &p, double radius)
{
	if (!active) return;
	if (basesDone) clear.push_back ({ p.unit(), radius, radius * 1.2 + 20.0 });
	cells.clear();
	streetCells.clear();
	generation++;
}

bool CityMap::Water (double lat, double lng)
{
	Vector rgb;
	double water = 0.0;
	if (!cover->Sample (lat, lng, rgb, water)) return false;
	if (water > 0.5) return true;
	// (no water mask there, e.g. Earth poleward of 56 deg: dark blue ground)
	if (!cover->HasMask (lat, lng)) {
		double lum = (rgb.x + rgb.y + rgb.z) / 3.0;
		return rgb.z > rgb.x * 1.15 && rgb.z >= rgb.y * 0.95 && lum < 0.3;
	}
	return false;
}

double CityMap::Urban (const Vector &dir)
{
	if (!active) return 0.0;
	double lat, lng;
	LatLng (dir, lat, lng);
	return SmoothStep (0.06, 0.4, lights->Sample (lat, lng));
}

namespace {
	const double GROUND_STEP = 0.0025 * RAD; // node spacing [rad]
}

float CityMap::GroundNode (int64_t i, int64_t j)
{
	// an elevation manager not in use (the data alone)
	RawElev *re = nullptr;
	{
		std::lock_guard<std::mutex> lock (rawMtx);
		for (auto &r : rawElev)
			if (r->m.try_lock ()) { re = r.get(); break; }
		if (!re) {
			rawElev.push_back (std::make_unique<RawElev> ());
			re = rawElev.back().get();
			re->m.lock ();
			re->em = std::make_unique<ElevationManager> (planet);
			re->em->SetNoDetail ();
			re->tiles = std::make_unique<std::vector<ElevationTile>> (16);
		}
	}
	// samples 5 x 5, 90 m apart; their lowest eighth (downtowns: mostly roofs)
	double lat = i * GROUND_STEP, lng = j * GROUND_STEP;
	double dlat = 90.0 / R, dlng = 90.0 / (R * std::max (cos (lat), 0.01));
	float e[25];
	int n = 0;
	for (int a = -2; a <= 2; a++)
		for (int b = -2; b <= 2; b++) {
			double h = re->em->Elevation (lat + a * dlat, lng + b * dlng, 0, re->tiles.get());
			if (fabs (h) > 0.25) e[n++] = (float)h; // (not the sea)
		}
	re->m.unlock ();
	float g = std::numeric_limits<float>::quiet_NaN ();
	if (n >= 6) {
		std::nth_element (e, e + n / 8, e + n);
		g = e[n / 8];
	}
	return g;
}

std::shared_ptr<const CityMap::GroundBlock> CityMap::MakeGroundBlock (int64_t bi, int64_t bj)
{
	// The nodes in parallel (the pool of data-only elevation managers:
	// one per thread at a time). ORBITER_CITY_GROUND_SYNC=1: on the calling
	// thread (threads crashed here in ElevationManager::Elevation once, at
	// Sriharikota; not reproduced since, 2026-10-30)
	auto blk = std::make_shared<GroundBlock> ();
	auto rows = [&](int r0, int r1) {
		for (int r = r0; r < r1; r++)
			for (int c = 0; c < GBLOCK; c++) (*blk)[r * GBLOCK + c] = GroundNode (bi * GBLOCK + r, bj * GBLOCK + c);
	};
	static const bool sync = getenv ("ORBITER_CITY_GROUND_SYNC") && atoi (getenv ("ORBITER_CITY_GROUND_SYNC"));
	const int nt = sync ? 1 : (int)std::clamp (std::thread::hardware_concurrency () / 2, 1u, 4u);
	std::vector<std::future<void>> fs;
	for (int k = 1; k < nt; k++)
		fs.push_back (std::async (std::launch::async, rows, GBLOCK * k / nt, GBLOCK * (k + 1) / nt));
	rows (0, GBLOCK / nt);
	for (auto &f : fs) f.get ();
	return blk;
}

double CityMap::Ground (double lat, double lng)
{
	if (!active) return std::numeric_limits<double>::quiet_NaN ();
	double u = lat / GROUND_STEP, v = lng / GROUND_STEP;
	int64_t i = (int64_t)floor (u), j = (int64_t)floor (v);
	double fu = u - i, fv = v - j;
	float g[4];
	for (int k = 0; k < 4; k++) {
		int64_t ni = i + (k >> 1), nj = j + (k & 1);
		int64_t bi = ni >= 0 ? ni / GBLOCK : (ni - GBLOCK + 1) / GBLOCK, bj = nj >= 0 ? nj / GBLOCK : (nj - GBLOCK + 1) / GBLOCK;
		int64_t key = NodeKey (bi, bj);
		std::shared_ptr<const GroundBlock> blk;
		{
			std::shared_lock<std::shared_mutex> lock (groundMtx);
			auto it = groundBlocks.find (key);
			if (it != groundBlocks.end()) blk = it->second;
		}
		if (!blk) { // (made outside the lock; by two threads at once at worst)
			blk = MakeGroundBlock (bi, bj);
			std::unique_lock<std::shared_mutex> lock (groundMtx);
			if (groundBlocks.size() > 50000) groundBlocks.clear();
			groundBlocks[key] = blk;
		}
		g[k] = (*blk)[(ni - bi * GBLOCK) * GBLOCK + (nj - bj * GBLOCK)];
	}
	double w[4] = { (1 - fu) * (1 - fv), (1 - fu) * fv, fu * (1 - fv), fu * fv }, s = 0.0, sw = 0.0;
	for (int k = 0; k < 4; k++)
		if (!std::isnan (g[k])) s += w[k] * g[k], sw += w[k];
	return sw > 0.5 ? s / sw : std::numeric_limits<double>::quiet_NaN ();
}

double CityMap::Built (const Vector &dir)
{
	if (!active) return 0.0;
	double lat, lng, core, hMax, dtBuilt;
	LatLng (dir, lat, lng);
	Core (dir, core, hMax, dtBuilt);
	return std::max (SmoothStep (0.05, 0.3, lights->Sample (lat, lng)), dtBuilt); // (as Zoned)
}

bool CityMap::Occupied (const Vector &p, double margin)
{
	if (!active) return false;
	static std::vector<std::pair<int,int>> cl;
	CellsNear (p, 150.0, cl);
	for (auto &rc : cl)
		for (const Building &b : Cell (rc.first, rc.second)) {
			Vector l (tmul (b.rot, p - b.pos));
			if (fabs (l.x) < 0.5 * b.w + margin && fabs (l.z) < 0.5 * b.d + margin) return true;
		}
	return false;
}

Grid CityMap::GridOf (int64_t id) const
{
	Grid g;
	g.id = id, g.R = R;
	if (id >= 0) {
		const Centre &c = centres[id];
		g.C = c.dir, g.X = c.X, g.Y = c.Y, g.style = c.style, g.seed = c.seed;
	} else { // (none near: a grid of its own, north-aligned, per degree)
		int64_t k = -id - 1;
		double la = ((double)(k / 1000) - 89.5) * RAD, lo = ((double)(k % 1000) - 179.5) * RAD;
		g.C = Dir (la, lo), g.X = East (lo), g.Y = North (la, lo);
		g.style = StyleAt (la, lo), g.seed = Hash (seed, (uint64_t)k + 0x9000000);
	}
	g.st = &styles[g.style];
	return g;
}

int64_t CityMap::OwnerId (const Vector &d) const
{
	int o = Owner (d);
	if (o >= 0) return o;
	double la, lo;
	LatLng (d, la, lo);
	return -1 - ((int64_t)floor (la * DEG + 90.0) * 1000 + (int64_t)floor (lo * DEG + 180.0));
}

void CityMap::Blocks (int row, int col, const std::function<void(const Grid&, int64_t, int64_t, const Vector&)> &fn)
{
	int nr = Rows (), nc = Cols (row);
	double dth = Pi / nr, dph = Pi2 / nc;
	double lat0 = -Pi05 + row * dth, lng0 = -Pi + col * dph;
	auto inCell = [&](const Vector &d) {
		double la, lo;
		LatLng (d, la, lo);
		if (la < lat0 || la >= lat0 + dth) return false;
		double x = lo - lng0;
		if (x < 0.0) x += Pi2; else if (x >= Pi2) x -= Pi2;
		return x < dph;
	};
	// the street grids of the cell: of the centres its points belong to
	std::set<int64_t> owners;
	for (int j = 0; j <= 2; j++)
		for (int i = 0; i <= 2; i++)
			owners.insert (OwnerId (Dir (lat0 + dth * j * 0.5, lng0 + dph * i * 0.5)));
	for (int64_t id : owners) {
		Grid g = GridOf (id);
		const Style &st = *g.st;
		// the cell's extent in street coordinates (warp and a block of margin)
		double smin = 1e30, smax = -1e30, tmin = 1e30, tmax = -1e30;
		for (int j = 0; j <= 2; j++)
			for (int i = 0; i <= 2; i++) {
				double x, y;
				g.ToPlane (Dir (lat0 + dth * j * 0.5, lng0 + dph * i * 0.5), x, y);
				smin = std::min (smin, x), smax = std::max (smax, x), tmin = std::min (tmin, y), tmax = std::max (tmax, y);
			}
		double m = st.warp + std::max (st.blockS, st.blockT);
		int64_t i0 = (int64_t)floor ((smin - m) / st.blockS), i1 = (int64_t)floor ((smax + m) / st.blockS);
		int64_t j0 = (int64_t)floor ((tmin - m) / st.blockT), j1 = (int64_t)floor ((tmax + m) / st.blockT);
		if ((i1 - i0) * (j1 - j0) > 400) continue; // (sanity)
		for (int64_t bj = j0; bj <= j1; bj++)
			for (int64_t bi = i0; bi <= i1; bi++) {
				Vector bdir (g.BlockCentre (bi, bj));
				if (inCell (bdir) && OwnerId (bdir) == id) fn (g, bi, bj, bdir);
			}
	}
}

bool CityMap::Zoned (const Grid &g, int64_t bi, int64_t bj, const Vector &bdir, Zone &z)
{
	z.rb = Rnd (Hash (Hash (g.seed, (uint64_t)bi), (uint64_t)bj));
	z.clr = Clearance (bdir);
	if (z.clr <= 0.0) return false;
	double blat, blng;
	LatLng (bdir, blat, blng);
	Core (bdir, z.core, z.hMax, z.dtBuilt);
	z.lit = std::max (lights->Sample (blat, blng), 0.7 * z.dtBuilt);
	z.built = std::max (SmoothStep (0.05, 0.3, z.lit), z.dtBuilt) * z.clr;
	return z.built > 0.0 && z.rb () < std::min (1.0, z.built * 1.6);
}

void CityMap::Make (int row, int col, double minHeight, CellData &cd)
{
	cd.b.clear();
	ElevationManager *emgr = planet->ElevMgr();
	Blocks (row, col, [&](const Grid &g, int64_t bi, int64_t bj, const Vector &bdir) {
				const Style &st = *g.st;
				double s0 = g.Line (0, bi), s1 = g.Line (0, bi + 1), t0 = g.Line (1, bj), t1 = g.Line (1, bj + 1);
				// the block: zoned
				Zone z;
				if (!Zoned (g, bi, bj, bdir, z)) return;
				Rnd &rb = z.rb;
				double core = z.core, hMax = z.hMax, dtBuilt = z.dtBuilt, lit = z.lit, built = z.built;
				int kind;
				double r = rb ();
				if (r < st.park * (1.0 - 0.6 * core)) return; // (a park, a square)
				if (core > 0.45 && hMax > 60.0) kind = TOWER;
				else if (core > 0.12 || dtBuilt > 0.4 || (lit > 0.5 && rb () < 0.25)) kind = MIDRISE;
				else if (lit > 0.2 && lit < 0.6 && rb () < st.industrial) kind = INDUSTRIAL;
				else if (lit > 0.4 && rb () < (lit - 0.3) * 1.5) kind = LOWRISE;
				else kind = HOUSE;
				bool apartments = kind == MIDRISE && core < 0.3 && rb () < st.highrise; // (point towers)
				double maxH = kind == HOUSE ? 12.0 : kind == LOWRISE ? st.lowStoreys[1] * st.storey + 3.0 :
					kind == INDUSTRIAL ? 16.0 : kind == TOWER ? 2.5 * hMax + 20.0 :
					apartments ? 40.0 * st.storey : st.midStoreys[1] * st.storey + 3.0;
				if (maxH < minHeight) return;

				// the block's lots inside its streets; x along the longer side
				double hs = 0.5 * st.street;
				double bs0 = s0 + hs, bs1 = s1 - hs, bt0 = t0 + hs, bt1 = t1 - hs;
				if (bs1 - bs0 < 15.0 || bt1 - bt0 < 15.0) return;
				bool alongS = bs1 - bs0 >= bt1 - bt0;
				double L = alongS ? bs1 - bs0 : bt1 - bt0, D = alongS ? bt1 - bt0 : bs1 - bs0;
				double lc = alongS ? 0.5 * (bs0 + bs1) : 0.5 * (bt0 + bt1), dc = alongS ? 0.5 * (bt0 + bt1) : 0.5 * (bs0 + bs1);
				// a building at (x along L, z along D) from the block centre,
				// turned by quarter turns q
				// storeys of sh [m], h the height above the ground [m]
				auto emit = [&](double x, double z, double w, double d, double h, int q, int k, int storeys, double sh) {
					if (h < minHeight) return;
					double s = alongS ? lc + x : dc + z, t = alongS ? dc + z : lc + x;
					double wx, wy, ax, ay;
					g.Warp (s, t, wx, wy);
					if (alongS) g.Warp (s + 1.0, t, ax, ay); else g.Warp (s, t + 1.0, ax, ay);
					Vector d0 (g.ToDir (wx, wy));
					double la, lo;
					LatLng (d0, la, lo);
					if (Clearance (d0) < 0.5) return;
					if (Water (la, lo)) return;
					// its frame: x along the street, y up
					Vector up (d0);
					Vector fx ((g.ToDir (ax, ay) - d0));
					fx = (fx - up * dotp (fx, up)).unit();
					for (int i = 0; i < (q & 3); i++) fx = crossp (fx, up); // (quarter turns about up)
					Vector fz (crossp (fx, up));
					// the ground: sunk to its lowest corner; not on steep slopes
					Vector nh (0, 1, 0);
					double elev = emgr ? emgr->Elevation (la, lo, 0, etile.get(), &nh) : 0.0;
					double slope = sqrt (std::max (0.0, 1.0 - nh.y * nh.y)) / std::max (nh.y, 0.1); // (tan)
					if (slope > (k == HOUSE ? 0.35 : 0.2)) return;
					double sink = 0.5 * sqrt (w * w + d * d) * slope + 0.3;
					Building b;
					b.pos = up * (R + elev - sink);
					b.rot = Matrix (fx.x, up.x, fz.x, fx.y, up.y, fz.y, fx.z, up.z, fz.z);
					b.w = (float)w, b.d = (float)d, b.h = (float)(h + sink);
					b.sunk = (float)sink, b.storey = (float)sh, b.storeys = (uint16_t)std::max (1, storeys);
					b.kind = (uint8_t)k, b.style = (uint8_t)g.style;
					b.seed = (uint32_t)Hash (rb.s, cd.b.size());
					cd.b.push_back (b);
				};
				double pLot = std::min (1.0, built * 1.3);
				switch (kind) {
				case HOUSE: {
					// rows of detached houses facing the long streets
					double lotW = st.houseLot * rb (0.85, 1.2);
					int n = std::max (1, (int)(L / lotW));
					int rows = D >= 2.0 * (st.houseD + 10.0) ? 2 : 1;
					for (int rr = 0; rr < rows; rr++)
						for (int k = 0; k < n; k++) {
							Rnd rl (Hash (rb.s, (uint64_t)(rr * 1000 + k)));
							if (rl () >= pLot) continue;
							double w = st.houseW * rl (0.8, 1.25), d = st.houseD * rl (0.8, 1.25);
							int storeys = rl () < st.houseStoreys - 1.0 ? 2 : 1;
							double h = storeys * 2.9 + rl (1.5, 3.5); // (and the roof)
							double x = -0.5 * L + (k + 0.5) * L / n + rl (-0.1, 0.1) * lotW;
							double set = rl (5.0, 9.0) + 0.5 * d;
							double z = rows == 1 ? 0.0 : (rr ? 0.5 * D - set : -0.5 * D + set);
							emit (x, z, std::min (w, 0.85 * L / n), d, h, rr ? 2 : 0, HOUSE, storeys, 2.9);
						}
					break; }
				case LOWRISE: {
					// row buildings along the long streets
					double depth = std::min (rb (10.0, 14.0), 0.45 * D);
					for (int rr = 0; rr < 2; rr++) {
						double x = -0.5 * L;
						int k = 0;
						while (x < 0.5 * L - 8.0) {
							Rnd rl (Hash (rb.s, (uint64_t)(rr * 1000 + k++)));
							double w = std::min (rl (9.0, 20.0), 0.5 * L - x);
							if (rl () < pLot) {
								int ns = (int)floor (rl (st.lowStoreys[0], st.lowStoreys[1] + 0.99));
								double h = st.storey * ns + rl (0.5, 1.5);
								emit (x + 0.5 * w, (rr ? 0.5 : -0.5) * (D - depth), w - rl (0.0, 1.5), depth, h, rr ? 2 : 0, LOWRISE, ns, st.storey);
							}
							x += w;
						}
					}
					break; }
				case MIDRISE:
					if (apartments) {
						// point towers in open ground
						int nx = std::max (1, (int)(L / 55.0)), nz = std::max (1, (int)(D / 55.0));
						for (int a = 0; a < nx; a++)
							for (int c = 0; c < nz; c++) {
								Rnd rl (Hash (rb.s, (uint64_t)(a * 100 + c)));
								if (rl () >= pLot) continue;
								double w = rl (18.0, 28.0), d = rl (16.0, 24.0);
								int ns = (int)floor (rl (10.0, 32.0));
								double h = st.storey * ns + 2.0;
								emit (-0.5 * L + (a + 0.5) * L / nx, -0.5 * D + (c + 0.5) * D / nz, std::min (w, 0.8 * L / nx), std::min (d, 0.8 * D / nz), h, 0, MIDRISE, ns, st.storey);
							}
					} else if (rb () < st.perimeter) {
						// round the block's edges, a courtyard inside
						double depth = std::min (rb (12.0, 16.0), 0.3 * std::min (L, D));
						for (int side = 0; side < 4; side++) {
							bool lng = !(side & 1);
							double len = lng ? L : D - 2.0 * depth;
							double x = -0.5 * len;
							int k = 0;
							while (x < 0.5 * len - 6.0) {
								Rnd rl (Hash (rb.s, (uint64_t)(side * 1000 + k++)));
								double w = std::min (rl (15.0, 40.0), 0.5 * len - x);
								int ns = (int)floor (rl (st.midStoreys[0], st.midStoreys[1] + 0.99));
								double h = st.storey * ns + rl (0.5, 2.0);
								if (rl () < pLot) {
									double c = x + 0.5 * w;
									if (lng) emit (c, (side ? 0.5 : -0.5) * (D - depth), w, depth, h, side ? 2 : 0, MIDRISE, ns, st.storey);
									else emit ((side == 1 ? 0.5 : -0.5) * (L - depth), c, w, depth, h, side == 1 ? 1 : 3, MIDRISE, ns, st.storey);
								}
								x += w;
							}
						}
					} else {
						// slabs
						int n = std::clamp ((int)(L / rb (40.0, 80.0)), 1, 4);
						for (int k = 0; k < n; k++) {
							Rnd rl (Hash (rb.s, (uint64_t)k));
							if (rl () >= pLot) continue;
							double w = L / n * rl (0.6, 0.9), d = D * rl (0.35, 0.8);
							int ns = (int)floor (rl (st.midStoreys[0], st.midStoreys[1] + 0.99));
							double h = st.storey * ns + rl (0.5, 2.0);
							emit (-0.5 * L + (k + 0.5) * L / n, rl (-0.15, 0.15) * (D - d), w, d, h, rl () < 0.5 ? 0 : 2, MIDRISE, ns, st.storey);
						}
					}
					break;
				case TOWER: {
					// parcels with towers: the taller, the nearer the core
					int nx = L > 95.0 ? 2 : 1, nz = D > 95.0 ? 2 : 1;
					for (int a = 0; a < nx; a++)
						for (int c = 0; c < nz; c++) {
							Rnd rl (Hash (rb.s, (uint64_t)(a * 100 + c)));
							double pw = L / nx, pd = D / nz;
							double w = std::max (18.0, pw * rl (0.55, 0.85)), d = std::max (18.0, pd * rl (0.55, 0.85));
							double u = rl ();
							double h = hMax * (0.2 + 0.8 * core) * (0.3 + 0.7 * u * u * u) * (rl () < 0.04 ? 1.8 : 1.0);
							h = std::max (h, st.storey * st.midStoreys[1]);
							const double sh = 3.9; // (offices)
							int ns = std::max (1, (int)((h - 2.0) / sh));
							emit (-0.5 * L + (a + 0.5) * pw, -0.5 * D + (c + 0.5) * pd, std::min (w, pw - 4.0), std::min (d, pd - 4.0), h, 0, TOWER, ns, sh);
						}
					break; }
				case INDUSTRIAL: {
					// sheds
					int n = std::clamp ((int)(L / rb (50.0, 120.0)), 1, 3);
					for (int k = 0; k < n; k++) {
						Rnd rl (Hash (rb.s, (uint64_t)k));
						if (rl () >= pLot) continue;
						double w = L / n * rl (0.6, 0.9), d = D * rl (0.5, 0.85);
						double h = rl (7.0, 14.0);
						emit (-0.5 * L + (k + 0.5) * L / n, rl (-0.1, 0.1) * (D - d), w, d, h, 0, INDUSTRIAL, 1, h - 1.0);
					}
					break; }
				}
	});
}

const std::vector<Street> &CityMap::Streets (int row, int col)
{
	static const std::vector<Street> none;
	if (!active) return none;
	uint64_t key = CellKey (row, col, 0.0);
	auto it = streetCells.find (key);
	if (it != streetCells.end()) { it->second.used = ++clock; return it->second.s; }
	if (streetCells.size() > MAX_CELLS) {
		std::vector<uint64_t> used;
		for (auto &c : streetCells) used.push_back (c.second.used);
		std::nth_element (used.begin(), used.begin() + used.size() / 4, used.end());
		uint64_t limit = used[used.size() / 4];
		for (auto i = streetCells.begin(); i != streetCells.end(); )
			if (i->second.used <= limit) i = streetCells.erase (i); else ++i;
	}
	StreetCell &sc = streetCells[key];
	sc.used = ++clock;
	ElevationManager *emgr = planet->ElevMgr();
	// each block's streets on its lower sides (s0, t0), if it or its
	// neighbour across them is built up (the other sides: the neighbours')
	Blocks (row, col, [&](const Grid &g, int64_t bi, int64_t bj, const Vector &bdir) {
		Zone z;
		bool me = Zoned (g, bi, bj, bdir, z);
		for (int axis = 0; axis < 2; axis++) {
			int64_t ni = axis ? bi : bi - 1, nj = axis ? bj - 1 : bj;
			if (!me && !Zoned (g, ni, nj, g.BlockCentre (ni, nj), z)) continue;
			int64_t k = axis ? bj : bi; // (the line)
			bool avenue = ((k % 5) + 5) % 5 == 0;
			double w = g.st->street * (avenue ? 1.7 : 1.0);
			// along the line: from the block's corner to the next
			double p0 = g.Line (1 - axis, axis ? bi : bj), p1 = g.Line (1 - axis, (axis ? bi : bj) + 1), q = g.Line (axis, k);
			int n = g.st->warp > 0.0 ? std::max (1, (int)ceil ((p1 - p0) / 40.0)) : 1;
			Vector prev;
			for (int i = 0; i <= n; i++) {
				double u = p0 + (p1 - p0) * i / n, x, y;
				if (axis) g.Warp (u, q, x, y); else g.Warp (q, u, x, y);
				Vector d (g.ToDir (x, y));
				double la, lo;
				LatLng (d, la, lo);
				d *= R + (emgr ? emgr->Elevation (la, lo, 0, etile.get()) : 0.0); // (on the ground)
				if (i) {
					// (not over water)
					LatLng ((prev + d).unit(), la, lo);
					if (!Water (la, lo)) sc.s.push_back ({ prev, d, (float)w, (uint8_t)(avenue ? 1 : 0) });
				}
				prev = d;
			}
		}
	});
	return sc.s;
}

void CityMap::Trim ()
{
	if (cells.size() <= MAX_CELLS) return;
	std::vector<uint64_t> used;
	for (auto &c : cells) used.push_back (c.second.used);
	std::nth_element (used.begin(), used.begin() + used.size() / 4, used.end());
	uint64_t limit = used[used.size() / 4];
	for (auto it = cells.begin(); it != cells.end(); )
		if (it->second.used <= limit) it = cells.erase (it); else ++it;
}

const std::vector<Building> &CityMap::Cell (int row, int col, double minHeight)
{
	static const std::vector<Building> none;
	if (!active) return none;
	Trim ();
	uint64_t key = CellKey (row, col, minHeight);
	auto it = cells.find (key);
	if (it == cells.end()) {
		it = cells.emplace (key, CellData ()).first;
		Make (row, col, minHeight, it->second);
	}
	it->second.used = ++clock;
	return it->second.b;
}

void CityMap::Obstacles (const Vector &p, double radius, std::vector<const convex::Polytope*> &out)
{
	out.clear();
	if (!active) return;
	static std::vector<std::pair<int,int>> cl;
	CellsNear (p, radius + 150.0, cl);
	for (auto &rc : cl) {
		Cell (rc.first, rc.second);
		CellData &cd = cells[CellKey (rc.first, rc.second, 0.0)];
		if (!cd.obstDone) {
			cd.obstDone = true;
			for (const Building &b : cd.b) cd.obst.push_back (Box (b));
		}
		for (const convex::Polytope &o : cd.obst)
			if (o.centre.dist (p) < radius + o.radius) out.push_back (&o);
	}
}

} // namespace cities

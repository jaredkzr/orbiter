// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "PlayaMap.h"
#include "Log.h"
#include "GroundScatter.h"
#include "Orbiter.h"
#include "Config.h"
#include "Planet.h"
#include "elevmgr.h"
#include "ZTreeMgr.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iterator>

extern Orbiter *g_pOrbiter;

namespace {
	std::atomic<uint64_t> playaSerial { 1 };

	const size_t MAX_TILES = 32;
	const int TL_N = 8;

	inline double SS (double a, double b, double x)
	{
		double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	struct LinTable {
		float v[256];
		LinTable ()
		{
			for (int i = 0; i < 256; i++) {
				double c = i / 255.0;
				v[i] = (float)(c <= 0.04045 ? c / 12.92 : std::pow ((c + 0.055) / 1.055, 2.4));
			}
		}
	};
	const LinTable lin;

	// thread-local hit cache
	struct TlEntry {
		uint64_t owner = 0;
		int lvl = 0, ilat = 0, ilng = 0;
		std::shared_ptr<const void> tile;
	};
	thread_local TlEntry tlCache[TL_N];
	thread_local int tlNext = 0;
	thread_local TlEntry tlGCache[TL_N];
	thread_local int tlGNext = 0;

	// Ground's filter: Gaussian of sigma GSIGMA nodes, radius GRAD nodes
	const double GSIGMA = 3.0;
	const int GRAD = 9;
	const int GGRID = 256;   // nodes per tile side (the elevation tiles')

	inline uint64_t Key (int lvl, int ilat, int ilng)
	{
		return (uint64_t)lvl << 56 | (uint64_t)(uint32_t)ilat << 28 | (uint64_t)(uint32_t)ilng;
	}
}

double PlayaMap::ColourRule (double r, double g, double b)
{
	double mx = std::max ({ r, g, b }), mn = std::min ({ r, g, b });
	double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
	double sat = (mx - mn) / std::max (mx, 1e-4);
	double rb = std::max (r, b);
	double veg = (g - rb) / std::max (g + rb, 1e-4);
	double blue = (b - r) / std::max (mx, 1e-4);
	return SS (0.45, 0.65, lum) * (1.0 - SS (0.14, 0.20, sat)) * (1.0 - SS (0.02, 0.06, blue)) * (1.0 - SS (0.02, 0.08, veg));
}

double PlayaMap::ContrastRule (double lum, double regional)
{
	if (!(regional > 0.0)) return 1.0;   // (no regional mean: no gate)
	return SS (CONTRAST_0, CONTRAST_1, lum / regional);
}

PlayaMap::PlayaMap (const Planet *pl): planet (pl), radius (pl->Size()), serial (playaSerial++)
{
	DWORD src = g_pOrbiter->Cfg()->CfgPRenderPrm.TileLoadFlags;
	char cbuf[512];
	if (src & 0x0002) {
		g_pOrbiter->Cfg()->PTexPath (cbuf, planet->Name());
		tree = ZTreeMgr::CreateFromFile (cbuf, ZTreeMgr::LAYER_SURF);
	}
	if (src & 0x0001) {
		std::string f = std::string (planet->Name()) + "\\Surf";
		g_pOrbiter->Cfg()->PTexPath (cbuf, f.c_str());
		dir = cbuf;
	}
	hasSource = tree || !dir.empty();
	if (!hasSource)
		LOGOUT_WARN ("%s: no surface textures for the playa rule (PlanetTexDir, TileLoadFlags): no playas, the ground there is not levelled", planet->Name());
}

PlayaMap::~PlayaMap ()
{
	delete tree;
}

bool PlayaMap::Exists (int lvl, int ilat, int ilng)
{
	uint64_t key = Key (lvl, ilat, ilng);
	auto it = exists.find (key);   // (also the tiles that exist but can't be decoded: remembered as missing)
	if (it != exists.end()) return it->second;
	if (tree && tree->Idx (lvl, ilat, ilng) != (DWORD)-1) return true;
	if (dir.empty()) return false;
	char fname[64];
	snprintf (fname, sizeof(fname), "\\%02d\\%06d\\%06d.dds", lvl, ilat, ilng);
	std::string path = dir + fname;
	for (char &c : path) if (c == '\\') c = '/';
	bool ok = std::ifstream (path, std::ios::binary).good();
	exists[key] = ok;
	return ok;
}

bool PlayaMap::Decode (int lvl, int ilat, int ilng, std::vector<uint32_t> &px, int &w, int &h)
{
	bool ok = false;
	if (tree) {
		BYTE *buf = nullptr;
		DWORD n;
		{
			std::lock_guard<std::mutex> lock (ioMtx);   // (one file handle)
			n = tree->ReadData (lvl, ilat, ilng, &buf);
		}
		if (n) {
			ok = scatter::DecodeDXT1 (buf, n, px, w, h);
			std::lock_guard<std::mutex> lock (ioMtx);
			tree->ReleaseData (buf);
		}
	}
	if (!ok && !dir.empty()) {
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
	return ok;
}

void PlayaMap::BuildRegion ()
{
	// mean land luminance of the REGION_LEVEL texels, per cell of REGION_DOWN^2 texels, then box
	// filtered over (2 REGION_BOX + 1)^2 cells (wrapping in longitude)
	const int nlat = 1 << (REGION_LEVEL - 4), nlng = 2 << (REGION_LEVEL - 4);
	std::vector<double> sl, sn;
	int tw = 0, th = 0;
	for (int ilat = 0; ilat < nlat; ilat++)
		for (int ilng = 0; ilng < nlng; ilng++) {
			std::vector<uint32_t> px;
			int w = 0, h = 0;
			if (!Decode (REGION_LEVEL, ilat, ilng, px, w, h)) continue;
			if (!tw) {
				tw = w, th = h;
				regW = nlng * (w / REGION_DOWN), regH = nlat * (h / REGION_DOWN);
				sl.assign ((size_t)regW * regH, 0.0), sn.assign ((size_t)regW * regH, 0.0);
			}
			if (w != tw || h != th) continue;
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) {
					uint32_t c = px[(size_t)y * w + x];
					if ((c >> 24) == 0) continue;
					double l = 0.2126 * lin.v[c & 255] + 0.7152 * lin.v[(c >> 8) & 255] + 0.0722 * lin.v[(c >> 16) & 255];
					if (l < LAND_LUM) continue;
					size_t k = (size_t)(ilat * (h / REGION_DOWN) + y / REGION_DOWN) * regW + ilng * (w / REGION_DOWN) + x / REGION_DOWN;
					sl[k] += l, sn[k] += 1.0;
				}
		}
	if (!regW) return;
	region.assign ((size_t)regW * regH, std::nanf (""));
	for (int y = 0; y < regH; y++)
		for (int x = 0; x < regW; x++) {
			double a = 0.0, n = 0.0;
			for (int j = -REGION_BOX; j <= REGION_BOX; j++) {
				int yy = std::clamp (y + j, 0, regH - 1);
				for (int i = -REGION_BOX; i <= REGION_BOX; i++) {
					int xx = (x + i + regW) % regW;
					a += sl[(size_t)yy * regW + xx], n += sn[(size_t)yy * regW + xx];
				}
			}
			if (n > 0.0) region[(size_t)y * regW + x] = (float)(a / n);
		}
}

double PlayaMap::Region (double lat, double lng)
{
	std::call_once (regionOnce, [this]() { BuildRegion (); });
	if (region.empty()) return std::nan ("");
	double X = (lng + Pi) / Pi2 * regW - 0.5, Y = std::clamp ((Pi05 - lat) / Pi * regH - 0.5, 0.0, regH - 1.0);
	X = std::remainder (X, (double)regW);
	if (X < 0.0) X += regW;
	int x0 = (int)X % regW, y0 = std::min ((int)Y, regH - 1);
	int x1 = (x0 + 1) % regW, y1 = std::min (y0 + 1, regH - 1);
	double fx = X - std::floor (X), fy = Y - y0;
	double v[4] = { region[(size_t)y0 * regW + x0], region[(size_t)y0 * regW + x1], region[(size_t)y1 * regW + x0], region[(size_t)y1 * regW + x1] };
	double a = 0.0, wsum = 0.0, wt[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
	for (int i = 0; i < 4; i++) if (std::isfinite (v[i])) a += wt[i] * v[i], wsum += wt[i];   // (cells without land: left out)
	return wsum > 0.0 ? a / wsum : std::nan ("");
}

std::shared_ptr<const PlayaMap::Tile> PlayaMap::Load (int lvl, int ilat, int ilng)
{
	std::vector<uint32_t> px;
	int w = 0, h = 0;
	if (!Decode (lvl, ilat, ilng, px, w, h)) return nullptr;
	auto t = std::make_shared<Tile>();
	t->lvl = lvl, t->ilat = ilat, t->ilng = ilng, t->w = w, t->h = h;
	t->c.resize ((size_t)w * h);
	const int nlat = 1 << (lvl - 4), nlng = 2 << (lvl - 4);
	for (size_t i = 0; i < t->c.size(); i++) {
		uint32_t c = px[i];
		if ((c >> 24) == 0) { t->c[i] = 0; continue; } // (transparent)
		double r = lin.v[c & 255], g = lin.v[(c >> 8) & 255], b = lin.v[(c >> 16) & 255];
		double v = ColourRule (r, g, b);
		if (v > 0.0) {   // (the region only where the colour says playa: most tiles have none)
			int x = (int)(i % w), y = (int)(i / w);
			double lat = Pi05 - ((double)ilat * h + y + 0.5) / ((double)nlat * h) * Pi;
			double lng = -Pi + ((double)ilng * w + x + 0.5) / ((double)nlng * w) * Pi2;
			v *= ContrastRule (0.2126 * r + 0.7152 * g + 0.0722 * b, Region (lat, lng));
		}
		t->c[i] = (uint8_t)std::lround (std::clamp (v, 0.0, 1.0) * 255.0);
	}
	return t;
}

double PlayaMap::texel (int lvl) const
{
	return radius * Pi / (std::ldexp (1.0, lvl - 4) * 512.0);
}

std::shared_ptr<const PlayaMap::Tile> PlayaMap::Get (int lvl, int ilat, int ilng)
{
	{
		std::lock_guard<std::mutex> lock (mtx);
		for (auto it = lru.begin(); it != lru.end(); ++it)
			if ((*it)->lvl == lvl && (*it)->ilat == ilat && (*it)->ilng == ilng) {
				lru.splice (lru.begin(), lru, it);
				return lru.front();
			}
		if (!Exists (lvl, ilat, ilng)) return nullptr;
	}
	std::shared_ptr<const Tile> t = Load (lvl, ilat, ilng);   // (not under the lock)
	std::lock_guard<std::mutex> lock (mtx);
	if (!t) {   // (missing, or not decodable: remembered, not read again for every query)
		exists[Key (lvl, ilat, ilng)] = false;
		return nullptr;
	}
	for (const auto &o : lru)   // (another thread was first: use its tile)
		if (o->lvl == lvl && o->ilat == ilat && o->ilng == ilng) return o;
	lru.push_front (t);
	if (lru.size() > MAX_TILES) lru.pop_back();
	return t;
}

const PlayaMap::Tile *PlayaMap::Fetch (int lvl, int ilat, int ilng, std::shared_ptr<const Tile> &hold)
{
	// (a hit costs no reference count: threads would fight over the shared tile's)
	for (TlEntry &e : tlCache)
		if (e.owner == serial && e.lvl == lvl && e.ilat == ilat && e.ilng == ilng && e.tile)
			return static_cast<const Tile*> (e.tile.get());
	hold = Get (lvl, ilat, ilng);
	if (hold) {
		TlEntry &e = tlCache[tlNext];
		tlNext = (tlNext + 1) & (TL_N - 1);
		e.owner = serial, e.lvl = lvl, e.ilat = ilat, e.ilng = ilng;
		e.tile = hold;
	}
	return hold.get();
}

std::shared_ptr<const PlayaMap::Tile> PlayaMap::Pin (const Tile *t)
{
	for (TlEntry &e : tlCache)
		if (e.owner == serial && e.tile.get() == t) return std::static_pointer_cast<const Tile> (e.tile);
	return nullptr;
}

double PlayaMap::Colour (double lat, double lng, double spacing)
{
	if (!hasSource) return 0.0;
	lat = std::clamp (lat, -Pi05, Pi05);
	// the finest level <= PLAYA_LEVEL with a tile there
	std::shared_ptr<const Tile> hold[4];   // (the slow paths' tiles)
	const Tile *own = nullptr;
	int L = PLAYA_LEVEL;
	while (L > 4 && spacing > 0.0 && texel (L) < 0.5 * spacing) L--;   // (coarse data: coarse texels)
	for (; L >= 4; L--) {
		int nlat = 1 << (L - 4), nlng = 2 << (L - 4);
		int ilat = std::clamp ((int)((Pi05 - lat) / Pi * nlat), 0, nlat - 1);
		int ilng = std::clamp ((int)((lng + Pi) / Pi2 * nlng), 0, nlng - 1);
		own = Fetch (L, ilat, ilng, hold[0]);
		if (own) break;
	}
	if (!own) return 0.0;
	const int nlat = 1 << (L - 4), nlng = 2 << (L - 4);
	const int W = own->w, H = own->h;
	const double NX = (double)nlng * W, NY = (double)nlat * H;
	double X = (lng + Pi) / Pi2 * NX - 0.5;
	double Y = std::clamp ((Pi05 - lat) / Pi * NY - 0.5, 0.0, NY - 1.0);
	X = std::remainder (X, NX);
	if (X < 0.0) X += NX;
	double fx0 = std::floor (X), fy0 = std::floor (Y);
	double fx = X - fx0, fy = Y - fy0;
	long x0 = (long)fx0, y0 = (long)fy0;
	long x[2] = { x0, (x0 + 1) % (long)NX }, y[2] = { y0, std::min (y0 + 1, (long)NY - 1) };
	double v[2][2];
	int nh = 1;
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++) {
			int ti = (int)(x[i] / W), tj = (int)(y[j] / H);
			const Tile *t = own;
			if (ti != own->ilng || tj != own->ilat) {
				if (!hold[0]) hold[0] = Pin (own);   // (the fetches below may replace its cache entry)
				t = Fetch (L, tj, ti, hold[nh < 4 ? nh : 3]);
				nh++;
			}
			int lx, ly;
			if (t && t->w == W && t->h == H) lx = (int)(x[i] % W), ly = (int)(y[j] % H);
			else {
				t = own;
				lx = std::clamp ((int)(x[i] - (long)own->ilng * W), 0, W - 1);
				ly = std::clamp ((int)(y[j] - (long)own->ilat * H), 0, H - 1);
			}
			v[j][i] = t->c[(size_t)ly * t->w + lx] * (1.0 / 255.0);
		}
	return (v[0][0] * (1.0 - fx) + v[0][1] * fx) * (1.0 - fy) + (v[1][0] * (1.0 - fx) + v[1][1] * fx) * fy;
}

// ---------------------------------------------------------------------
// reference ground

std::shared_ptr<const PlayaMap::GTile> PlayaMap::BuildG (int ilat, int ilng)
{
	const ElevationManager *em = planet->ElevMgr();
	if (!em) return nullptr;
	const int S = ElevationManager::RefStride();
	const int nlat = 1 << (PLAYA_REF_LEVEL - 4), nlng = 2 << (PLAYA_REF_LEVEL - 4);
	const long NY = (long)nlat * GGRID, NX = (long)nlng * GGRID;
	const int P = S + 2 * GRAD;   // patch side: nodes -1-GRAD to 257+GRAD
	const int o = 1 + GRAD;       // patch index of node 0

	// raw tiles, loaded as the patch needs them
	struct Raw { int tj, ti; bool ok; std::vector<float> d; };
	std::deque<Raw> raws;   // (references stay valid)
	auto GetRaw = [&](int tj, int ti) -> const Raw & {
		for (const Raw &r : raws) if (r.tj == tj && r.ti == ti) return r;
		raws.push_back (Raw { tj, ti, false, {} });
		Raw &r = raws.back();
		r.ok = em->RefTile (PLAYA_REF_LEVEL, tj, ti, r.d);
		return r;
	};
	const Raw &centre = GetRaw (ilat, ilng);
	if (!centre.ok) return nullptr;
	const std::vector<float> &cd = centre.d;

	std::vector<float> patch ((size_t)P * P);
	for (int py = 0; py < P; py++) {
		int jy = py - o;
		long N = std::clamp ((long)ilat * GGRID + jy, 0L, NY);
		int tj = (int)std::min (N / GGRID, (long)nlat - 1);
		int r = (int)((long)(tj + 1) * GGRID - N);           // (rows of a tile run south to north)
		for (int px = 0; px < P; px++) {
			int jx = px - o;
			long gx = ((long)ilng * GGRID + jx) % NX;
			if (gx < 0) gx += NX;
			int ti = (int)(gx / GGRID), c = (int)(gx % GGRID);
			const Raw &rw = GetRaw (tj, ti);
			float v;
			if (rw.ok) v = rw.d[(size_t)(r + 1) * S + c + 1];
			else v = cd[(size_t)(std::clamp (GGRID - jy, 0, GGRID) + 1) * S + std::clamp (jx, 0, GGRID) + 1];
			patch[(size_t)py * P + px] = v;
		}
	}

	// separable Gaussian
	std::vector<double> k (2 * GRAD + 1);
	double sum = 0.0;
	for (int i = -GRAD; i <= GRAD; i++) sum += (k[i + GRAD] = std::exp (-0.5 * i * i / (GSIGMA * GSIGMA)));
	for (double &w : k) w /= sum;
	std::vector<float> hp ((size_t)P * S);   // horizontal pass: all rows, the tile's columns
	for (int y = 0; y < P; y++)
		for (int x = 0; x < S; x++) {
			const float *src = &patch[(size_t)y * P + x];   // (x + GRAD - GRAD: patch column of out column x, minus the radius)
			double a = 0.0;
			for (int i = 0; i <= 2 * GRAD; i++) a += k[i] * src[i];
			hp[(size_t)y * S + x] = (float)a;
		}
	auto t = std::make_shared<GTile>();
	t->ilat = ilat, t->ilng = ilng;
	t->g.resize ((size_t)S * S);
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++) {
			double a = 0.0;
			for (int i = 0; i <= 2 * GRAD; i++) a += k[i] * hp[(size_t)(y + i) * S + x];
			t->g[(size_t)y * S + x] = (float)a;
		}
	return t;
}

std::shared_ptr<const PlayaMap::GTile> PlayaMap::GetG (int ilat, int ilng)
{
	{
		std::lock_guard<std::mutex> lock (gmtx);
		if (gnone.count (Key (PLAYA_REF_LEVEL, ilat, ilng))) return nullptr;   // (no data: permanent)
		for (auto it = glru.begin(); it != glru.end(); ++it)
			if ((*it)->ilat == ilat && (*it)->ilng == ilng) {
				glru.splice (glru.begin(), glru, it);
				return glru.front();
			}
	}
	std::shared_ptr<const GTile> t = BuildG (ilat, ilng);   // (not under the lock)
	std::lock_guard<std::mutex> lock (gmtx);
	if (!t) { gnone.insert (Key (PLAYA_REF_LEVEL, ilat, ilng)); return nullptr; }
	for (const auto &o : glru)   // (another thread was first: use its tile)
		if (o->ilat == ilat && o->ilng == ilng) return o;
	glru.push_front (t);
	if (glru.size() > MAX_TILES) glru.pop_back();
	return t;
}

const PlayaMap::GTile *PlayaMap::FetchG (int ilat, int ilng, std::shared_ptr<const GTile> &hold)
{
	for (TlEntry &e : tlGCache)
		if (e.owner == serial && e.ilat == ilat && e.ilng == ilng && e.tile)
			return static_cast<const GTile*> (e.tile.get());
	hold = GetG (ilat, ilng);
	if (hold) {
		TlEntry &e = tlGCache[tlGNext];
		tlGNext = (tlGNext + 1) & (TL_N - 1);
		e.owner = serial, e.ilat = ilat, e.ilng = ilng;
		e.tile = hold;
	}
	return hold.get();
}

double PlayaMap::Ground (double lat, double lng)
{
	if (!std::isfinite (lat) || !std::isfinite (lng)) return std::nan ("");
	lat = std::clamp (lat, -Pi05, Pi05);
	lng = std::remainder (lng, Pi2);
	const int nlat = 1 << (PLAYA_REF_LEVEL - 4), nlng = 2 << (PLAYA_REF_LEVEL - 4);
	const double Y = (Pi05 - lat) / Pi * nlat * GGRID;
	const double X = (lng + Pi) / Pi2 * nlng * GGRID;
	int ilat = std::clamp ((int)(Y / GGRID), 0, nlat - 1);
	int ilng = std::clamp ((int)(X / GGRID), 0, nlng - 1);
	std::shared_ptr<const GTile> hold;
	const GTile *t = FetchG (ilat, ilng, hold);
	if (!t) return std::nan ("");
	const int S = ElevationManager::RefStride();
	double ly = std::clamp (Y - (double)ilat * GGRID, 0.0, (double)GGRID);
	double lx = std::clamp (X - (double)ilng * GGRID, 0.0, (double)GGRID);
	int iy = std::min ((int)ly, GGRID - 1), ix = std::min ((int)lx, GGRID - 1);
	double ty = ly - iy, tx = lx - ix;
	auto cr = [](double a, double b, double c, double d, double u) { // Catmull-Rom
		return 0.5 * (2.0 * b + u * (-a + c) + u * u * (2.0 * a - 5.0 * b + 4.0 * c - d) + u * u * u * (-a + 3.0 * b - 3.0 * c + d));
	};
	double col[4];
	for (int a = 0; a < 4; a++) {
		const float *row = &t->g[(size_t)(iy + a) * S + ix];   // (node iy-1+a, ix-1: the tile's border is node -1)
		col[a] = cr (row[0], row[1], row[2], row[3], tx);
	}
	return cr (col[0], col[1], col[2], col[3], ty);
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "SeaSwell.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace sea {

namespace {

	const double PI_ = 3.14159265358979323846;
	const double DEG = PI_ / 180.0;
	const double PM_ALPHA = 8.1e-3; // Phillips' constant
	const size_t CACHE_CELLS = 64;
	std::mutex cacheLock;

	uint64_t SplitMix (uint64_t &s)
	{
		uint64_t z = (s += 0x9e3779b97f4a7c15ull);
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}
	double Uniform (uint64_t &s) { return (SplitMix (s) >> 11) * (1.0 / 9007199254740992.0); }

	// cos^2 spreading over (-pi/2, pi/2): the angle at which the cumulative share is q
	double SpreadQuantile (double q)
	{
		double lo = -PI_ / 2.0, hi = PI_ / 2.0;
		for (int i = 0; i < 50; i++) {
			double m = 0.5 * (lo + hi);
			double F = (m + sin (m) * cos (m)) / PI_ + 0.5;
			(F < q ? lo : hi) = m;
		}
		return 0.5 * (lo + hi);
	}

}

void Swell::Setup (const SwellParams &p)
{
	prm = p;
	prm.wind = std::clamp (prm.wind, 0.0, 25.0);
	cache.clear();
	bins.clear();
	calm = true;
	hs = hsFull = tp = crestMax = lambdaMax = 0.0;
	nrows = (int)std::lround (180.0 / CELL_DEG);
	const double g = prm.g;
	if (prm.wind < 1.0 || g <= 0.0) return; // (glassy)
	// Pierson-Moskowitz: S(w) = alpha g^2 w^-5 exp (-5/4 (wp/w)^4), with the
	// peak wp = 0.877 g / U19.5, U19.5 = 1.06 U10 (as the client's waves)
	double U = 1.06 * prm.wind;
	double wp = 0.877 * g / U;
	tp = 2.0 * PI_ / wp;
	hsFull = 4.0 * sqrt (PM_ALPHA / 5.0) * g / (wp * wp);
	auto S = [&](double w) { return PM_ALPHA * g * g * pow (w, -5.0) * exp (-1.25 * pow (wp / w, 4.0)); };
	wlo = 0.6 * wp;
	whi = std::min (sqrt (2.0 * PI_ * g / std::max (prm.lambdaMin, 0.1)), 5.0 * wp);
	if (whi < 1.2 * wlo) return; // (the whole sea shorter than the swell's band)
	double var = 0.0;
	for (int i = 0; i < NFREQ; i++) {
		Bin b;
		b.w0 = wlo * pow (whi / wlo, (double)i / NFREQ);
		b.w1 = wlo * pow (whi / wlo, (double)(i + 1) / NFREQ);
		// (the bin's variance: Simpson's rule)
		const int n = 16;
		double h = (b.w1 - b.w0) / n, sum = S (b.w0) + S (b.w1);
		for (int j = 1; j < n; j++) sum += S (b.w0 + j * h) * (j % 2 ? 4.0 : 2.0);
		b.S = sum * h / 3.0;
		var += b.S;
		bins.push_back (b);
	}
	hs = 4.0 * sqrt (var);
	for (const Bin &b : bins) crestMax += NDIR * sqrt (2.0 * b.S / NDIR);
	lambdaMax = 2.0 * PI_ * g / (wlo * wlo);
	calm = false;
}

int Swell::RowCols (int row) const
{
	double latc = (-90.0 + (row + 0.5) * CELL_DEG) * DEG;
	return std::max (1, (int)floor (360.0 * cos (latc) / CELL_DEG));
}

void Swell::Rows (double lat, int &r0, int &r1, double &w1) const
{
	double v = (lat / DEG + 90.0) / CELL_DEG - 0.5;
	r0 = (int)floor (v);
	double fr = v - r0;
	r1 = r0 + 1;
	w1 = SmoothStep (0.5 - BLEND, 0.5 + BLEND, fr);
	if (r0 < 0) r0 = r1 = 0, w1 = 0.0;
	else if (r1 >= nrows) r0 = r1 = nrows - 1, w1 = 0.0;
}

void Swell::Cols (int row, double lng, int &c0, int &c1, double &w1) const
{
	int nc = RowCols (row);
	double u = (lng / DEG + 180.0) / 360.0 * nc - 0.5;
	int c = (int)floor (u);
	double fr = u - c;
	w1 = SmoothStep (0.5 - BLEND, 0.5 + BLEND, fr);
	c0 = ((c % nc) + nc) % nc;
	c1 = (c0 + 1) % nc;
	if (nc == 1) w1 = 0.0;
}

const Swell::Cell &Swell::GetCell (int row, int col) const
{
	// (the caller holds cacheLock)
	for (Entry &e : cache)
		if (e.row == row && e.col == col) { e.used = ++clock; return e.cell; }
	Entry ne;
	ne.row = row, ne.col = col, ne.used = ++clock;
	Cell &c = ne.cell;
	int nc = RowCols (row);
	double lat = (-90.0 + (row + 0.5) * CELL_DEG) * DEG;
	double lng = (-180.0 + (col + 0.5) * 360.0 / nc) * DEG;
	double slat = sin (lat), clat = cos (lat), slng = sin (lng), clng = cos (lng);
	c.e = Vector (-slng, 0.0, clng);
	c.n = Vector (-slat * clng, clat, -slat * slng);
	uint64_t s = ((uint64_t)prm.seed << 32) ^ ((uint64_t)(uint32_t)row << 16) ^ (uint64_t)(uint32_t)col ^ 0x5eaull;
	SplitMix (s);
	c.comp.reserve (bins.size() * NDIR);
	for (const Bin &b : bins) {
		for (int j = 0; j < NDIR; j++) {
			Comp k;
			// (a frequency within the bin, a direction within its stratum of the spreading)
			double w = b.w0 + (b.w1 - b.w0) * Uniform (s);
			double th = SpreadQuantile ((j + Uniform (s)) / NDIR);
			k.w = w;
			k.k = w * w / prm.g;
			k.a = sqrt (2.0 * b.S / NDIR);
			double dir = prm.bearing + th;
			k.dx = sin (dir), k.dz = cos (dir);
			k.phase = 2.0 * PI_ * Uniform (s);
			c.comp.push_back (k);
		}
	}
	if (cache.size() < CACHE_CELLS) {
		cache.push_back (std::move (ne));
		return cache.back().cell;
	}
	auto lru = std::min_element (cache.begin(), cache.end(), [](const Entry &a, const Entry &b) { return a.used < b.used; });
	*lru = std::move (ne);
	return lru->cell;
}

template<bool FULL>
void Swell::Accumulate (const Cell &c, double w, const Vector &u, double t, double z, SwellSample &s) const
{
	const double R = prm.radius;
	double x = R * dotp (u, c.e), y = R * dotp (u, c.n);
	double zc = std::min (z, 0.0);
	for (const Comp &k : c.comp) {
		double ph = k.k * (k.dx * x + k.dz * y) - fmod (k.w * t, 2.0 * PI_) + k.phase;
		double cp = cos (ph);
		s.h += w * k.a * cp;
		if (FULL) {
			double sp = sin (ph);
			double ak = w * k.a * k.k;
			s.sx -= ak * k.dx * sp;
			s.sz -= ak * k.dz * sp;
			double v = w * k.a * k.w * (zc < 0.0 ? exp (k.k * zc) : 1.0);
			s.vel.x += v * cp * k.dx;
			s.vel.z += v * cp * k.dz;
			s.vel.y += v * sp;
		}
	}
}

template<bool FULL>
void Swell::Eval (const Vector &u, double t, double z, SwellSample &s) const
{
	s = SwellSample();
	if (calm) return;
	double lat = asin (std::clamp (u.y, -1.0, 1.0)), lng = atan2 (u.z, u.x);
	int r[2];
	double wr;
	Rows (lat, r[0], r[1], wr);
	double wrow[2] = { 1.0 - wr, wr };
	struct W { int row, col; double w; } cells[4];
	int n = 0;
	double w2 = 0.0;
	for (int i = 0; i < 2; i++) {
		if (wrow[i] <= 0.0 || (i == 1 && r[1] == r[0])) continue;
		int c0, c1;
		double wc;
		Cols (r[i], lng, c0, c1, wc);
		if (1.0 - wc > 0.0) cells[n++] = { r[i], c0, wrow[i] * (1.0 - wc) };
		if (wc > 0.0 && c1 != c0) cells[n++] = { r[i], c1, wrow[i] * wc };
	}
	for (int i = 0; i < n; i++) w2 += cells[i].w * cells[i].w;
	double norm = w2 > 0.0 ? 1.0 / sqrt (w2) : 0.0;
	// (the point's own east and north: the cells' frames turned onto them)
	double slat = sin (lat), clat = cos (lat), slng = sin (lng), clng = cos (lng);
	Vector ep (-slng, 0.0, clng), np (-slat * clng, clat, -slat * slng);
	std::lock_guard<std::mutex> lk (cacheLock);
	for (int i = 0; i < n; i++) {
		const Cell &c = GetCell (cells[i].row, cells[i].col);
		if (!FULL) { Accumulate<false> (c, cells[i].w * norm, u, t, z, s); continue; }
		SwellSample cs;
		Accumulate<true> (c, cells[i].w * norm, u, t, z, cs);
		double ee = dotp (ep, c.e), en = dotp (ep, c.n), ne = dotp (np, c.e), nn = dotp (np, c.n);
		s.h += cs.h;
		s.sx += cs.sx * ee + cs.sz * en;
		s.sz += cs.sx * ne + cs.sz * nn;
		s.vel.x += cs.vel.x * ee + cs.vel.z * en;
		s.vel.z += cs.vel.x * ne + cs.vel.z * nn;
		s.vel.y += cs.vel.y;
	}
}

void Swell::Sample (const Vector &u, double t, double z, SwellSample &s) const
{
	Eval<true> (u, t, z, s);
}

void Swell::Components (const Vector &u, double t, std::vector<Comp2> &out) const
{
	out.clear();
	if (calm) return;
	double lat = asin (std::clamp (u.y, -1.0, 1.0)), lng = atan2 (u.z, u.x);
	int r[2];
	double wr;
	Rows (lat, r[0], r[1], wr);
	double wrow[2] = { 1.0 - wr, wr };
	struct W { int row, col; double w; } cells[4];
	int n = 0;
	for (int i = 0; i < 2; i++) {
		if (wrow[i] <= 0.0 || (i == 1 && r[1] == r[0])) continue;
		int c0, c1;
		double wc;
		Cols (r[i], lng, c0, c1, wc);
		if (1.0 - wc > 0.0) cells[n++] = { r[i], c0, wrow[i] * (1.0 - wc) };
		if (wc > 0.0 && c1 != c0) cells[n++] = { r[i], c1, wrow[i] * wc };
	}
	double w2 = 0.0;
	for (int i = 0; i < n; i++) w2 += cells[i].w * cells[i].w;
	double norm = w2 > 0.0 ? 1.0 / sqrt (w2) : 0.0;
	double slat = sin (lat), clat = cos (lat), slng = sin (lng), clng = cos (lng);
	Vector ep (-slng, 0.0, clng), np (-slat * clng, clat, -slat * slng);
	const double R = prm.radius;
	std::lock_guard<std::mutex> lk (cacheLock);
	for (int i = 0; i < n; i++) {
		const Cell &c = GetCell (cells[i].row, cells[i].col);
		double ee = dotp (ep, c.e), en = dotp (ep, c.n), ne = dotp (np, c.e), nn = dotp (np, c.n);
		double x = R * dotp (u, c.e), y = R * dotp (u, c.n);
		for (const Comp &k : c.comp) {
			Comp2 o;
			o.a = k.a * cells[i].w * norm;
			o.k = k.k;
			// (its direction in the reference's east and north)
			double dx = k.dx * ee + k.dz * en, dz = k.dx * ne + k.dz * nn;
			double dl = std::hypot (dx, dz);
			o.dx = dx / dl, o.dz = dz / dl;
			o.phase = fmod (k.k * (k.dx * x + k.dz * y) - fmod (k.w * t, 2.0 * PI_) + k.phase, 2.0 * PI_);
			out.push_back (o);
		}
	}
}

double Swell::Height (const Vector &u, double t) const
{
	SwellSample s;
	Eval<false> (u, t, 0.0, s);
	return s.h;
}

} // namespace sea

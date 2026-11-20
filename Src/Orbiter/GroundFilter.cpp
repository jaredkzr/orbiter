// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "GroundFilter.h"
#include "Planet.h"
#include "elevmgr.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <thread>

namespace {
	std::atomic<uint64_t> filterSerial{ 1 };
}

// an elevation manager of the data alone, used by one thread at a time
struct GroundFilter::RawElev {
	std::mutex m;
	std::unique_ptr<ElevationManager> em;
	std::unique_ptr<std::vector<ElevationTile>> tiles;
};

GroundFilter::GroundFilter (const Planet *p, const Params &prm_)
	: planet (p), R (p->Size()), prm (prm_), serial (filterSerial++)
{}

GroundFilter::~GroundFilter () {}

GroundFilter::NodeVal GroundFilter::Node (int64_t i, int64_t j)
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
	// n x n samples about the node; a low percentile of the land's
	double lat = i * prm.step, lng = j * prm.step;
	double dlat = prm.spacing / R, dlng = prm.spacing / (R * std::max (cos (lat), 0.01));
	thread_local std::vector<float> e, ea, eb;
	e.clear (), ea.clear (), eb.clear ();
	const int h = prm.n / 2, ns = (2 * h + 1) * (2 * h + 1);
	for (int a = -h; a <= h; a++)
		for (int b = -h; b <= h; b++) {
			double v = re->em->Elevation (lat + a * dlat, lng + b * dlng, 0, re->tiles.get());
			if (fabs (v) > 0.25) e.push_back ((float)v), ea.push_back ((float)a), eb.push_back ((float)b); // (not the sea)
		}
	re->m.unlock ();
	float g = std::numeric_limits<float>::quiet_NaN ();
	const int n = (int)e.size ();
	// The ground of the samples picked (sel: 0 all, 1-4 the half of them
	// north, south, west, east of the node, with its own row or column):
	// false if they are too few. rms: their scatter about their plane [m]
	thread_local std::vector<float> se, sa_, sb_, res;
	auto ground = [&](int sel, float &gs, double &rms) -> bool {
		se.clear (), sa_.clear (), sb_.clear ();
		for (int q = 0; q < n; q++) {
			if ((sel == 1 && ea[q] > 0) || (sel == 2 && ea[q] < 0) || (sel == 3 && eb[q] > 0) || (sel == 4 && eb[q] < 0)) continue;
			se.push_back (e[q]), sa_.push_back (ea[q]), sb_.push_back (eb[q]);
		}
		const int m = (int)se.size ();
		if (m < (sel ? std::max (prm.minLand / 2, 6) : prm.minLand)) return false;
		const int k = std::clamp ((int)(m * prm.percentile), 0, m - 1);
		// the samples' plane v = c0 + ca a + cb b (least squares; a, b in spacings)
		double w = 0.0, c0 = 0.0, ca = 0.0, cb = 0.0;
		rms = 0.0;
		if (prm.slope1 > 0.0) {
			double sa = 0, sb = 0, sv = 0, saa = 0, sbb = 0, sab = 0, sav = 0, sbv = 0;
			for (int q = 0; q < m; q++) {
				sa += sa_[q], sb += sb_[q], sv += se[q];
				saa += sa_[q] * sa_[q], sbb += sb_[q] * sb_[q], sab += sa_[q] * sb_[q];
				sav += sa_[q] * se[q], sbv += sb_[q] * se[q];
			}
			// (about the samples' centroid)
			const double ma = sa / m, mb = sb / m, mv = sv / m;
			const double Saa = saa - m * ma * ma, Sbb = sbb - m * mb * mb, Sab = sab - m * ma * mb;
			const double Sav = sav - m * ma * mv, Sbv = sbv - m * mb * mv;
			const double det = Saa * Sbb - Sab * Sab;
			if (det > 1e-6 * std::max (Saa * Sbb, 1.0)) {
				ca = (Sav * Sbb - Sbv * Sab) / det, cb = (Sbv * Saa - Sav * Sab) / det;
				c0 = mv - ca * ma - cb * mb;
				const double slope = std::hypot (ca, cb) / prm.spacing;
				w = std::clamp ((slope - prm.slope0) / std::max (prm.slope1 - prm.slope0, 1e-9), 0.0, 1.0);
				w = w * w * (3.0 - 2.0 * w);
			} else c0 = mv;
			res.resize (m);
			for (int q = 0; q < m; q++) {
				res[q] = (float)(se[q] - (c0 + ca * sa_[q] + cb * sb_[q]));
				rms += (double)res[q] * res[q];
			}
			rms = sqrt (rms / m);
		}
		float gd = 0.0f;
		if (w > 0.0) { // (the heights over the plane)
			std::nth_element (res.begin(), res.begin() + k, res.end());
			gd = (float)(c0 + res[k]);
		}
		std::nth_element (se.begin(), se.begin() + k, se.end());
		gs = (float)(se[k] + (gd - se[k]) * w);
		return true;
	};
	if (n >= prm.minLand) {
		double rms = 0.0;
		ground (0, g, rms);
		// A step in the ground (a plateau's cliff, a river's bank) is no
		// plane: across it the samples scatter by a share of its height, and
		// its upper edge came out metres low. Where they scatter this much
		// (more than a canopy does), the ground of the half of them about
		// the node that scatters least, if by much less than all of them:
		// the node's own side of the step
		if (prm.step0 > 0.0 && rms > prm.step0) {
			float gb = g;
			double rb = rms;
			for (int sel = 1; sel <= 4; sel++) {
				float gs;
				double rs;
				if (ground (sel, gs, rs) && rs < rb) gb = gs, rb = rs;
			}
			if (rb < 0.6 * rms) {
				double w = std::clamp ((rms - prm.step0) / std::max (prm.step1 - prm.step0, 1e-9), 0.0, 1.0);
				g = (float)(g + (gb - g) * w);
			}
		}
	}
	return { g, (float)n / ns };
}

std::shared_ptr<const GroundFilter::Block> GroundFilter::MakeBlock (int64_t bi, int64_t bj)
{
	// The nodes in parallel (the pool of data-only elevation managers:
	// one per thread at a time). ORBITER_CITY_GROUND_SYNC=1: on the calling
	// thread (threads crashed here in ElevationManager::Elevation once, at
	// Sriharikota; not reproduced since, 2026-10-30)
	auto blk = std::make_shared<Block> ();
	auto rows = [&](int r0, int r1) {
		for (int r = r0; r < r1; r++)
			for (int c = 0; c < GBLOCK; c++) (*blk)[r * GBLOCK + c] = Node (bi * GBLOCK + r, bj * GBLOCK + c);
	};
	static const bool sync = getenv ("ORBITER_CITY_GROUND_SYNC") && atoi (getenv ("ORBITER_CITY_GROUND_SYNC"));
	// (one thread a block while others make blocks too, e.g. a client's
	// mesh workers: each thread of each takes an elevation manager, whose
	// tiles it loads anew)
	const bool alone = making.load () <= 1;
	const int nt = sync || !alone ? 1 : (int)std::clamp (std::thread::hardware_concurrency () / 2, 1u, 4u);
	std::vector<std::future<void>> fs;
	for (int k = 1; k < nt; k++)
		fs.push_back (std::async (std::launch::async, rows, GBLOCK * k / nt, GBLOCK * (k + 1) / nt));
	rows (0, GBLOCK / nt);
	for (auto &f : fs) f.get ();
	return blk;
}

double GroundFilter::Ground (double lat, double lng, double *land)
{
	double u = lat / prm.step, v = lng / prm.step;
	int64_t i = (int64_t)floor (u), j = (int64_t)floor (v);
	double fu = u - i, fv = v - j;
	float g[4], l[4];
	for (int k = 0; k < 4; k++) {
		int64_t ni = i + (k >> 1), nj = j + (k & 1);
		int64_t bi = ni >= 0 ? ni / GBLOCK : (ni - GBLOCK + 1) / GBLOCK, bj = nj >= 0 ? nj / GBLOCK : (nj - GBLOCK + 1) / GBLOCK;
		int64_t key = NodeKey (bi, bj);
		// (the thread's last blocks first: the shared lock and the block's
		// count, taken by every thread making meshes for every point, were
		// most of their time; blocks don't change once made)
		struct Cached { uint64_t filter = 0; int64_t key = 0; std::shared_ptr<const Block> blk; };
		thread_local Cached cache[4];
		thread_local unsigned next = 0;
		const Block *b = nullptr;
		for (const Cached &c : cache)
			if (c.filter == serial && c.key == key) { b = c.blk.get(); break; }
		if (!b) {
			std::shared_ptr<const Block> blk;
			{
				std::shared_lock<std::shared_mutex> lock (mtx);
				auto it = blocks.find (key);
				if (it != blocks.end()) blk = it->second;
			}
			if (!blk) { // (made outside the lock, by one thread: others needing it wait)
				std::promise<std::shared_ptr<const Block>> made;
				std::shared_future<std::shared_ptr<const Block>> pend;
				bool mine = false;
				{
					std::unique_lock<std::shared_mutex> lock (mtx);
					auto it = blocks.find (key);
					if (it != blocks.end()) blk = it->second;
					else {
						auto p = pending.find (key);
						if (p != pending.end()) pend = p->second;
						else {
							pend = pending[key] = made.get_future ().share ();
							mine = true;
						}
					}
				}
				if (mine) {
					making++;
					blk = MakeBlock (bi, bj);
					making--;
					{
						std::unique_lock<std::shared_mutex> lock (mtx);
						if (blocks.size() > 50000) blocks.clear();
						blocks[key] = blk;
						pending.erase (key);
					}
					made.set_value (blk);
				} else if (!blk) blk = pend.get ();
			}
			Cached &c = cache[next++ & 3];
			c.filter = serial, c.key = key, c.blk = std::move (blk);
			b = c.blk.get();
		}
		const NodeVal &nv = (*b)[(ni - bi * GBLOCK) * GBLOCK + (nj - bj * GBLOCK)];
		g[k] = nv.g, l[k] = nv.land;
	}
	double w[4] = { (1 - fu) * (1 - fv), (1 - fu) * fv, fu * (1 - fv), fu * fv }, s = 0.0, sw = 0.0;
	for (int k = 0; k < 4; k++)
		if (!std::isnan (g[k])) s += w[k] * g[k], sw += w[k];
	if (land) *land = w[0] * l[0] + w[1] * l[1] + w[2] * l[2] + w[3] * l[3];
	return sw > 0.5 ? s / sw : std::numeric_limits<double>::quiet_NaN ();
}

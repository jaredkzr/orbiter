// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Hrtf.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>

namespace audio {

namespace {
	constexpr float RAD = 57.2957795f;
}

bool Hrtf::Load (const std::string &path, unsigned r)
{
	dirs.clear (), rings.clear ();
	FILE *f = fopen (path.c_str (), "rb");
	if (!f) return false;
	char magic[8];
	uint32_t hdr[3];
	bool ok = fread (magic, 1, 8, f) == 8 && !memcmp (magic, "OHRTF1\0\0", 8) && fread (hdr, 4, 3, f) == 3 && hdr[1] == TAPS && hdr[2] > 0 && hdr[2] < 100000;
	const unsigned fileRate = ok ? hdr[0] : 0;
	std::vector<Dir> d (ok ? hdr[2] : 0);
	for (auto &x : d) {
		float h[4];
		if (!(ok = fread (h, 4, 4, f) == 4 && fread (x.l, 4, TAPS, f) == TAPS && fread (x.r, 4, TAPS, f) == TAPS)) break;
		x.el = h[0], x.az = h[1], x.dl = h[2], x.dr = h[3];
	}
	fclose (f);
	if (!ok || !fileRate) return false;
	rate = r ? r : fileRate;
	// (another rate: the filters resampled - linearly: they are short and
	// smooth - and scaled, their gain kept; the delays scaled)
	if (rate != fileRate) {
		const double k = (double)fileRate / rate;
		for (auto &x : d) {
			float l[TAPS], rr[TAPS];
			for (int i = 0; i < TAPS; i++) {
				const double t = i * k;
				const int i0 = (int)t;
				const float w = (float)(t - i0);
				auto at = [&] (const float *h, int j) { return j < TAPS ? h[j] : 0.0f; };
				l[i] = (float)k * ((1 - w) * at (x.l, i0) + w * at (x.l, i0 + 1));
				rr[i] = (float)k * ((1 - w) * at (x.r, i0) + w * at (x.r, i0 + 1));
			}
			memcpy (x.l, l, sizeof (l)), memcpy (x.r, rr, sizeof (rr));
			x.dl = (float)(x.dl / k), x.dr = (float)(x.dr / k);
		}
	}
	dirs = std::move (d);
	std::map<float, std::vector<int>> byEl;
	maxDelay = 0;
	for (int i = 0; i < (int)dirs.size (); i++) {
		byEl[dirs[i].el].push_back (i);
		maxDelay = std::max (maxDelay, (int)std::ceil (std::max (dirs[i].dl, dirs[i].dr)) + 1);
	}
	for (auto &e : byEl) {
		Ring rg;
		rg.el = e.first;
		rg.idx = e.second;
		std::sort (rg.idx.begin (), rg.idx.end (), [this] (int a, int b) { return dirs[a].az < dirs[b].az; });
		rings.push_back (std::move (rg));
	}
	return true;
}

void Hrtf::Angles (float x, float y, float z, float &el, float &az)
{
	const float h = std::sqrt (x * x + z * z);
	el = std::atan2 (y, h) * RAD;
	az = std::atan2 (x, z) * RAD;
	if (az < 0.0f) az += 360.0f;
}

void Hrtf::OnRing (const Ring &r, float az, int &a, int &b, float &w) const
{
	const int n = (int)r.idx.size ();
	if (n == 1) { a = b = r.idx[0]; w = 0.0f; return; }
	// (the first above az, and the one before it, around the circle)
	int j = 0;
	while (j < n && dirs[r.idx[j]].az <= az) j++;
	const int i0 = (j - 1 + n) % n, i1 = j % n;
	float a0 = dirs[r.idx[i0]].az, a1 = dirs[r.idx[i1]].az;
	if (a1 <= a0) a1 += 360.0f;
	float x = az;
	if (x < a0) x += 360.0f;
	a = r.idx[i0], b = r.idx[i1];
	w = a1 > a0 ? std::clamp ((x - a0) / (a1 - a0), 0.0f, 1.0f) : 0.0f;
}

void Hrtf::Get (float x, float y, float z, float *left, float *right, float &delayL, float &delayR) const
{
	if (dirs.empty ()) return;
	float el, az;
	Angles (x, y, z, el, az);
	// the rings about the elevation (below the lowest: the lowest)
	int r1 = 0;
	while (r1 < (int)rings.size () && rings[r1].el < el) r1++;
	int r0 = std::max (0, r1 - 1);
	r1 = std::min (r1, (int)rings.size () - 1);
	float we = 0.0f;
	if (r1 != r0) we = std::clamp ((el - rings[r0].el) / (rings[r1].el - rings[r0].el), 0.0f, 1.0f);
	int idx[4];
	float wt[4], wa;
	OnRing (rings[r0], az, idx[0], idx[1], wa);
	wt[0] = (1 - we) * (1 - wa), wt[1] = (1 - we) * wa;
	OnRing (rings[r1], az, idx[2], idx[3], wa);
	wt[2] = we * (1 - wa), wt[3] = we * wa;
	std::fill (left, left + TAPS, 0.0f), std::fill (right, right + TAPS, 0.0f);
	delayL = delayR = 0.0f;
	for (int k = 0; k < 4; k++) {
		if (wt[k] <= 0.0f) continue;
		const Dir &d = dirs[idx[k]];
		for (int i = 0; i < TAPS; i++) left[i] += wt[k] * d.l[i], right[i] += wt[k] * d.r[i];
		delayL += wt[k] * d.dl, delayR += wt[k] * d.dr;
	}
}

} // namespace audio

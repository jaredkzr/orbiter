// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "GroundTracks.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
	constexpr double STEP = 0.25;       // a new segment after this distance [m]
	constexpr double JUMP = 20.0;       // farther in one frame: no segment (moved, not rolled)
	constexpr double RANGE = 3000.0;    // drawn within this distance of the camera [m]
}

bool GroundTracks::Init (vkc::Context *c)
{
	ctx = c;
	for (auto &b : buf) {
		b = ctx->CreateBuffer (BUFFER_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		memset (b.mapped, 0, BUFFER_SIZE);
	}
	return true;
}

void GroundTracks::Shutdown ()
{
	if (!ctx) return;
	for (auto &b : buf) ctx->DestroyBuffer (b);
	ctx = nullptr;
}

void GroundTracks::Add (const Seg &s)
{
	if (segs.size() >= MAX_SEGS) segs.pop_front ();
	segs.push_back (s);
	candBody = nullptr; // (indices changed)
}

void GroundTracks::Contact (OBJHANDLE vessel, int idx, OBJHANDLE body, const VECTOR3 &gpos, const VECTOR3 &gnormal, double halfWidth, double depth)
{
	VECTOR3 bp;
	MATRIX3 R;
	oapiGetGlobalPos (body, &bp);
	oapiGetRotationMatrix (body, &R);
	VECTOR3 lp = tmul (R, gpos - bp), ln = unit (tmul (R, gnormal));
	Point &p = points[{ vessel, idx }];
	if (!p.contact || p.body != body) {
		// touchdown: the imprint of the gear (wider and a little deeper)
		Add ({ body, lp, lp, ln, 1.6 * halfWidth, 1.3 * depth, 1 });
		p.body = body, p.last = lp, p.contact = true;
		return;
	}
	double d = length (lp - p.last);
	if (d < STEP) return;
	if (d < JUMP) Add ({ body, p.last, lp, ln, halfWidth, depth, 0 });
	p.last = lp;
}

void GroundTracks::Scorch (OBJHANDLE body, const VECTOR3 &gpos, const VECTOR3 &gnormal, double radius, double depth)
{
	VECTOR3 bp;
	MATRIX3 R;
	oapiGetGlobalPos (body, &bp);
	oapiGetRotationMatrix (body, &R);
	VECTOR3 lp = tmul (R, gpos - bp), ln = unit (tmul (R, gnormal));
	Add ({ body, lp, lp, ln, radius, depth, 2 });
}

void GroundTracks::BlastHalo (OBJHANDLE body, const VECTOR3 &gpos, const VECTOR3 &gnormal, double radius)
{
	VECTOR3 bp;
	MATRIX3 R;
	oapiGetGlobalPos (body, &bp);
	oapiGetRotationMatrix (body, &R);
	VECTOR3 lp = tmul (R, gpos - bp), ln = unit (tmul (R, gnormal));
	Add ({ body, lp, lp, ln, radius, 0.0, 3 });
}

void GroundTracks::NoContact (OBJHANDLE vessel, int idx)
{
	auto it = points.find ({ vessel, idx });
	if (it != points.end()) it->second.contact = false;
}

void GroundTracks::BeginFrame (uint32_t f, const VECTOR3 &cp, const MATRIX3 &cr)
{
	frame = f, used = 0;
	camPos = cp, camRot = cr;
	candBody = nullptr;
	tiles.clear ();
}

void GroundTracks::ForTile (OBJHANDLE body, const VECTOR3 &ppos, const MATRIX3 &prot, const VECTOR3 &centreDir, double angRadius, float out[2])
{
	out[0] = (float)used, out[1] = 0.0f;
	if (segs.empty() || !buf[frame].mapped) return;
	if (candBody != body) {
		// this body's segments near the camera (scorch marks first: kept if
		// the entries run out)
		candBody = body;
		cand.clear ();
		tiles.clear ();
		groups.clear ();
		VECTOR3 cl = tmul (prot, camPos - ppos);
		double R = oapiGetSize (body);
		for (uint32_t i = 0; i < segs.size(); i++) {
			const Seg &s = segs[i];
			if (s.body == body && length (s.a - cl) < RANGE) {
				// (as wide as the shading reaches: scorch marks are wide)
				double e = std::min ((2.0 + 1.8 * s.hw) / R, PI);
				cand.push_back ({ i, unit (s.a), unit (s.b), e, cos (e), sin (e) });
			}
		}
		std::stable_partition (cand.begin(), cand.end(), [this](const Cand &c) { return segs[c.i].kind >= 2; }); // (scorch marks, blast halos)
		// caps: the mean direction, and the largest angle to an end plus its
		// width (angles by atan2: acos loses metres near 1)
		auto angle = [](const VECTOR3 &u, const VECTOR3 &v) { return atan2 (length (crossp (u, v)), dotp (u, v)); };
		auto cap = [&](uint32_t b, uint32_t e) {
			VECTOR3 c{};
			for (uint32_t k = b; k < e; k++) c += cand[k].ua + cand[k].ub;
			c = unit (c);
			double r = 0.0;
			for (uint32_t k = b; k < e; k++)
				r = std::max (r, std::max (angle (c, cand[k].ua), angle (c, cand[k].ub)) + cand[k].e);
			r = std::min (r * 1.0001 + 1e-12, PI);
			return Cap { c, r, cos (r), sin (r), b, e };
		};
		for (uint32_t b = 0; b < cand.size(); b += GROUP) groups.push_back (cap (b, std::min<uint32_t> (b + GROUP, (uint32_t)cand.size())));
		if (!cand.empty()) all = cap (0, (uint32_t)cand.size());
	}
	if (cand.empty()) return;
	// a cap of angular radius r reaches the tile (radius A, with the margin)
	// if the angle between their centres is at most A + r: cos (A + r) by
	// the sum formula (no trigonometry per test)
	double A = std::min (angRadius * 1.2, PI), cA = cos (A), sA = sin (A);
	auto reaches = [&](const Cap &c) {
		return A + c.r >= PI || dotp (c.c, centreDir) >= c.ce * cA - c.se * sA;
	};
	if (!reaches (all)) return;
	// (a tile drawn again in the frame: depth prepass, shadows, main pass)
	auto key = std::make_tuple (centreDir.x, centreDir.y, centreDir.z, angRadius);
	auto it = tiles.find (key);
	if (it != tiles.end()) { out[0] = (float)it->second.first, out[1] = (float)it->second.second; return; }
	GpuSeg *g = (GpuSeg*)buf[frame].mapped;
	uint32_t n = 0;
	bool full = false;
	for (const Cap &gr : groups) {
		if (full) break;
		if (!reaches (gr)) continue;
		for (uint32_t k = gr.begin; k < gr.end; k++) {
			const Cand &cd = cand[k];
			const Seg &s = segs[cd.i];
			// (both ends tested: segments are short against tiles; their width
			// too: within A + e, cos (A + e) by the sum formula as above)
			double cosLim = A + cd.e >= PI ? -1.0 : cd.ce * cA - cd.se * sA;
			if (dotp (cd.ua, centreDir) < cosLim && dotp (cd.ub, centreDir) < cosLim) continue;
			if (used + n >= MAX_GPU) { full = true; break; }
			VECTOR3 a = tmul (camRot, ppos + mul (prot, s.a) - camPos);
			VECTOR3 b = tmul (camRot, ppos + mul (prot, s.b) - camPos);
			VECTOR3 u = tmul (camRot, mul (prot, s.n));
			GpuSeg &e = g[used + n++];
			e.a[0] = (float)a.x, e.a[1] = (float)a.y, e.a[2] = (float)a.z, e.a[3] = (float)s.hw;
			e.b[0] = (float)b.x, e.b[1] = (float)b.y, e.b[2] = (float)b.z, e.b[3] = (float)s.depth;
			e.up[0] = (float)u.x, e.up[1] = (float)u.y, e.up[2] = (float)u.z, e.up[3] = (float)s.kind;
		}
	}
	out[1] = (float)n;
	tiles[key] = { used, n };
	used += n;
}

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
		Add ({ body, lp, lp, ln, 1.6 * halfWidth, 1.3 * depth, true });
		p.body = body, p.last = lp, p.contact = true;
		return;
	}
	double d = length (lp - p.last);
	if (d < STEP) return;
	if (d < JUMP) Add ({ body, p.last, lp, ln, halfWidth, depth, false });
	p.last = lp;
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
}

void GroundTracks::ForTile (OBJHANDLE body, const VECTOR3 &ppos, const MATRIX3 &prot, const VECTOR3 &centreDir, double angRadius, float out[2])
{
	out[0] = (float)used, out[1] = 0.0f;
	if (segs.empty() || !buf[frame].mapped) return;
	if (candBody != body) {
		// this body's segments near the camera
		candBody = body;
		cand.clear ();
		VECTOR3 cl = tmul (prot, camPos - ppos);
		for (uint32_t i = 0; i < segs.size(); i++) {
			const Seg &s = segs[i];
			if (s.body == body && length (s.a - cl) < RANGE) cand.push_back (i);
		}
	}
	if (cand.empty()) return;
	GpuSeg *g = (GpuSeg*)buf[frame].mapped;
	uint32_t n = 0;
	double R = oapiGetSize (body);
	double cosLim = cos (std::min (angRadius * 1.2 + 2.0 / R, PI));
	for (uint32_t i : cand) {
		const Seg &s = segs[i];
		// (both ends tested: segments are short against tiles)
		if (dotp (unit (s.a), centreDir) < cosLim && dotp (unit (s.b), centreDir) < cosLim) continue;
		if (used + n >= MAX_GPU) break;
		VECTOR3 a = tmul (camRot, ppos + mul (prot, s.a) - camPos);
		VECTOR3 b = tmul (camRot, ppos + mul (prot, s.b) - camPos);
		VECTOR3 u = tmul (camRot, mul (prot, s.n));
		GpuSeg &e = g[used + n++];
		e.a[0] = (float)a.x, e.a[1] = (float)a.y, e.a[2] = (float)a.z, e.a[3] = (float)s.hw;
		e.b[0] = (float)b.x, e.b[1] = (float)b.y, e.b[2] = (float)b.z, e.b[3] = (float)s.depth;
		e.up[0] = (float)u.x, e.up[1] = (float)u.y, e.up[2] = (float)u.z, e.up[3] = s.imprint ? 1.0f : 0.0f;
	}
	out[1] = (float)n;
	used += n;
}

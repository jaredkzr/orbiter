// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ---------------------------------------------------------------------
// A base's runway markings, drawn on its runway groups (the core's RUNWAY
// with MARKINGS: Src/Orbiter/Baseobj.h; their pavement a texture tiled in
// slabs): the markings of FAA AC 150/5340-1 at their real sizes from the
// runway's description (frame.runways[]: Scene.h RunwayGpu), in the base's
// frame - sharp at any distance, filtered over the pixel's footprint (box
// filters of the stripes; the designators' glyphs distance fields made of
// strokes, after the FAA's runway numerals).
// Needs GNoise (mesh.frag) and frame.glsl.

const float RW_FT = 0.3048;
const int RWMARK_VISUAL = 1, RWMARK_NONPRECISION = 2, RWMARK_PRECISION = 3; // (OrbiterAPI.h)

// (linear colours)
const vec3 RW_PAINT = vec3 (0.86, 0.86, 0.83);
const vec3 RW_YELLOW = vec3 (0.73, 0.39, 0.03);
const vec3 RW_RUBBER = vec3 (0.022, 0.022, 0.022);
// roughness
const float RW_ROUGH_PAINT = 0.66, RW_ROUGH_RUBBER = 0.78;

// coverage of the pixel (centred at v, mid its footprint h) by [a, b]
float RwCover (float v, float h, float a, float b)
{
	return clamp ((min (v + h, b) - max (v - h, a)) / (2.0 * h), 0.0, 1.0);
}

// stripes w wide every P from a (n of them; n < 0: without end): the
// pixel's coverage (box filter: the integral of the square wave)
float RwStripeF (float u, float a, float P, float w, float n)
{
	float v = u - a;
	if (n >= 0.0) v = clamp (v, 0.0, n * P);
	return floor (v / P) * w + min (v - P * floor (v / P), w);
}
float RwStripes (float u, float h, float a, float P, float w, float n)
{
	return (RwStripeF (u + h, a, P, w, n) - RwStripeF (u - h, a, P, w, n)) / (2.0 * h);
}

// ---------------------------------------------------------------------
// glyphs: distance fields [ft] in a box 20 ft wide (u, to the right), 60 ft
// tall (v, away from the threshold); strokes 5 ft wide

float RwSeg (vec2 p, vec2 a, vec2 b, float ext) // a straight stroke a-b, its ends extended by ext
{
	vec2 d = b - a;
	float l = length (d);
	vec2 u = d / l, q = p - 0.5 * (a + b);
	vec2 e = abs (vec2 (dot (q, u), dot (q, vec2 (-u.y, u.x)))) - vec2 (0.5 * l + ext, 2.5);
	return length (max (e, 0.0)) + min (max (e.x, e.y), 0.0);
}

float RwArc (vec2 p, vec2 c, float a0, float a1) // a stroke along a circle (radius 7.5 ft) from a0 to a1 [deg, ccw from +u]
{
	const float R = 7.5;
	vec2 q = p - c;
	float a = mod (degrees (atan (q.y, q.x)) - a0, 360.0);
	if (a <= a1 - a0) return abs (length (q) - R) - 2.5;
	vec2 e0 = c + R * vec2 (cos (radians (a0)), sin (radians (a0)));
	vec2 e1 = c + R * vec2 (cos (radians (a1)), sin (radians (a1)));
	return min (length (p - e0), length (p - e1)) - 2.5;
}

float RwGlyph6 (vec2 p)
{
	float d = RwSeg (p, vec2 (2.5, 10.0), vec2 (2.5, 47.5), 0.0);
	d = min (d, RwArc (p, vec2 (10.0, 47.5), 30.0, 180.0));
	d = min (d, RwArc (p, vec2 (10.0, 10.0), 180.0, 360.0));
	d = min (d, RwSeg (p, vec2 (17.5, 10.0), vec2 (17.5, 20.0), 0.0));
	return min (d, RwArc (p, vec2 (10.0, 20.0), 0.0, 180.0));
}

// c: 0-9 the digits, 11 L, 12 C, 13 R
float RwGlyph (int c, vec2 p)
{
	float d = 1e3;
	if (c == 0) {
		d = min (RwArc (p, vec2 (10.0, 47.5), 0.0, 180.0), RwArc (p, vec2 (10.0, 12.5), 180.0, 360.0));
		d = min (d, RwSeg (p, vec2 (2.5, 12.5), vec2 (2.5, 47.5), 0.0));
		d = min (d, RwSeg (p, vec2 (17.5, 12.5), vec2 (17.5, 47.5), 0.0));
	} else if (c == 1) {
		d = min (RwSeg (p, vec2 (11.0, 2.5), vec2 (11.0, 57.5), 2.5), RwSeg (p, vec2 (11.0, 57.0), vec2 (4.5, 50.5), 0.0));
	} else if (c == 2) {
		d = RwArc (p, vec2 (10.0, 47.5), -35.0, 180.0);
		d = min (d, RwSeg (p, vec2 (16.14, 43.2), vec2 (2.6, 2.5), 0.0));
		d = min (d, RwSeg (p, vec2 (2.5, 2.5), vec2 (17.5, 2.5), 2.5));
	} else if (c == 3) {
		d = RwArc (p, vec2 (10.0, 50.0), 0.0, 160.0);
		d = min (d, RwSeg (p, vec2 (17.5, 50.0), vec2 (17.5, 37.5), 0.0));
		d = min (d, RwArc (p, vec2 (10.0, 37.5), -90.0, 0.0));
		d = min (d, RwSeg (p, vec2 (5.0, 30.0), vec2 (10.0, 30.0), 0.0));
		d = min (d, RwArc (p, vec2 (10.0, 22.5), 0.0, 90.0));
		d = min (d, RwSeg (p, vec2 (17.5, 22.5), vec2 (17.5, 10.0), 0.0));
		d = min (d, RwArc (p, vec2 (10.0, 10.0), 200.0, 360.0));
	} else if (c == 4) {
		d = RwSeg (p, vec2 (13.5, 2.5), vec2 (13.5, 57.5), 2.5);
		d = min (d, RwSeg (p, vec2 (13.5, 57.5), vec2 (2.5, 18.0), 0.0));
		d = min (d, RwSeg (p, vec2 (2.5, 18.0), vec2 (17.5, 18.0), 2.5));
	} else if (c == 5) {
		d = RwSeg (p, vec2 (2.5, 57.5), vec2 (17.5, 57.5), 2.5);
		d = min (d, RwSeg (p, vec2 (2.5, 57.5), vec2 (2.5, 32.0), 2.5));
		d = min (d, RwSeg (p, vec2 (2.5, 32.0), vec2 (10.0, 32.0), 0.0));
		d = min (d, RwArc (p, vec2 (10.0, 24.5), 0.0, 90.0));
		d = min (d, RwSeg (p, vec2 (17.5, 24.5), vec2 (17.5, 10.0), 0.0));
		d = min (d, RwArc (p, vec2 (10.0, 10.0), 180.0, 360.0));
	} else if (c == 6) {
		d = RwGlyph6 (p);
	} else if (c == 7) {
		d = min (RwSeg (p, vec2 (2.5, 57.5), vec2 (17.5, 57.5), 2.5), RwSeg (p, vec2 (16.0, 56.0), vec2 (6.0, 2.5), 0.0));
	} else if (c == 8) {
		d = min (RwArc (p, vec2 (10.0, 50.0), 0.0, 180.0), RwArc (p, vec2 (10.0, 37.5), 180.0, 360.0));
		d = min (d, min (RwSeg (p, vec2 (2.5, 37.5), vec2 (2.5, 50.0), 0.0), RwSeg (p, vec2 (17.5, 37.5), vec2 (17.5, 50.0), 0.0)));
		d = min (d, min (RwArc (p, vec2 (10.0, 22.5), 0.0, 180.0), RwArc (p, vec2 (10.0, 10.0), 180.0, 360.0)));
		d = min (d, min (RwSeg (p, vec2 (2.5, 10.0), vec2 (2.5, 22.5), 0.0), RwSeg (p, vec2 (17.5, 10.0), vec2 (17.5, 22.5), 0.0)));
	} else if (c == 9) {
		d = RwGlyph6 (vec2 (20.0, 60.0) - p);
	} else if (c == 11) { // L
		d = min (RwSeg (p, vec2 (2.5, 2.5), vec2 (2.5, 57.5), 2.5), RwSeg (p, vec2 (2.5, 2.5), vec2 (17.5, 2.5), 2.5));
	} else if (c == 12) { // C
		d = min (RwArc (p, vec2 (10.0, 47.5), 20.0, 180.0), RwArc (p, vec2 (10.0, 12.5), 180.0, 340.0));
		d = min (d, RwSeg (p, vec2 (2.5, 12.5), vec2 (2.5, 47.5), 0.0));
	} else if (c == 13) { // R
		d = RwSeg (p, vec2 (2.5, 2.5), vec2 (2.5, 57.5), 2.5);
		d = min (d, RwSeg (p, vec2 (2.5, 57.5), vec2 (10.0, 57.5), 0.0));
		d = min (d, RwArc (p, vec2 (10.0, 50.0), 0.0, 90.0));
		d = min (d, RwSeg (p, vec2 (17.5, 50.0), vec2 (17.5, 42.5), 0.0));
		d = min (d, RwArc (p, vec2 (10.0, 42.5), -90.0, 0.0));
		d = min (d, RwSeg (p, vec2 (2.5, 35.0), vec2 (10.0, 35.0), 0.0));
		d = min (d, RwSeg (p, vec2 (10.0, 35.0), vec2 (17.5, 2.5), 0.0));
	}
	return d;
}

// a glyph's coverage: in a box from (x0, d0) [ft] (its lower left as the
// pilot sees it); x, d [ft], hx, hd the pixel's mid footprint [ft]
float RwGlyphCover (int c, float x, float d, float hx, float hd, float x0, float d0)
{
	vec2 p = vec2 (x - x0, d - d0);
	float m = 2.0 * max (hx, hd) + 1.0;
	if (p.x < -m || p.x > 20.0 + m || p.y < -m || p.y > 60.0 + m) return 0.0;
	float s = RwGlyph (c, p);
	// (the distance's gradient, for the footprint in its units)
	const float E = 0.25;
	float gx = (RwGlyph (c, p + vec2 (E, 0.0)) - s) / E, gd = (RwGlyph (c, p + vec2 (0.0, E)) - s) / E;
	float hs = abs (gx) * hx + abs (gd) * hd + 1e-3;
	float cov = clamp (0.5 - s / (2.0 * hs), 0.0, 1.0);
	// (far: strokes below the pixel - its share of the glyph's box)
	float far = smoothstep (1.5, 4.0, max (hx, hd));
	float box = RwCover (x, hx, x0, x0 + 20.0) * RwCover (d, hd, d0, d0 + 60.0);
	return mix (cov, 0.42 * box, far);
}

// a designator's coverage (code: digits d0 + 11 d1, 10 none; letter 121 l):
// 60 ft tall digits 20 ft wide 15 ft apart, from 190 ft past the threshold;
// a letter there, the digits 80 ft further on
float RwDesignator (float code, float x, float d, float hx, float hd)
{
	int k = int (code + 0.5);
	int c0 = k % 11, c1 = (k / 11) % 11, l = k / 121;
	float d0 = 190.0, cov = 0.0;
	if (l > 0) {
		cov = max (cov, RwGlyphCover (10 + l, x, d, hx, hd, -10.0, d0));
		d0 += 80.0;
	}
	int n = (c0 < 10 ? 1 : 0) + (c1 < 10 ? 1 : 0);
	float x0 = -0.5 * (float (n) * 20.0 + float (n - 1) * 15.0);
	if (c0 < 10) cov = max (cov, RwGlyphCover (c0, x, d, hx, hd, x0, d0));
	if (c1 < 10) cov = max (cov, RwGlyphCover (c1, x, d, hx, hd, x0 + 35.0, d0));
	return cov;
}

// ---------------------------------------------------------------------
// The runway at p (base frame, the ground's x-z [m]) among
// frame.runways[first, first + count): its index, s along it from END1 and t
// across it (to the right looking towards END2) [m]. False: on none.
struct RunwayHit { int i; float s, t; };
bool RunwayFind (vec2 p, int first, int count, out RunwayHit h)
{
	h.i = -1, h.s = h.t = 0.0;
	for (int k = first; k < first + count && k < MAX_RUNWAYS; k++) {
		vec4 r0 = frame.runways[4 * k], r1 = frame.runways[4 * k + 1];
		vec2 q = p - r0.xy;
		float ss = dot (q, r0.zw), tt = dot (q, vec2 (r0.w, -r0.z));
		if (ss > -0.5 && ss < r1.x + 0.5 && abs (tt) < r1.y + 0.5) { h.i = k, h.s = ss, h.t = tt; return true; }
	}
	return false;
}

// The pavement's slabs (runways, pads): each a slab of the texture's atlas
// (RW_SLABS a side, Utils/runway/make_pavement.py), turned or mirrored one
// of 8 ways, picked by a hash of its place: where to sample the maps (uv,
// its gradients for the pixel's screen derivatives of p, dpx, dpy) and the
// turn from the texture's frame to the mesh's (its texture coordinates: the
// atlas tiled, q in units of it). q: the place on the pavement [m] along its
// axes ax (u), az (v) (base frame x-z, unit)
const float RW_SLAB = 7.62;
const int RW_SLABS = 4;
void SlabUV (vec2 q, vec2 ax, vec2 az, vec2 dpx, vec2 dpy, float seed, out vec2 uv, out vec2 dx, out vec2 dy, out mat2 rot)
{
	vec2 qs = q / RW_SLAB;
	vec2 cell = floor (qs), f = qs - cell;
	float hv = GHash (cell + vec2 (seed * 7.0, 0.5)) * 0.5 + 0.5, ho = GHash (cell.yx + vec2 (3.3, seed * 5.0)) * 0.5 + 0.5;
	int v = min (int (hv * float (RW_SLABS * RW_SLABS)), RW_SLABS * RW_SLABS - 1);
	int o = min (int (ho * 8.0), 7);
	// M: the slab's frame -> the texture's (a quarter turn k times, mirrored across u first)
	vec2 c = vec2 ((o & 1) != 0 ? 0.0 : 1.0, (o & 1) != 0 ? 1.0 : 0.0); // (cos, sin of k 90 deg)
	if ((o & 2) != 0) c = -c;
	mat2 M = mat2 (c.x, c.y, -c.y, c.x) * mat2 ((o & 4) != 0 ? -1.0 : 1.0, 0.0, 0.0, 1.0);
	vec2 fv = M * (f - 0.5) + 0.5;
	uv = (vec2 (float (v % RW_SLABS), float (v / RW_SLABS)) + fv) / float (RW_SLABS);
	float k = 1.0 / (RW_SLAB * float (RW_SLABS));
	dx = M * vec2 (dot (dpx, ax), dot (dpx, az)) * k;
	dy = M * vec2 (dot (dpy, ax), dot (dpy, az)) * k;
	rot = transpose (M);
}

void RunwaySlabUV (RunwayHit h, vec2 dpx, vec2 dpy, out vec2 uv, out vec2 dx, out vec2 dy, out mat2 rot)
{
	vec4 r0 = frame.runways[4 * h.i], r2 = frame.runways[4 * h.i + 2];
	vec2 u = r0.zw, rt = vec2 (u.y, -u.x);
	SlabUV (vec2 (h.t, h.s), rt, u, dpx, dpy, r2.w, uv, dx, dy, rot);
}

// the transverse grooves of a concrete runway (saw-cut, 1/4 in wide every
// 1.5 in, 1/4 in deep): their height [m] at the hit, faded where the
// pixel's footprint along the runway covers them; 0 on asphalt
float RunwayGrooves (RunwayHit h, vec2 dpx, vec2 dpy)
{
	vec4 r0 = frame.runways[4 * h.i], r1 = frame.runways[4 * h.i + 1], r2 = frame.runways[4 * h.i + 2];
	float fp = abs (dot (dpx, r0.zw)) + abs (dot (dpy, r0.zw));
	float L = r1.x;
	bool concrete = r2.y < 0.5 || h.s < r1.z + r2.z || h.s > L - r1.w - r2.z;
	float w = 1.0 - smoothstep (0.006, 0.02, fp);
	if (!concrete || w <= 0.0) return 0.0;
	const float P = 1.5 * 0.0254, G = 0.25 * 0.0254;
	float hs = max (0.5 * fp, 1e-4);
	return -0.004 * w * RwStripes (h.s, hs, 0.0, P, G, -1.0);
}

// The runway's markings and the pavement's variation at the hit (p, dpx, dpy
// as RunwayFind, RunwaySlabUV) applied to the albedo and roughness
void RunwayShade (vec2 p, vec2 dpx, vec2 dpy, RunwayHit h, inout vec3 alb, inout float rough)
{
	int i = h.i;
	float s = h.s, t = h.t;
	vec4 r0 = frame.runways[4 * i], r1 = frame.runways[4 * i + 1], r2 = frame.runways[4 * i + 2], r3 = frame.runways[4 * i + 3];
	vec2 u = r0.zw, rt = vec2 (u.y, -u.x);
	float L = r1.x, hw = r1.y;
	// the pixel's half footprint along and across [m]
	float hs = max (0.5 * (abs (dot (dpx, u)) + abs (dot (dpy, u))), 1e-4);
	float ht = max (0.5 * (abs (dot (dpx, rt)) + abs (dot (dpy, rt))), 1e-4);

	// the pavement: concrete slabs a shade apart; broad variation, stains
	bool concrete = r2.y < 0.5 || s < r1.z + r2.z || s > L - r1.w - r2.z;
	float fp = max (hs, ht);
	float var = 1.0 + 0.035 * GNoise (p / 37.0 + r2.w) + 0.025 * GNoise (p / 9.0 + 3.7)
		+ 0.02 * GNoise (p / 2.3 + 7.1) * (1.0 - smoothstep (0.3, 1.2, fp));
	if (concrete) {
		vec2 slab = floor (vec2 (t, s) / 7.62);
		float tint = GHash (slab + r2.w * 13.0) * 0.035 * (1.0 - smoothstep (2.0, 8.0, fp));
		var *= 1.0 + tint;
	}
	float stain = clamp (GNoise (p / 5.0 + 11.0) * 0.7 + GNoise (p / 1.7 + 2.0) * 0.3 - 0.45, 0.0, 1.0);
	var *= 1.0 - 0.18 * stain * (1.0 - smoothstep (1.0, 4.0, fp));
	alb *= var;

	// this end's frame: d past its threshold [m], x to the right of a pilot landing there
	bool far = s > 0.5 * L;
	float thr = far ? r1.w : r1.z;
	float d = far ? L - s - thr : s - thr;
	float x = far ? -t : t;
	float Lrw = L - r1.z - r1.w, mid = 0.5 * Lrw; // (between the thresholds)
	int mk = int (r2.x + 0.5);
	float white = 0.0, yellow = 0.0;

	// the overrun (blast pad): older, darker; yellow chevrons, 3 ft wide at
	// 45 degrees every 100 ft, pointing at the threshold
	if (d < 0.0) {
		alb *= 0.88;
		if (thr > 1.0 && mk > 0) {
			float o = d + abs (x);
			yellow = RwStripes (o, hs + ht, -50.0 * RW_FT - 0.7071 * 3.0 * RW_FT, 100.0 * RW_FT, 1.4142 * 3.0 * RW_FT, -1.0);
			yellow *= RwCover (x, ht, -hw + 3.0 * RW_FT, hw - 3.0 * RW_FT) * RwCover (d, hs, -thr, 0.0);
		}
	} else if (mk > 0) {
		float dft = d / RW_FT, xft = x / RW_FT, hdf = hs / RW_FT, hxf = ht / RW_FT;
		float lim = RwCover (d, hs, -1.0, mid); // (this end's mid)
		// the designator
		float code = far ? r3.y : r3.x;
		bool letter = code > 120.5;
		if (dft > 180.0 - 2.0 * hdf && dft < 340.0 + 2.0 * hdf)
			white = max (white, RwDesignator (code, xft, dft, hxf, hdf) * lim);
		// the centreline: 120 ft stripes, 80 ft gaps, from beyond the designator
		float cw = (mk == RWMARK_PRECISION ? 36.0 : mk == RWMARK_NONPRECISION ? 18.0 : 12.0) * 0.0254;
		float c0 = (letter ? 350.0 : 280.0) * RW_FT;
		white = max (white, RwStripes (d, hs, c0, 200.0 * RW_FT, 120.0 * RW_FT, -1.0) * RwCover (d, hs, c0, mid) * RwCover (x, ht, -0.5 * cw, 0.5 * cw));
		if (mk >= RWMARK_NONPRECISION) {
			// threshold stripes, 20-170 ft: their number by the width (8 a side at 200 ft), a double space at the centre
			float wft = 2.0 * hw / RW_FT;
			float n = wft < 67.5 ? 2.0 : wft < 87.5 ? 3.0 : wft < 125.0 ? 4.0 : wft < 175.0 ? 6.0 : 8.0;
			float ws = (hw - 3.0 * RW_FT) / (2.0 * n);
			float ax = abs (x);
			white = max (white, RwStripes (ax, ht, ws, 2.0 * ws, ws, n) * RwCover (d, hs, 20.0 * RW_FT, 170.0 * RW_FT));
			// the aiming point: 150 ft long from 1020 ft, 30 ft wide (20 ft on narrower runways), 72 ft apart
			float aw = (wft >= 150.0 ? 30.0 : 20.0) * RW_FT, g = 36.0 * RW_FT;
			if (mid > 1200.0 * RW_FT)
				white = max (white, RwCover (ax, ht, g, g + aw) * RwCover (d, hs, 1020.0 * RW_FT, 1170.0 * RW_FT));
		}
		if (mk == RWMARK_PRECISION) {
			float ax = abs (x);
			// side stripes, 3 ft
			white = max (white, RwCover (ax, ht, hw - 3.0 * RW_FT, hw + 1.0) * lim);
			// touchdown zone: pairs of bars 75 x 6 ft, 5 ft apart, 72 ft between the pairs
			const float BARS[10] = float[10] (500.0, 3.0, 1500.0, 2.0, 2000.0, 2.0, 2500.0, 1.0, 3000.0, 1.0);
			float g = 36.0 * RW_FT, bw = 6.0 * RW_FT;
			for (int b = 0; b < 5; b++) {
				float b0 = BARS[2 * b] * RW_FT, b1 = b0 + 75.0 * RW_FT;
				if (b1 > mid || d < b0 - hs || d > b1 + hs) continue;
				white = max (white, RwStripes (ax, ht, g, bw + 5.0 * RW_FT, bw, BARS[2 * b + 1]) * RwCover (d, hs, b0, b1));
			}
		}
	}
	// tyre rubber in the touchdown zones, about the gears' tracks
	if (d > 250.0 * RW_FT && d < 3000.0 * RW_FT && d < mid) {
		float along = smoothstep (250.0, 800.0, d / RW_FT) * (1.0 - smoothstep (1800.0, 3000.0, d / RW_FT));
		float tr = 0.0;
		for (int g = 0; g < 4; g++) {
			float gx = (g < 2 ? 1.5 : 4.5) * (g % 2 == 0 ? -1.0 : 1.0);
			tr = max (tr, exp (-0.5 * pow ((x - gx) / 2.2, 2.0)));
		}
		float streak = clamp (0.6 + 0.6 * GNoise (vec2 (d / 30.0, x / 0.45) + r2.w), 0.0, 1.0);
		streak = mix (streak, 0.6, smoothstep (0.1, 0.5, ht));
		float a = 0.4 * along * tr * streak;
		alb = mix (alb, RW_RUBBER, a * (1.0 - white));
		rough = mix (rough, RW_ROUGH_RUBBER, a);
	}
	// worn paint: a little of the pavement through it
	float wear = clamp (0.93 + 0.05 * GNoise (p / 0.8 + 5.0) - 0.25 * max (GNoise (p / 0.23 + 9.0) - 0.45, 0.0)
		* (1.0 - smoothstep (0.05, 0.2, fp)), 0.6, 1.0);
	white *= wear, yellow *= wear;
	alb = mix (alb, RW_PAINT, white);
	alb = mix (alb, RW_YELLOW, yellow);
	rough = mix (rough, RW_ROUGH_PAINT, max (white, yellow));
}

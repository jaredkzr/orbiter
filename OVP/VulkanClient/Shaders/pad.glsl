// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ---------------------------------------------------------------------
// A base's landing pads, drawn on their groups (the core's LPADx but STYLE
// classic: Src/Orbiter/Baseobj.h; their pavement a texture of slabs: concrete
// on Earth, slabs of sintered regolith tinted to the local ground
// elsewhere), from their description (frame.pads[]: Scene.h PadGpu), after
// heliports' and rocket landing zones' markings (FAA AC 150/5390-2): a
// hazard band at the edge (the octagon's raised rim), a white perimeter
// stripe, a yellow touchdown circle, the pad's number large in white within
// it (runway.glsl's numerals), the blast's scorch at the centre. Lit at night
// by green perimeter and yellow touchdown lights (VBase::ReadPads).
// Needs runway.glsl (and so GNoise, frame.glsl).

const vec3 PAD_BLACK = vec3 (0.03, 0.03, 0.03);

// the pad at p (base frame x-z [m]) among frame.pads[first, first + count):
// its index, the place on it q (its frame, x and z [m])
struct PadHit { int i; vec2 q; };
bool PadFind (vec2 p, int first, int count, out PadHit h)
{
	h.i = -1, h.q = vec2 (0.0);
	for (int k = first; k < first + count && k < MAX_PADS; k++) {
		vec4 a = frame.pads[3 * k], b = frame.pads[3 * k + 1];
		vec2 d = p - a.xy;
		vec2 q = vec2 (a.z * d.x + a.w * d.y, -a.w * d.x + a.z * d.y);
		float r = 46.0 * abs (b.x); // (the octagon's rim reaches 45.3 m)
		if (abs (q.x) < r && abs (q.y) < r) { h.i = k, h.q = q; return true; }
	}
	return false;
}

void PadSlabUV (PadHit h, vec2 dpx, vec2 dpy, out vec2 uv, out vec2 dx, out vec2 dy, out mat2 rot)
{
	vec4 a = frame.pads[3 * h.i], c = frame.pads[3 * h.i + 2];
	SlabUV (h.q, vec2 (a.z, a.w), vec2 (-a.w, a.z), dpx, dpy, c.w, uv, dx, dy, rot);
}

// its markings and the pavement's variation at the hit (dpx, dpy: the
// screen derivatives of p) applied to the albedo and roughness
void PadShade (vec2 dpx, vec2 dpy, PadHit hit, inout vec3 alb, inout float rough)
{
	vec4 a = frame.pads[3 * hit.i], b = frame.pads[3 * hit.i + 1], c = frame.pads[3 * hit.i + 2];
	vec2 ax = vec2 (a.z, a.w), az = vec2 (-a.w, a.z);
	vec2 q = hit.q;
	float sc = abs (b.x), h = 40.0 * sc;
	bool octagon = b.y < 0.5;
	// the pixel's half footprint along the pad's axes [m]
	float hx = max (0.5 * (abs (dot (dpx, ax)) + abs (dot (dpy, ax))), 1e-4);
	float hz = max (0.5 * (abs (dot (dpx, az)) + abs (dot (dpy, az))), 1e-4);
	float fp = max (hx, hz);

	// the pavement: sintered slabs tinted to the ground about the base; concrete's a shade apart
	if (b.w > 0.5) alb *= c.rgb;
	else {
		float tint = GHash (floor (q / RW_SLAB) + c.w * 13.0) * 0.035 * (1.0 - smoothstep (2.0, 8.0, fp));
		alb *= 1.0 + tint;
	}
	alb *= 1.0 + 0.03 * GNoise (q / 23.0 + c.w) + 0.02 * GNoise (q / 6.0 + 2.1);

	// distance in from its edge [m] (the octagon's flats h from its centre)
	vec2 aq = abs (q);
	float e = h - (octagon ? max (max (aq.x, aq.y), (aq.x + aq.y) * 0.70711) : max (aq.x, aq.y));
	float r = length (q);
	float white = 0.0, yellow = 0.0, black = 0.0;
	// the hazard band: the octagon's rim, the square's outer metre: yellow and black, 0.8 m, at 45 degrees
	float band = octagon ? RwCover (e, fp, -10.0 * sc, 0.0) : RwCover (e, fp, 0.0, 1.0 * sc);
	if (band > 0.0) {
		float d = (q.x + q.y) * 0.70711;
		float y = RwStripes (d, (hx + hz) * 0.70711, 0.0, 1.6 * sc, 0.8 * sc, -1.0);
		yellow = band * y, black = band * (1.0 - y);
	}
	// the perimeter stripe: white, 1 m, 1.5 m in from the edge
	white = max (white, RwCover (e, fp, 1.5 * sc, 2.5 * sc));
	// the touchdown circle: yellow, 1.8 m wide, its middle at 0.6 of the way out
	yellow = max (yellow, RwCover (r, fp, 0.6 * h - 0.9 * sc, 0.6 * h + 0.9 * sc));
	// the number: 0.62 of the half width tall (60 units: runway.glsl's numerals), upright towards +z
	{
		float unit = 0.62 * h / 60.0;
		int num = int (b.z + 0.5);
		int d0 = num >= 10 ? (num / 10) % 10 : 10, d1 = num % 10;
		int n = d0 < 10 ? 2 : 1;
		float x0 = -0.5 * (float (n) * 20.0 + float (n - 1) * 15.0);
		vec2 g = q / unit;
		float gh = hx / unit, gv = hz / unit;
		if (abs (g.y) < 32.0 + 2.0 * gv && abs (g.x) < 40.0 + 2.0 * gh) {
			if (d0 < 10) white = max (white, RwGlyphCover (d0, g.x, g.y, gh, gv, x0, -30.0));
			white = max (white, RwGlyphCover (d1, g.x, g.y, gh, gv, x0 + (n == 2 ? 35.0 : 0.0), -30.0));
		}
	}
	// worn paint
	float wear = clamp (0.93 + 0.05 * GNoise (q / 0.8 + 5.0) - 0.25 * max (GNoise (q / 0.23 + 9.0) - 0.45, 0.0)
		* (1.0 - smoothstep (0.05, 0.2, fp)), 0.6, 1.0);
	white *= wear, yellow *= wear;
	alb = mix (alb, RW_PAINT, white);
	alb = mix (alb, RW_YELLOW, yellow);
	alb = mix (alb, PAD_BLACK, black);
	rough = mix (rough, RW_ROUGH_PAINT, max (max (white, yellow), black));
	// the blast's scorch about the centre (the paint too, a little)
	float rs = r / h + 0.05 * GNoise (q / (3.0 * sc));
	float blast = (1.0 - smoothstep (0.04, 0.26, rs)) * clamp (0.6 + 0.35 * GNoise (q / (1.3 * sc) + 4.0), 0.0, 1.0);
	alb *= 1.0 - 0.45 * blast * (1.0 - 0.6 * max (white, yellow));
}

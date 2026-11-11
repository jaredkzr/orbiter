// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Crash fireballs (fireball_grid.comp, fireball.frag): the flow carrying
// the noise's coordinates (phase 4c). The including shader defines FbNoise
// bindings itself; this is geometry only.

// a point x (ball space, in units of 0.9 r) traced back by the vortex
// ring's turn (turn [rad]) about the ring's core (a circle of radius a at
// about the centre's height), faster near the core, lopsided (stronger on
// one side, by azimuth: seedx). Seen from aside the outside rolls down,
// clockwise in (rh, z): back by the opposite turn
vec3 FbRolled (vec3 x, float turn, float a, float seedx)
{
	float rh = length (x.xy);
	vec2 hd = rh > 1e-4 ? x.xy / rh : vec2 (1.0, 0.0);
	vec2 v = vec2 (rh - a, x.z + 0.1);
	float az = atan (x.y, x.x) + 6.2832 * seedx;
	// (a turn of more than about a radian and a half drew the noise out
	// into rings round the core: softly held below it)
	float ang = 1.5 * tanh (turn / 1.5) * exp (-dot (v, v) / 1.5) * (1.0 + 0.4 * sin (az));
	float ca = cos (ang), sa = sin (ang);
	vec2 v0 = vec2 (ca * v.x - sa * v.y, sa * v.x + ca * v.y);
	return vec3 (hd * (v0.x + a), v0.y - 0.1);
}

// the seed of a phase of the flow (its number n): an offset of the noise's coordinates
vec3 FbPhaseSeed (float n)
{
	return fract (vec3 (0.317, 0.571, 0.823) * (n + 3.0) * 1.7);
}

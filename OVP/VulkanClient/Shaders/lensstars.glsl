// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The stars seen through a black hole's lens (lens_stars.comp, lenssky.frag):
// each star's direction (Galactic) and light, binned in the cells of a cube
// (LENS_N x LENS_N a face) by direction. A record: the direction
// octahedrally in 2 x 16 bits, the light in the four bands [S10 deg^2] x
// LENS_FSCALE as halves.

#ifndef LENSSTARS_GLSL
#define LENSSTARS_GLSL

const int LENS_N = 128;
const int LENS_NCELL = 6 * LENS_N * LENS_N;
const float LENS_FSCALE = 0.01;

int LensFace (vec3 d, out vec2 uv)
{
	vec3 a = abs (d);
	int f;
	if (a.x >= a.y && a.x >= a.z) { f = d.x > 0.0 ? 0 : 1; uv = d.yz / a.x; }
	else if (a.y >= a.z) { f = d.y > 0.0 ? 2 : 3; uv = d.xz / a.y; }
	else { f = d.z > 0.0 ? 4 : 5; uv = d.xy / a.z; }
	return f;
}

ivec2 LensCellXY (vec2 uv)
{
	return clamp (ivec2 ((uv * 0.5 + 0.5) * float (LENS_N)), ivec2 (0), ivec2 (LENS_N - 1));
}

int LensCellIndex (int f, ivec2 c) { return (f * LENS_N + c.y) * LENS_N + c.x; }

int LensCell (vec3 d)
{
	vec2 uv;
	int f = LensFace (d, uv);
	return LensCellIndex (f, LensCellXY (uv));
}

uint LensOctEncode (vec3 n)
{
	n /= abs (n.x) + abs (n.y) + abs (n.z);
	vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs (n.yx)) * vec2 (n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return packUnorm2x16 (e * 0.5 + 0.5);
}

vec3 LensOctDecode (uint u)
{
	vec2 e = unpackUnorm2x16 (u) * 2.0 - 1.0;
	vec3 n = vec3 (e, 1.0 - abs (e.x) - abs (e.y));
	float t = max (-n.z, 0.0);
	n.xy += vec2 (n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize (n);
}

#endif

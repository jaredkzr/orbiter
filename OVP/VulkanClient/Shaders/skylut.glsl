// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The sky-view table (a look trade, ORBITER_VK_SKY_LUT=1; sky_lut.comp,
// sky.frag): the light of the air and its transmittance along every
// direction from the camera, by azimuth (from the sun's) and by the angle
// from the zenith, its rows denser towards the horizon (the square root of
// the angle from it), the horizon on the boundary between the two middle
// rows (rays above it leave the air, those below meet the ground).
// Requires atmosphere.glsl (atm)

const float SKYLUT_PI = 3.14159265;

void SkyLutFrame (out vec3 up, out vec3 a1, out vec3 a2, out float zh)
{
	float r = length (atm.camPos.xyz);
	up = atm.camPos.xyz / r;
	vec3 s = atm.sunDir.xyz - up * dot (atm.sunDir.xyz, up);
	if (dot (s, s) < 1e-8) s = abs (up.y) < 0.9 ? cross (up, vec3 (0.0, 1.0, 0.0)) : cross (up, vec3 (1.0, 0.0, 0.0));
	a1 = normalize (s);
	a2 = cross (up, a1);
	zh = SKYLUT_PI - asin (clamp (atm.radii.x / r, 0.0, 1.0)); // (the horizon's angle from the zenith)
}

vec3 SkyLutDir (vec2 uv)
{
	vec3 up, a1, a2;
	float zh;
	SkyLutFrame (up, a1, a2, zh);
	float phi = uv.x * 2.0 * SKYLUT_PI, a;
	if (uv.y >= 0.5) {
		float x = (2.0 * uv.y - 1.0) * (2.0 * uv.y - 1.0);
		a = zh * (1.0 - x);
	} else {
		float x = (1.0 - 2.0 * uv.y) * (1.0 - 2.0 * uv.y);
		a = zh + x * (SKYLUT_PI - zh);
	}
	return up * cos (a) + (a1 * cos (phi) + a2 * sin (phi)) * sin (a);
}

// (rows: the table's height; ground: the ray meets the ground - the side of
// the horizon taken, a texel's row from the other side never blended in)
vec2 SkyLutUv (vec3 d, float rows, bool ground)
{
	vec3 up, a1, a2;
	float zh;
	SkyLutFrame (up, a1, a2, zh);
	float a = acos (clamp (dot (d, up), -1.0, 1.0));
	float phi = atan (dot (d, a2), dot (d, a1));
	float v;
	if (!ground) v = max (0.5 + 0.5 * sqrt (clamp ((zh - a) / zh, 0.0, 1.0)), 0.5 + 0.5 / rows);
	else v = min (0.5 - 0.5 * sqrt (clamp ((a - zh) / (SKYLUT_PI - zh), 0.0, 1.0)), 0.5 - 0.5 / rows);
	return vec2 (phi / (2.0 * SKYLUT_PI), v);
}

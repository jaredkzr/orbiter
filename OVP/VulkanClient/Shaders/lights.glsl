// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Diffuse light from local light sources (vessel lights), and from the
// extra stars (a binary's other star, a black hole's disc: directional, type
// 3, first in the list). Requires frame.glsl.

// the share of a local light (not a star) reaching a point p (view space),
// without its direction: the attenuation (at most 1: Orbiter's coefficients
// make it far larger near the light), faded to nothing at the range, a
// spot's cone (the particles: lit from all sides)
// ext2: the square of an extent added to the distance's in the inverse
// square (a sprite: the mean over its puff, not the light at its centre)
float LightReach (LocalLight l, vec3 p, float ext2)
{
	float d = length (l.pos.xyz - p);
	if (d > l.pos.w) return 0.0;
	float a = min (1.0 / max (l.att.x + l.att.y * d + l.att.z * (d * d + ext2), 1e-4), 1.0);
	float x = clamp (1.0 - pow (d / l.pos.w, 4.0), 0.0, 1.0);
	a *= x * x;
	if (l.dir.w > 1.5) a *= smoothstep (l.spot.x, l.att.w, dot (normalize (p - l.pos.xyz), l.dir.xyz));
	return a;
}

float LightReach (LocalLight l, vec3 p) { return LightReach (l, p, 0.0); }

// the cosine factor of a spherical source of angular radius asin(s) on a
// surface whose normal is at cosine c to its centre: the cosine while the
// whole sphere is above the surface's horizon, falling smoothly to none
// once it has set (a quadratic across the horizon, matching the cosine's
// value and slope): faces turned from an extended flame still lit by its
// near side
float SphereLightCos (float c, float s)
{
	s = clamp (s, 0.0, 1.0);
	if (c >= s) return c;
	if (c <= -s) return 0.0;
	return (c + s) * (c + s) / (4.0 * s);
}

// vis: visibility of the first four lights (ray traced shadows)
vec3 LocalLighting (vec3 posView, vec3 N, vec4 vis)
{
	vec3 sum = vec3(0.0);
	int n = int(frame.lightCount.x);
	for (int i = 0; i < n; i++) {
		LocalLight l = frame.lights[i];
		if (l.dir.w > 2.5) { // a star: from its direction, unattenuated
			sum += l.col.rgb * (i < 4 ? vis[i] : 1.0) * max (dot (N, l.dir.xyz), 0.0);
			continue;
		}
		vec3 L = l.pos.xyz - posView;
		float d = length(L);
		if (d > l.pos.w || d < 1e-4) continue;
		L /= d;
		// distance attenuation, at most full intensity (Orbiter's coefficients
		// make it far larger near the light); clamped before the fades below,
		// which would otherwise be lost in the clamp and leave hard edges
		float a = min(1.0 / max(l.att.x + l.att.y * d + l.att.z * d * d, 1e-4), 1.0);
		// fading to nothing at the range (the attenuation alone does not)
		float x = d / l.pos.w;
		x = clamp(1.0 - x * x * x * x, 0.0, 1.0);
		a *= x * x;
		if (l.dir.w > 1.5) // spot: full intensity inside the umbra cone, none outside the penumbra
			a *= smoothstep(l.spot.x, l.att.w, dot(-L, l.dir.xyz));
		if (i < 4) a *= vis[i];
		sum += l.col.rgb * a * SphereLightCos (dot (N, L), l.dir.w < 1.5 ? l.att.w / d : 0.0);
	}
	return sum;
}

vec3 LocalLighting (vec3 posView, vec3 N) { return LocalLighting(posView, N, vec4(1.0)); }

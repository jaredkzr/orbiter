// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// HDR scene + bloom -> display. The curve is the identity up to SHOULDER and
// approaches 1 smoothly above, so that normally lit scenes are unchanged
// and only highlights are compressed.

#version 450

layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 1, binding = 0) uniform sampler2D bloom;

layout(push_constant) uniform Params {
	float exposure;
	float bloomStrength;
	float flareStrength;
	float aspect;     // width / height
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float SHOULDER = 0.6;

vec3 Curve (vec3 c)
{
	vec3 hi = SHOULDER + (1.0 - SHOULDER) * (1.0 - exp (-(c - SHOULDER) / (1.0 - SHOULDER)));
	return mix (c, hi, step (vec3 (SHOULDER), c));
}

// Lens flare: ghost images of very bright sources (the part of the bloom
// image above FLARE_THRESHOLD: the sun, not sunlit clouds) reflected through
// the image centre at several scales, and a halo ring
const float FLARE_THRESHOLD = 4.0;

vec3 FlareSource (vec2 uv)
{
	return max (texture (bloom, uv).rgb - FLARE_THRESHOLD, vec3 (0.0));
}

vec3 Flare (vec2 uv)
{
	const vec4 G[4] = vec4[] (vec4 (-0.6, 0.9, 0.7, 1.0), vec4 (-0.25, 0.6, 1.0, 0.7),
	                          vec4 (0.45, 1.0, 0.8, 0.5), vec4 (1.4, 0.5, 0.8, 1.0)); // scale, tint
	vec2 toC = vec2 (0.5) - uv;
	vec3 f = vec3 (0.0);
	for (int i = 0; i < 4; i++) {
		vec2 g = vec2 (0.5) + toC * G[i].x;
		if (any (lessThan (g, vec2 (0.0))) || any (greaterThan (g, vec2 (1.0)))) continue;
		vec2 d = (g - 0.5) * vec2 (pc.aspect, 1.0);
		float w = pow (max (1.0 - length (d) / 0.9, 0.0), 2.0);
		f += FlareSource (g) * G[i].yzw * w;
	}
	// halo: a ring at fixed distance from the centre
	vec2 dir = toC * vec2 (pc.aspect, 1.0);
	float len = length (dir);
	if (len > 1e-4) {
		vec2 h = clamp (uv + normalize (dir) / vec2 (pc.aspect, 1.0) * 0.35, vec2 (0.0), vec2 (1.0));
		vec2 dh = (h - 0.5) * vec2 (pc.aspect, 1.0);
		float w = pow (max (1.0 - abs (length (dh) - 0.35) / 0.35, 0.0), 4.0);
		f += FlareSource (h) * vec3 (0.8, 0.9, 1.0) * w;
	}
	return f;
}

void main ()
{
	vec3 c = texture (scene, vUV).rgb + texture (bloom, vUV).rgb * pc.bloomStrength;
	if (pc.flareStrength > 0.0) c += Flare (vUV) * pc.flareStrength;
	outColor = vec4 (Curve (max (c * pc.exposure, vec3 (0.0))), 1.0);
}

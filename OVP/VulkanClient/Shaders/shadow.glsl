// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Sun shadow lookup (fragment shaders). Requires the Frame uniform block
// (shadowMat, shadowParams) of the including shader. Three cascades: around
// the camera target, and two around the camera; each fades into the next
// towards its edge.

const int SHADOW_CASCADES = 3;
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMaps[SHADOW_CASCADES];

// shadow map coordinates of a point, or a w < 0 if outside the map
vec4 ShadowCoord (mat4 m, vec4 prm, vec3 posView, vec3 nView)
{
	// offset along the normal against self-shadowing artefacts
	vec4 sc = m * vec4(posView + nView * prm.y, 1.0);
	bool inside = sc.x > 0.0 && sc.x < 1.0 && sc.y > 0.0 && sc.y < 1.0 && sc.z < 1.0;
	return vec4(sc.xy, sc.z - prm.z, inside ? 1.0 : -1.0);
}

// Percentage-closer filtering over a rotated spiral of samples (each a
// bilinear 2x2 comparison); t: texel size in texture coordinates
float ShadowLookup (int i, vec4 sc, float t)
{
	const int N = 12;
	const float GOLDEN = 2.39996323;
	// per-pixel rotation (interleaved gradient noise)
	float rot = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	float radius = 1.8 * t;
	float s = 0.0;
	for (int k = 0; k < N; k++) {
		float r = sqrt((float(k) + 0.5) / float(N)) * radius;
		float a = float(k) * GOLDEN + rot;
		s += texture(shadowMaps[i], vec3(sc.xy + r * vec2(cos(a), sin(a)), sc.z));
	}
	return s / float(N);
}

// Fraction of sunlight reaching a point (view space position and normal)
float SunShadow (vec3 posView, vec3 nView)
{
	float result = 0.0, remaining = 1.0;
	for (int i = 0; i < SHADOW_CASCADES && remaining > 0.0; i++) {
		if (frame.shadowParams[i].x < 0.5) continue;
		vec4 sc = ShadowCoord(frame.shadowMat[i], frame.shadowParams[i], posView, nView);
		if (sc.w < 0.0) continue;
		// weight: 1 inside, falling to 0 over the outer 4% towards the edge
		float edge = min(min(sc.x, 1.0 - sc.x), min(sc.y, 1.0 - sc.y));
		float w = smoothstep(0.0, 0.04, edge);
		result += remaining * w * ShadowLookup(i, sc, frame.shadowParams[i].w);
		remaining *= 1.0 - w;
	}
	return result + remaining; // beyond the maps: lit
}

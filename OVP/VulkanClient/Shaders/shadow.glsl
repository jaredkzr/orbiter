// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Sun shadow lookup. Requires the Frame uniform block (shadowMat,
// shadowParams) of the including shader.

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;   // near cascade
layout(set = 0, binding = 2) uniform sampler2DShadow shadowMap2;  // far cascade

// shadow map coordinates of a point, or a w < 0 if outside the map
vec4 ShadowCoord (mat4 m, vec4 prm, vec3 posView, vec3 nView)
{
	// offset along the normal against self-shadowing artefacts
	vec4 sc = m * vec4(posView + nView * prm.y, 1.0);
	bool inside = sc.x > 0.0 && sc.x < 1.0 && sc.y > 0.0 && sc.y < 1.0 && sc.z < 1.0;
	return vec4(sc.xy, sc.z - prm.z, inside ? 1.0 : -1.0);
}

float ShadowLookup (sampler2DShadow map, vec4 sc, float t)
{
	float s = texture(map, vec3(sc.xy + vec2(-0.5, -0.5) * t, sc.z));
	s += texture(map, vec3(sc.xy + vec2( 0.5, -0.5) * t, sc.z));
	s += texture(map, vec3(sc.xy + vec2(-0.5,  0.5) * t, sc.z));
	s += texture(map, vec3(sc.xy + vec2( 0.5,  0.5) * t, sc.z));
	return s * 0.25;
}

// Fraction of sunlight reaching a point (view space position and normal)
float SunShadow (vec3 posView, vec3 nView)
{
	if (frame.shadowParams.x > 0.5) {
		vec4 sc = ShadowCoord(frame.shadowMat, frame.shadowParams, posView, nView);
		if (sc.w > 0.0) return ShadowLookup(shadowMap, sc, frame.shadowParams.w);
	}
	if (frame.shadowParams2.x > 0.5) {
		vec4 sc = ShadowCoord(frame.shadowMat2, frame.shadowParams2, posView, nView);
		if (sc.w > 0.0) return ShadowLookup(shadowMap2, sc, frame.shadowParams2.w);
	}
	return 1.0;
}

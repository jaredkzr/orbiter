// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Sun shadow lookup. Requires the Frame uniform block (shadowMat,
// shadowParams) of the including shader.

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

// Fraction of sunlight reaching a point (view space position and normal)
float SunShadow (vec3 posView, vec3 nView)
{
	if (frame.shadowParams.x < 0.5) return 1.0;
	// offset along the normal against self-shadowing artefacts
	vec4 sc = frame.shadowMat * vec4(posView + nView * frame.shadowParams.y, 1.0);
	if (sc.x < 0.0 || sc.x > 1.0 || sc.y < 0.0 || sc.y > 1.0 || sc.z > 1.0) return 1.0;
	float ref = sc.z - frame.shadowParams.z;
	float t = frame.shadowParams.w; // texel size
	float s = texture(shadowMap, vec3(sc.xy + vec2(-0.5, -0.5) * t, ref));
	s += texture(shadowMap, vec3(sc.xy + vec2( 0.5, -0.5) * t, ref));
	s += texture(shadowMap, vec3(sc.xy + vec2(-0.5,  0.5) * t, ref));
	s += texture(shadowMap, vec3(sc.xy + vec2( 0.5,  0.5) * t, ref));
	return s * 0.25;
}

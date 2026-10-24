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
	vec2 pad;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float SHOULDER = 0.6;

vec3 Curve (vec3 c)
{
	vec3 hi = SHOULDER + (1.0 - SHOULDER) * (1.0 - exp (-(c - SHOULDER) / (1.0 - SHOULDER)));
	return mix (c, hi, step (vec3 (SHOULDER), c));
}

void main ()
{
	vec3 c = texture (scene, vUV).rgb + texture (bloom, vUV).rgb * pc.bloomStrength;
	outColor = vec4 (Curve (max (c * pc.exposure, vec3 (0.0))), 1.0);
}

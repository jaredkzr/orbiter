// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Bloom: downsample (4 bilinear taps = 4x4 box); the first pass keeps only
// the light above the threshold

#version 450

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Params {
	vec2 texel;      // source texel size
	float threshold; // > 0: extract light above this level
	float knee;      // soft transition width
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main ()
{
	vec2 t = pc.texel;
	vec3 c = 0.25 * (texture (src, vUV + vec2(-t.x, -t.y)).rgb + texture (src, vUV + vec2(t.x, -t.y)).rgb +
	                 texture (src, vUV + vec2(-t.x,  t.y)).rgb + texture (src, vUV + vec2(t.x,  t.y)).rgb);
	if (pc.threshold > 0.0) {
		float br = max (c.r, max (c.g, c.b));
		float soft = clamp (br - pc.threshold + pc.knee, 0.0, 2.0 * pc.knee);
		soft = soft * soft / (4.0 * pc.knee + 1e-4);
		float w = max (soft, br - pc.threshold) / max (br, 1e-4);
		c *= w;
	}
	outColor = vec4 (c, 1.0);
}

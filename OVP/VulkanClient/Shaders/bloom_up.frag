// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Bloom: upsample with a 3x3 tent filter, added to the next larger level

#version 450

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Params {
	vec2 texel;   // source texel size
	float weight; // contribution of this level
	float pad;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main ()
{
	vec2 t = pc.texel;
	vec3 c = texture (src, vUV).rgb * 4.0;
	c += (texture (src, vUV + vec2(-t.x, 0)).rgb + texture (src, vUV + vec2(t.x, 0)).rgb +
	      texture (src, vUV + vec2(0, -t.y)).rgb + texture (src, vUV + vec2(0, t.y)).rgb) * 2.0;
	c += texture (src, vUV + vec2(-t.x, -t.y)).rgb + texture (src, vUV + vec2(t.x, -t.y)).rgb +
	     texture (src, vUV + vec2(-t.x,  t.y)).rgb + texture (src, vUV + vec2(t.x,  t.y)).rgb;
	outColor = vec4 (c / 16.0 * pc.weight, 1.0);
}

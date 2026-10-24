// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Params {
	vec2 scale;
	vec2 pad;
	vec4 ckey;
	uint mode;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

vec3 LinearToSRGB (vec3 c)
{
	return mix (c * 12.92, 1.055 * pow (c, vec3(1.0/2.4)) - 0.055, step (vec3(0.0031308), c));
}

void main ()
{
	vec4 t;
	if ((pc.mode & 1u) != 0u) {
		ivec2 size = textureSize (tex, 0);
		t = texelFetch (tex, clamp (ivec2 (floor (vUV)), ivec2(0), size - 1), 0);
	} else {
		// clamp to the texel centres at the edges, so that wrapping doesn't bleed in
		vec2 half_texel = 0.5 / vec2 (textureSize (tex, 0));
		t = texture (tex, clamp (vUV, half_texel, 1.0 - half_texel));
	}
	if ((pc.mode & 2u) != 0u) {
		vec3 d = abs (LinearToSRGB (t.rgb) - pc.ckey.rgb);
		if (max (d.r, max (d.g, d.b)) < 0.6/255.0) discard;
	}
	outColor = t * vColor;
}

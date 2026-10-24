// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: textured, fading out where they meet opaque surfaces
// (soft particles); premultiplied (diffuse) or additive (emissive)

#version 450

layout(set = 1, binding = 4) uniform sampler2D prepass; // view depth (w; 0: none)
layout(set = 2, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Draw {
	uint first;
	uint flags;   // 1: emissive
	float alpha;
	float pad;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vPosView;
layout(location = 3) in float vSize;

layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

void main ()
{
	vec4 t = texture (tex, vUV);
	float a = t.a * vColor.a;
	float zs = texelFetch (prepass, ivec2 (gl_FragCoord.xy), 0).w;
	if (zs > 0.0) a *= clamp ((zs - vPosView.z) / max (0.6 * vSize, 0.05), 0.0, 1.0);
	if (a <= 0.001) discard;
	vec3 c = t.rgb * vColor.rgb;
	if ((pc.flags & 1u) != 0u) {
		outColor = vec4 (c * a, 0.0); // additive
		float lum = dot (c * a, vec3 (0.2126, 0.7152, 0.0722));
		outReactive = clamp (lum / (1.0 + lum), 0.0, 0.9);
	} else {
		outColor = vec4 (c * a, a);
		outReactive = clamp (a, 0.0, 0.9);
	}
}

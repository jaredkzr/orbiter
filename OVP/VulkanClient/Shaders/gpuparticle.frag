// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: textured, fading out where they meet opaque surfaces
// (soft particles); premultiplied (diffuse) or additive (emissive).
// Lit puffs (the smoke texture: normal, thickness, coverage) are shaded by
// their billows' normals (sunlit sides, shaded crevices, light shining
// through thin edges) and eroded by drifting noise as they age, so that
// their outlines are ragged and churn instead of round

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
layout(set = 1, binding = 3) uniform sampler3D detailNoise;
layout(set = 1, binding = 4) uniform sampler2D prepass; // view depth (w; 0: none)
layout(set = 2, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Draw {
	uint first;
	uint flags;   // 1: emissive, 2: lit puffs, 4: sorted
	float alpha;
	float pad;
	vec4 bodyPos, bodyVel, omega; // (vertex shader)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vPosView;
layout(location = 3) in float vSize;
layout(location = 4) in vec3 vSun;
layout(location = 5) in vec3 vAmb;
layout(location = 6) in vec4 vMisc;

layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

void main ()
{
	vec4 t = texture (tex, vUV);
	float a;
	vec3 c;
	if ((pc.flags & 2u) != 0u) {
		// erosion: the thinner parts go first as the puff ages
		vec2 lq = fract (vUV * 2.0); // (within the quadrant)
		float n = texture (detailNoise, vec3 (lq * 1.6 + vMisc.z * 7.3, vMisc.w * 0.5 + vMisc.z)).r * 0.6
			+ texture (detailNoise, vec3 (lq * 4.0 - vMisc.z * 3.1, vMisc.w * 0.8 + 0.37)).r * 0.4;
		float thick = t.b;
		float e = 0.22 + 0.5 * vMisc.w;
		float cov = smoothstep (e, e + 0.18, thick + 0.7 * (n - 0.5));
		a = t.a * cov * vColor.a;
		// the billow's normal (sprite axes -> view: turned, facing the camera)
		vec2 nn = t.rg * 2.0 - 1.0;
		vec2 nr = vec2 (vMisc.x * nn.x - vMisc.y * nn.y, vMisc.y * nn.x + vMisc.x * nn.y);
		vec3 N = normalize (vec3 (nr, -sqrt (max (1.0 - dot (nn, nn), 0.0))));
		vec3 L = frame.sunDir.xyz;
		float wrap = clamp ((dot (N, L) + 0.35) / 1.35, 0.0, 1.0);
		float through = (1.0 - thick) * 0.6; // (light passing through thin edges)
		float occl = mix (0.55, 1.0, thick); // (crevices between billows are darker)
		c = vColor.rgb * (vAmb * occl + vSun * (0.15 + 0.85 * wrap + through));
	} else {
		a = t.a * vColor.a;
		c = t.rgb * vColor.rgb * (vSun + vAmb + ((pc.flags & 1u) != 0u ? vec3 (1.0) : vec3 (0.0)));
	}
	float zs = texelFetch (prepass, ivec2 (gl_FragCoord.xy), 0).w;
	if (zs > 0.0) a *= clamp ((zs - vPosView.z) / max (0.6 * vSize, 0.05), 0.0, 1.0);
	if (a <= 0.001) discard;
	if ((pc.flags & 1u) != 0u) {
		outColor = vec4 (c * a, 0.0); // additive
		float lum = dot (c * a, vec3 (0.2126, 0.7152, 0.0722));
		outReactive = clamp (lum / (1.0 + lum), 0.0, 0.9);
	} else {
		outColor = vec4 (c * a, a);
		outReactive = clamp (a, 0.0, 0.9);
	}
}

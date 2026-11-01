// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The diffuse light of the Milky Way: the baked cube (Galactic frame, S10
// per spectral band) in the view direction, as scene radiance. While a new
// bake replaces the old one, the two are cross-faded. With it, the
// zodiacal light (its own cube, alike). The enhanced display treats the
// light as a long-exposure photograph's processing does: the contrast
// raised about a pivot, the brightest compressed (a shoulder, so that a
// nebula's core keeps its colour), the colours deepened.

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform samplerCube skyA;
layout(set = 0, binding = 1) uniform samplerCube skyB;
layout(set = 0, binding = 2) uniform samplerCube zodiacal;

#include "relativity.glsl"

layout(push_constant) uniform Params {
	vec4 galFromView[3]; // rows of the rotation view -> Galactic (xyz); w: the camera's velocity (view, units of c)
	vec4 proj;           // 1/fx, 1/fy, fade to skyB, zodiacal light (0, 1)
	vec4 colour[3];      // bands (S10) -> scene RGB (incl. the eye's adaptation)
	vec4 tone;           // pivot luminance, contrast, saturation, highlight shoulder (0: none)
} pc;

void main ()
{
	// inverse of the projection (x' = fx x / z, y' = -fy y / z)
	vec3 v = normalize (vec3 (vNdc.x * pc.proj.x, -vNdc.y * pc.proj.y, 1.0));
	// (relativistic view: the camera's velocity in the rows' w, view space: the direction at rest)
	v = RelTrueDirOf (v, vec3 (pc.galFromView[0].w, pc.galFromView[1].w, pc.galFromView[2].w));
	vec3 d = vec3 (dot (pc.galFromView[0].xyz, v), dot (pc.galFromView[1].xyz, v), dot (pc.galFromView[2].xyz, v));
	vec4 b = texture (skyA, d);
	if (pc.proj.z > 0.0) b = mix (b, texture (skyB, d), pc.proj.z);
	b += pc.proj.w * texture (zodiacal, d);
	vec3 c = max (vec3 (dot (pc.colour[0], b), dot (pc.colour[1], b), dot (pc.colour[2], b)), vec3 (0.0));
	if (pc.tone.w > 0.0 || pc.tone.y != 1.0 || pc.tone.z != 1.0) {
		float Y = max (dot (c, vec3 (0.2126, 0.7152, 0.0722)), 1e-20);
		float y = pow (Y / pc.tone.x, pc.tone.y);
		if (pc.tone.w > 0.0) y /= 1.0 + y / pc.tone.w;
		c = max (mix (vec3 (Y), c, pc.tone.z), vec3 (0.0)) * (pc.tone.x * y / Y);
	}
	outColor = vec4 (c, 1.0);
}

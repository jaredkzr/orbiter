// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Prepass of planet surface tiles (see prepass.vert), with the morph of
// the continuous level of detail (as tile.vert)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Prepass {
	mat4 modelView;
	mat4 prevClipFromView; // view space now -> clip space of the previous frame
	vec4 maskUV;
	vec4 params;
	vec4 morph;            // morph: start distance [m], 1/range [1/m] (0: none)
	vec4 vessel;           // nearest vessel on the ground, view space (w: present)
} pp;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 4) in vec3 inMorph;

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec4 vClip;
layout(location = 3) out vec4 vPrevClip;
layout(location = 4) out vec2 vUV;

void main()
{
	vec3 p0 = (pp.modelView * vec4(inPos, 1.0)).xyz;
	float d = length (p0);
	if (pp.vessel.w > 0.5) d = min (d, length (p0 - pp.vessel.xyz));
	float m = clamp ((d - pp.morph.x) * pp.morph.y, 0.0, 1.0);
	vec4 p = pp.modelView * vec4(mix (inPos, inMorph, m), 1.0);
	vPosView = p.xyz;
	vNormalView = mat3(pp.modelView) * inNormal;
	gl_Position = frame.proj * p;
	vClip = gl_Position - vec4(frame.jitter.xy * gl_Position.w, 0.0, 0.0);
	vPrevClip = pp.prevClipFromView * p;
	vUV = inUV;
}

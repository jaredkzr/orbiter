// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Billboards (particles, exhaust flames) with view-space vertices

#version 450
#extension GL_GOOGLE_include_directive : require
#include "relativity.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform Params {
	mat4 proj;
	vec4 relBeta; // relativistic view: the camera's velocity (view space, units of c; 0: at rest)
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main ()
{
	gl_Position = pc.proj * vec4 (RelApparentPos (inPos, pc.relBeta.xyz), 1.0); // (relativistic view: as seen)
	vUV = inUV;
	vColor = inColor;
}

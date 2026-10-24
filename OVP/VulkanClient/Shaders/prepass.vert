// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Prepass of opaque geometry (meshes and terrain tiles): view position and
// normal, clip positions in this and the previous frame for the screen
// motion (unjittered), and texture coordinates (material)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Prepass {
	mat4 modelView;
	mat4 prevClipFromView; // view space now -> clip space of the previous frame
} pp;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec4 vClip;
layout(location = 3) out vec4 vPrevClip;
layout(location = 4) out vec2 vUV;

void main()
{
	vec4 p = pp.modelView * vec4(inPos, 1.0);
	vPosView = p.xyz;
	vNormalView = mat3(pp.modelView) * inNormal;
	gl_Position = frame.proj * p;
	vClip = gl_Position - vec4(frame.jitter.xy * gl_Position.w, 0.0, 0.0);
	vPrevClip = pp.prevClipFromView * p;
	vUV = inUV;
}

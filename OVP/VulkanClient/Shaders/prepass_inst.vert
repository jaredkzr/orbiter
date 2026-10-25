// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Prepass of instanced meshes (ground scatter): as prepass.vert, with a
// transform per instance (see mesh_inst.vert)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Prepass {
	mat4 modelView;        // global frame relative to the camera -> view
	mat4 prevClipFromView; // view space now -> clip space of the previous frame
} pp;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inst0;
layout(location = 4) in vec4 inst1;
layout(location = 5) in vec4 inst2;

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec4 vClip;
layout(location = 3) out vec4 vPrevClip;
layout(location = 4) out vec2 vUV;

void main()
{
	mat4 M = transpose(mat4(inst0, inst1, inst2, vec4(0.0, 0.0, 0.0, 1.0)));
	vec4 p = pp.modelView * (M * vec4(inPos, 1.0));
	vPosView = p.xyz;
	vNormalView = mat3(pp.modelView) * (mat3(M) * inNormal);
	gl_Position = frame.proj * p;
	vClip = gl_Position - vec4(frame.jitter.xy * gl_Position.w, 0.0, 0.0);
	vPrevClip = pp.prevClipFromView * p;
	vUV = inUV;
}

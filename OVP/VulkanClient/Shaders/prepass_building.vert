// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Prepass of the buildings of the procedural cities (building.glsl), for
// prepass.frag (normals of sloped parts from derivatives there)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Prepass {
	mat4 modelView;        // frame of the transforms -> view
	mat4 prevClipFromView; // view space now -> clip space of the previous frame
} pp;

#include "building.glsl"

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec4 vClip;
layout(location = 3) out vec4 vPrevClip;
layout(location = 4) out vec2 vUV;

void main()
{
	mat4 M = BuildingFrame();
	vec4 p = pp.modelView * (M * vec4(BuildingPos(), 1.0));
	vPosView = p.xyz;
	vNormalView = mat3(pp.modelView) * (mat3(M) * inNormal);
	gl_Position = frame.proj * RelApparent (p); // (relativistic view: as seen)
	vClip = gl_Position - vec4(frame.jitter.xy * gl_Position.w, 0.0, 0.0);
	vPrevClip = pp.prevClipFromView * RelApparent (p);
	vUV = vec2(0.0);
}

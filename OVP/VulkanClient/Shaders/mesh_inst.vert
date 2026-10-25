// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Instanced meshes (ground scatter): as mesh.vert, with a transform per
// instance (object -> global frame relative to the camera; rows of a 3x4
// matrix, uniform scale) and the view rotation in the push constants

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Draw {
	mat4 modelView;  // global frame relative to the camera -> view
	vec4 diffuse;
	vec4 emissive;
	vec4 specular;
	vec4 light;
	vec4 pbr;
	vec4 heat;
	vec4 maps;
} draw;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inst0;
layout(location = 4) in vec4 inst1;
layout(location = 5) in vec4 inst2;

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec2 vUV;

void main()
{
	mat4 M = transpose(mat4(inst0, inst1, inst2, vec4(0.0, 0.0, 0.0, 1.0)));
	vec4 p = draw.modelView * (M * vec4(inPos, 1.0));
	vPosView = p.xyz;
	vNormalView = mat3(draw.modelView) * (mat3(M) * inNormal);
	vUV = inUV;
	gl_Position = frame.proj * p;
}

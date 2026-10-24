// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planet surface tiles

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

#include "frame.glsl"

layout(push_constant) uniform Tile {
	mat4 modelView;
	vec4 surfUV;   // offset (xy), scale (zw)
	vec4 maskUV;
	vec4 params;   // x: mask present, y: night light intensity
} tile;

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec2 vUV;

void main()
{
	vec4 p = tile.modelView * vec4(inPos, 1.0);
	vPosView = p.xyz;
	vNormalView = mat3(tile.modelView) * inNormal;
	vUV = inUV;
	gl_Position = frame.proj * p;
}

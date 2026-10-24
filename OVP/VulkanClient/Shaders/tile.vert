// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planet surface tiles

#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(set = 0, binding = 0) uniform Frame {
	mat4 proj;
	vec4 sunDir;
	vec4 sunColor;
	vec4 ambient;
	mat4 shadowMat;   // view space -> shadow map (texture coordinates, depth)
	vec4 shadowParams; // x: enabled, y: normal offset [m], z: depth bias, w: texel size
} frame;

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

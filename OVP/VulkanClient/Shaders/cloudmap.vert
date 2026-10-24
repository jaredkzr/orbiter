// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Cloud tiles into the coverage map of the volumetric clouds (top-down,
// orthographic around the camera)

#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(push_constant) uniform Tile {
	mat4 toMap;     // tile -> map clip space
	vec4 surfUV;    // offset (xy), scale (zw) into the tile texture
} tile;

layout(location = 0) out vec2 vUV;

void main()
{
	vUV = inUV * tile.surfUV.zw + tile.surfUV.xy;
	gl_Position = tile.toMap * vec4(inPos, 1.0);
}

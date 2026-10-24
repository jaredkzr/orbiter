// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// 2D drawing: positions in target pixel coordinates

#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform Params {
	vec2 scale;      // 2/width, 2/height
	vec2 pad;
	vec4 ckey;       // colour key (sRGB), used if mode & 2
	uint mode;       // 1: fetch texels (uv in texels), 2: colour key
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main ()
{
	gl_Position = vec4 (inPos * pc.scale - 1.0, 0.0, 1.0);
	vUV = inUV;
	vColor = inColor;
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Billboards (particles, exhaust flames) with view-space vertices

#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform Params {
	mat4 proj;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main ()
{
	gl_Position = pc.proj * vec4 (inPos, 1.0);
	vUV = inUV;
	vColor = inColor;
}

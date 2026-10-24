// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Engine plumes: the bounding box of a plume volume (view space)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec3 vPos;

void main ()
{
	vPos = inPos;
	gl_Position = frame.proj * vec4 (inPos, 1.0);
}

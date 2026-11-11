// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Crash fireballs: the box of a fireball's grid (view space), marched by
// fireball.frag

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec3 vPos;

void main ()
{
	vPos = inPos;
	gl_Position = frame.proj * vec4 (RelApparent (inPos), 1.0);
}

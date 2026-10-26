// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planetary rings, and their shadow on the planet (drawn over its sphere:
// the same transformation as mesh.vert, so that the depths match)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Draw {
	mat4 modelView;
	vec4 ring;
	vec4 legacy;
	vec4 pad[5];
} draw;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vObj;     // planet frame [planet radii]
layout(location = 1) out vec3 vPosView;

invariant gl_Position;

void main()
{
	vec4 p = draw.modelView * vec4(inPos, 1.0);
	vObj = inPos;
	vPosView = p.xyz;
	gl_Position = frame.proj * p;
}

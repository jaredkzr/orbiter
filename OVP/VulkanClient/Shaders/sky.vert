// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Full-screen pass for the sky seen through a planet's atmosphere

#version 450
#extension GL_GOOGLE_include_directive : require

#define ATM_SET 0
#include "atmosphere.glsl"

layout(location = 0) out vec3 vDirView;

void main ()
{
	vec2 ndc = vec2 ((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
	gl_Position = vec4 (ndc, 0.0, 1.0);
	// inverse of the projection (x' = fx x / z, y' = -fy y / z)
	vDirView = vec3 (ndc.x * atm.misc.y, -ndc.y * atm.misc.z, 1.0);
}

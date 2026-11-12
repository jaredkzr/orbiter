// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Reentry plasma volume (Plasma.cpp): the box around a heated vessel and
// its wake, in the flow's frame (x, y across, s along the flow, in units
// of its size), its far side marched by plasma_vol.frag

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Draw {
	mat4 toFlow;  // view -> flow frame
	vec4 tile;    // atlas offset (uv), tile scale (uv), the box's half width
	vec4 box;     // s: start, near/far split, end; time [s]
	vec4 look;    // glow, heating level, shock layer thickness, size [m]
	vec4 misc;    // jitter frame, sheet length, wake length, intensity
} pc;

layout(location = 0) out vec3 vPos;

const int IDX[36] = int[36] (0, 1, 3, 0, 3, 2,  4, 6, 7, 4, 7, 5,  0, 4, 5, 0, 5, 1,
	2, 3, 7, 2, 7, 6,  0, 2, 6, 0, 6, 4,  1, 5, 7, 1, 7, 3);

void main ()
{
	int c = IDX[gl_VertexIndex];
	float W = pc.tile.w;
	vec3 q = vec3 ((c & 1) != 0 ? W : -W, (c & 2) != 0 ? W : -W, (c & 4) != 0 ? pc.box.z : pc.box.x);
	vec4 p = inverse (pc.toFlow) * vec4 (q, 1.0);
	vPos = p.xyz;
	gl_Position = frame.proj * RelApparent (p);
}

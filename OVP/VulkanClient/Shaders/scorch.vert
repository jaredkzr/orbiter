// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Scorch marks: the box around a mark on the ground (view space; 36
// vertices from the push constants)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Mark {
	vec4 centre;  // view space, w: radius [m]
	vec4 normal;  // view space (unit), w: darkening 0..1
	vec4 misc;    // x: seed
} pc;

layout(location = 0) out vec3 vPos;

void main ()
{
	const int F[36] = int[] (0,1,3, 0,3,2, 4,6,7, 4,7,5, 0,4,5, 0,5,1, 2,3,7, 2,7,6, 0,2,6, 0,6,4, 1,5,7, 1,7,3);
	int c = F[gl_VertexIndex];
	vec3 n = pc.normal.xyz;
	vec3 ax = normalize (cross (n, abs (n.y) < 0.9 ? vec3 (0.0, 1.0, 0.0) : vec3 (1.0, 0.0, 0.0)));
	vec3 ay = cross (n, ax);
	vec3 o = vec3 ((c & 1) != 0 ? 1.0 : -1.0, (c & 2) != 0 ? 1.0 : -1.0, (c & 4) != 0 ? 1.0 : -1.0);
	float r = pc.centre.w;
	vPos = pc.centre.xyz + (ax * o.x + ay * o.y) * r + n * (o.z * max (0.1 * r, 1.0));
	gl_Position = frame.proj * vec4 (vPos, 1.0);
}

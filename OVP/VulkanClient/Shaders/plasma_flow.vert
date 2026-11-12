// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Reentry plasma (Plasma.cpp): a heated vessel's meshes seen along the
// flow into its tile of the flow-map atlas - x, y across the flow, s along
// it (downstream), in units of the vessel's size

#version 450

layout(push_constant) uniform Flow {
	mat4 toFlow;  // object -> flow frame
	vec4 scale;   // x: 1 / the map's half width
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 0) out float vS;

void main ()
{
	vec3 q = (pc.toFlow * vec4 (inPos, 1.0)).xyz;
	vS = q.z;
	gl_Position = vec4 (q.xy * pc.scale.x, 0.5, 1.0);
}

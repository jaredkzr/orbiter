// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Instanced shadow casters (ground scatter), alpha-tested where the
// texture says so (shadow_alpha.frag); transform per instance as in
// mesh_inst.vert

#version 450

layout(location = 0) in vec3 inPos;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inst0;
layout(location = 4) in vec4 inst1;
layout(location = 5) in vec4 inst2;

layout(push_constant) uniform Caster {
	mat4 mvp;     // global frame relative to the camera -> shadow map clip space
	vec4 alpha;   // x: texture alpha below which there is no shadow
} caster;

layout(location = 0) out vec2 vUV;

void main()
{
	mat4 M = transpose(mat4(inst0, inst1, inst2, vec4(0.0, 0.0, 0.0, 1.0)));
	gl_Position = caster.mvp * (M * vec4(inPos, 1.0));
	vUV = inUV;
}

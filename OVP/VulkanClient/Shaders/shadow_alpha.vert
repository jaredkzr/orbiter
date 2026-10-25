// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Shadow casters with alpha-tested textures (foliage cards): depth only,
// from the sun

#version 450

layout(location = 0) in vec3 inPos;
layout(location = 2) in vec2 inUV;

layout(push_constant) uniform Caster {
	mat4 mvp;     // object -> shadow map clip space
	vec4 alpha;   // x: texture alpha below which there is no shadow
} caster;

layout(location = 0) out vec2 vUV;

void main()
{
	gl_Position = caster.mvp * vec4(inPos, 1.0);
	vUV = inUV;
}

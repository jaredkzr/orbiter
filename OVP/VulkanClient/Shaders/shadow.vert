// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Shadow casters: depth only, from the sun

#version 450

layout(location = 0) in vec3 inPos;

layout(push_constant) uniform Caster {
	mat4 mvp;  // object -> shadow map clip space
} caster;

void main()
{
	gl_Position = caster.mvp * vec4(inPos, 1.0);
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Shadow casters with alpha-tested textures (see shadow_alpha.vert)

#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Caster {
	mat4 mvp;
	vec4 alpha;
} caster;

layout(location = 0) in vec2 vUV;

void main()
{
	if (texture(tex, vUV).a < caster.alpha.x) discard;
}

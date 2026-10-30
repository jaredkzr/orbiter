// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Buildings of the procedural cities casting shadows (building.glsl)

#version 450
#extension GL_GOOGLE_include_directive : require

layout(push_constant) uniform Caster {
	mat4 mvp;  // frame of the transforms -> shadow map clip space
} caster;

#include "building.glsl"

void main()
{
	gl_Position = caster.mvp * (BuildingFrame() * vec4(BuildingPos(), 1.0));
}

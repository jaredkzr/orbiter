// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Buildings of the procedural cities casting shadows (building.glsl)

#version 450
#extension GL_GOOGLE_include_directive : require

layout(push_constant) uniform Caster {
	mat4 mvp;  // frame of the transforms -> shadow map clip space
	vec4 city; // x: the camera's altitude above the ground [m] (the hand-off to the night light texture)
} caster;

#include "building.glsl"

void main()
{
	// (gone by the hand-off to the night light texture: casting nothing, as
	// the colour pass draws nothing, building.vert)
	if (BuildingFadedOut(BuildingAltFade(caster.city.x))) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	gl_Position = caster.mvp * (BuildingFrame() * vec4(BuildingPos(), 1.0));
}

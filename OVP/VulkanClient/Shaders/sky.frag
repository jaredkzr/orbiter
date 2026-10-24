// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require

#define ATM_SET 0
#include "atmosphere.glsl"

layout(location = 0) in vec3 vDirView;
layout(location = 0) out vec4 outColor;

void main ()
{
	vec3 d = normalize (mat3 (atm.viewToPlanet) * normalize (vDirView));
	// the sky ends at the ground (which is drawn over it)
	vec2 tg = RaySphereC (atm.camPos.xyz, d, atm.camC.x);
	float tMax = (tg.x > 0.0 && tg.x <= tg.y) ? tg.x : ATM_NONE;
	vec3 T;
	vec3 L = Inscatter (d, tMax, T);
	outColor = vec4 (AtmTonemap (L), 0.0);
}

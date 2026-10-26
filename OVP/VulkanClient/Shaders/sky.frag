// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require

#define ATM_SET 0
#include "atmosphere.glsl"

layout(location = 0) in vec3 vDirView;
layout(location = 0) out vec4 outColor; // premultiplied: the sky adds to the stars, the ground covers them

void main ()
{
	vec3 d = normalize (mat3 (atm.viewToPlanet) * normalize (vDirView));
	// Rays hitting the planet end at the ground. Terrain is drawn over it
	// where its tiles reach; beyond (e.g. seen from high ground above the
	// mean radius) the ground is shaded here from the planet's surface map.
	vec2 tg = RaySphereC (atm.camPos.xyz, d, atm.camC.x);
	bool ground = tg.x > 0.0 && tg.x <= tg.y;
	float tMax = ground ? tg.x : ATM_NONE;
	vec3 T;
	vec3 L = AtmTonemap (Inscatter (d, tMax, T));
	if (ground)
		outColor = vec4 (AtmGround (normalize (atm.camPos.xyz + d * tg.x)) * T + L, 1.0);
	else {
		// the stars through the air: dimmed by its extinction, and lost
		// against a bright sky (their contrast with the background: none
		// in daylight, the brightest come out in twilight)
		const vec3 LUM = vec3 (0.2126, 0.7152, 0.0722);
		outColor = vec4 (L, 1.0 - dot (T, LUM) * exp (-dot (L, LUM) / 0.02));
	}
}

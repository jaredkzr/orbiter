// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require

#define ATM_SET 0
#include "atmosphere.glsl"
#include "relativity.glsl"
#include "doppler.glsl"

// 1: the air's veil over a body beyond it (the Moon in the daytime sky), drawn
// where one was drawn after the sky: its light dimmed by the air, the air's
// own light in front of it - not lost against the sky as the stars are
layout(constant_id = 0) const int VEIL = 0;

layout(location = 0) in vec3 vDirView;
layout(location = 0) out vec4 outColor; // premultiplied: the sky adds to the stars, the ground covers them

// the sky-view table (a look trade, ORBITER_VK_SKY_LUT=1: skylut.glsl,
// sky_lut.comp): its light and transmittance in place of the march
#include "skylut.glsl"
layout(set = 1, binding = 0) uniform sampler2D skyLutL;
layout(set = 1, binding = 1) uniform sampler2D skyLutT;
layout(push_constant) uniform SkyDraw { int lut; } sd;

void main ()
{
	// (relativistic view: the direction at rest of what is seen here)
	vec3 d = normalize (mat3 (atm.viewToPlanet) * RelTrueDirOf (vDirView, atm.relBeta.xyz));
	// (and its Doppler shift: sky pixels are not shifted after the scene)
	float D = RelDopplerSeen (normalize (vDirView), atm.relBeta.xyz);
	// Rays hitting the planet end at the ground. Terrain is drawn over it
	// where its tiles reach; beyond (e.g. seen from high ground above the
	// mean radius) the ground is shaded here from the planet's surface map.
	vec2 tg = RaySphereC (atm.camPos.xyz, d, atm.camC.x);
	bool ground = tg.x > 0.0 && tg.x <= tg.y;
	float tMax = ground ? tg.x : ATM_NONE;
	vec3 T;
	vec3 L;
	if (VEIL == 0 && sd.lut != 0) {
		vec2 uv = SkyLutUv (d, float (textureSize (skyLutL, 0).y), ground);
		L = AtmTonemap (textureLod (skyLutL, uv, 0.0).rgb);
		T = textureLod (skyLutT, uv, 0.0).rgb;
	} else
		L = AtmTonemap (Inscatter (d, tMax, T));
	if (ground)
		outColor = vec4 (DopplerShiftRGB (AtmGround (normalize (atm.camPos.xyz + d * tg.x)) * T + L, D), 1.0);
	else {
		// the stars through the air: dimmed by its extinction, and lost
		// against a bright sky (their contrast with the background: none
		// in daylight, the brightest come out in twilight)
		const vec3 LUM = vec3 (0.2126, 0.7152, 0.0722);
		outColor = vec4 (DopplerShiftRGB (L, D), 1.0 - dot (T, LUM) * (VEIL != 0 ? 1.0 : exp (-dot (L, LUM) / 0.02)));
	}
}

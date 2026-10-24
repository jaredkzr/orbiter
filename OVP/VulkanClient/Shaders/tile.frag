// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0) uniform Frame {
	mat4 proj;
	vec4 sunDir;
	vec4 sunColor;
	vec4 ambient;
} frame;

layout(set = 1, binding = 0) uniform sampler2D surfTex;
layout(set = 2, binding = 0) uniform sampler2D maskTex;

layout(push_constant) uniform Tile {
	mat4 modelView;
	vec4 surfUV;
	vec4 maskUV;
	vec4 params;
} tile;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

#define ATM_SET 3
#include "atmosphere.glsl"

void main()
{
	vec3 N = normalize(vNormalView);
	vec3 L = frame.sunDir.xyz;
	float ndl = dot(N, L);
	// clamp to the texel centres at the tile edges (the sampler wraps around)
	vec2 hs = 0.5 / vec2(textureSize(surfTex, 0));
	vec3 base = texture(surfTex, clamp(vUV * tile.surfUV.zw + tile.surfUV.xy, hs, 1.0 - hs)).rgb;
	bool hasAtm = atm.misc.x > 0.5;
	vec3 sunT = vec3(1.0);
	vec3 pPlanet = vec3(0.0), dPlanet = vec3(0.0);
	if (hasAtm) {
		pPlanet = mat3(atm.viewToPlanet) * vPosView + atm.camPos.xyz;
		dPlanet = normalize(mat3(atm.viewToPlanet) * vPosView);
		sunT = SunTransmittance(pPlanet);
	}
	vec3 col = base * (frame.ambient.rgb + frame.sunColor.rgb * sunT * max(ndl, 0.0));

	if (tile.params.z > 0.5) { // cloud layer: lit from above, also seen from below
		float a = texture(surfTex, clamp(vUV * tile.surfUV.zw + tile.surfUV.xy, hs, 1.0 - hs)).a;
		if (hasAtm) {
			vec3 T;
			vec3 L = Inscatter(dPlanet, length(vPosView), T);
			col = col * T + AtmTonemap(L);
		}
		outColor = vec4(col, a);
		return;
	}

	if (tile.params.x > 0.5) {
		vec2 hm = 0.5 / vec2(textureSize(maskTex, 0));
		vec4 m = texture(maskTex, clamp(vUV * tile.maskUV.zw + tile.maskUV.xy, hm, 1.0 - hm));
		// night lights fade in beyond the terminator
		float night = clamp(0.2 - ndl * 4.0, 0.0, 1.0);
		col += m.rgb * night * tile.params.y;
		// specular reflection on water (mask alpha 0)
		if (m.a < 0.5 && ndl > 0.0) {
			vec3 V = normalize(-vPosView);
			vec3 H = normalize(L + V);
			col += frame.sunColor.rgb * sunT * 0.35 * pow(max(dot(N, H), 0.0), 60.0);
		}
	}
	if (hasAtm) { // aerial perspective
		vec3 T;
		vec3 L = Inscatter(dPlanet, length(vPosView), T);
		col = col * T + AtmTonemap(L);
	}
	outColor = vec4(col, 1.0);
}

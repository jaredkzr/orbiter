// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require

#define FRAME_AO
#include "frame.glsl"

#include "shadow.glsl"
#include "lights.glsl"

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
#include "brdf.glsl"

// Periodic value noise (period in cells), for patterns anchored to the
// 1 km-snapped origin (cell sizes divide 1 km)
float Hash (ivec3 c)
{
	uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u;
	h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
	return float(h & 0xffffu) / 65535.0;
}

float ValueNoise (vec3 p, float cell, int period)
{
	vec3 q = p / cell;
	ivec3 i = ivec3(floor(q));
	vec3 f = fract(q);
	f = f * f * (3.0 - 2.0 * f);
	float n[8];
	for (int k = 0; k < 8; k++) {
		// % is undefined for negative operands in GLSL: wrap explicitly
		ivec3 c = i + ivec3(k & 1, (k >> 1) & 1, k >> 2);
		c = c - period * ivec3(floor(vec3(c) / float(period)));
		n[k] = Hash(c);
	}
	return mix(mix(mix(n[0], n[1], f.x), mix(n[2], n[3], f.x), f.y),
	           mix(mix(n[4], n[5], f.x), mix(n[6], n[7], f.x), f.y), f.z);
}

// Brightness variation of the ground close to the camera, where the surface
// textures are blurry
float GroundDetail (vec3 posView, float dist)
{
	float fade = 1.0 - smoothstep(300.0, 2000.0, dist);
	if (fade <= 0.0 || atm.radii.x <= 0.0) return 1.0;
	vec3 p = mat3(atm.viewToPlanet) * posView + atm.water.xyz;
	float d = 0.4 * ValueNoise(p, 8.0, 125) + 0.6 * ValueNoise(p, 2.0, 500);
	return mix(1.0, 0.65 + 0.7 * d, fade);
}

// Wave normal of water surfaces (view space). The waves run along integer
// directions of the planet frame with wavelengths that divide 1 km, so the
// pattern is continuous when its origin (snapped to 1 km) moves.
vec3 WaterNormal (vec3 Nv, vec3 posView, float dist)
{
	if (atm.water.w < 0.0) return Nv;
	float fade = 1.0 - smoothstep (300.0, 3000.0, dist);
	if (fade <= 0.0) return Nv;
	mat3 v2p = mat3 (atm.viewToPlanet);
	vec3 p = v2p * posView + atm.water.xyz; // relative to the pattern origin
	vec3 Np = v2p * Nv;
	const vec4 W[6] = vec4[] (vec4 (1, 0, 0, 37), vec4 (0, 0, 1, 41), vec4 (1, 0, 1, 29),
	                          vec4 (0, 1, 1, 53), vec4 (1, 1, 0, 97), vec4 (1, 0, -1, 113));
	vec3 g = vec3 (0.0);
	for (int i = 0; i < 6; i++) {
		float k = 2.0 * PI * W[i].w / 1000.0;
		float w = sqrt (9.81 * k * length (W[i].xyz)); // deep water dispersion
		float ph = k * dot (p, W[i].xyz) + w * atm.water.w;
		g += W[i].xyz * (0.05 / (1.0 + 0.4 * float (i))) * cos (ph);
	}
	g -= Np * dot (g, Np); // slope in the tangent plane
	return transpose (v2p) * normalize (Np - g * fade);
}

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
		if (tile.params.z < 0.5) sunT *= CloudShadow(pPlanet); // terrain under the clouds
	}
	if (tile.params.z < 0.5) sunT *= SunShadow(vPosView, N); // terrain receives shadows
	if (tile.params.z < 0.5) base *= GroundDetail(vPosView, length(vPosView));
	float ao = tile.params.z < 0.5 ? AmbientOcclusion() : 1.0; // clouds are not in the prepass
	float aoDirect = mix(1.0, ao, frame.aoParams.z); // contact darkening in direct light too
	vec3 col = base * (frame.ambient.rgb * ao + (frame.sunColor.rgb * sunT * max(ndl, 0.0) + LocalLighting(vPosView, N)) * aoDirect);

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
		// the night light texture represents lights seen from altitude; close up
		// it would flood the ground, so it fades out near the camera
		night *= smoothstep(1000.0, 10000.0, length(vPosView));
		col += m.rgb * night * tile.params.y;
		// water (mask alpha 0): reflection of the sky and the sun's glitter,
		// which spreads out when seen from afar
		if (m.a < 0.5) {
			float dist = length(vPosView);
			vec3 V = normalize(-vPosView);
			vec3 Nw = WaterNormal(N, vPosView, dist);
			float NoV = max(dot(Nw, V), 1e-3);
			float fres = 0.02 + 0.98 * pow(1.0 - NoV, 5.0);
			if (hasAtm) {
				vec3 R = reflect(-V, Nw);
				vec3 T;
				vec3 sky = AtmTonemap(Inscatter(normalize(mat3(atm.viewToPlanet) * R), ATM_NONE, T));
				col = mix(col, sky, fres);
			}
			float rough = mix(0.05, 0.3, smoothstep(2e3, 3e5, dist));
			col += frame.sunColor.rgb * sunT * SpecularBRDF(Nw, V, L, vec3(0.02), rough) * PI;
		}
	}
	if (hasAtm) { // aerial perspective
		vec3 T;
		vec3 L = Inscatter(dPlanet, length(vPosView), T);
		col = col * T + AtmTonemap(L);
	}
	outColor = vec4(col, 1.0);
}

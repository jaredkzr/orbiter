// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

#include "shadow.glsl"
#include "lights.glsl"

#define ATM_SET 2
#include "atmosphere.glsl"

layout(set = 1, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Draw {
	mat4 modelView;
	vec4 diffuse;
	vec4 emissive;
	vec4 specular;
	vec4 light;     // rgb: sunlight reaching the object (eclipse)
	vec4 pbr;       // metalness, roughness, reflectivity, dielectric F0 scale
} draw;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;

// Microfacet specular reflection (GGX distribution, Smith-Schlick
// visibility, Schlick Fresnel), times N.L
vec3 SpecularBRDF (vec3 N, vec3 V, vec3 L, vec3 F0, float rough)
{
	vec3 H = normalize (V + L);
	float NoL = max (dot (N, L), 0.0), NoV = max (dot (N, V), 1e-4);
	float NoH = max (dot (N, H), 0.0), VoH = max (dot (V, H), 0.0);
	float a = rough * rough, a2 = a * a;
	float d = NoH * NoH * (a2 - 1.0) + 1.0;
	float D = a2 / (PI * d * d);
	float k = (rough + 1.0) * (rough + 1.0) / 8.0;
	float G = NoV / (NoV * (1.0 - k) + k) * NoL / (NoL * (1.0 - k) + k);
	vec3 F = F0 + (1.0 - F0) * pow (1.0 - VoH, 5.0);
	return D * G * F / (4.0 * NoV * NoL + 1e-4) * NoL;
}

// Analytic approximation of the pre-integrated environment BRDF (Karis)
vec3 EnvBRDF (vec3 F0, float rough, float NoV)
{
	const vec4 c0 = vec4 (-1.0, -0.0275, -0.572, 0.022);
	const vec4 c1 = vec4 (1.0, 0.0425, 1.04, -0.04);
	vec4 r = rough * c0 + c1;
	float a004 = min (r.x * r.x, exp2 (-9.28 * NoV)) * r.x + r.y;
	vec2 AB = vec2 (-1.04, 1.04) * a004 + r.zw;
	return F0 * AB.x + AB.y;
}

// Light arriving from direction d (planet frame): the sky of the planet near
// the camera, or its sunlit surface
vec3 Environment (vec3 d)
{
	if (atm.radii.x <= 0.0) return vec3 (0.0);
	vec3 o = atm.camPos.xyz;
	vec2 tg = RaySphereC (o, d, atm.camC.x);
	float tMax = ATM_NONE;
	vec3 col = vec3 (0.0);
	if (tg.x > 0.0 && tg.x <= tg.y) {
		tMax = tg.x;
		vec3 n = normalize (o + d * tg.x);
		vec3 alb = atm.ground.rgb;
		if (atm.ground.w > 0.5) {
			float lat = asin (clamp (n.y, -1.0, 1.0)), lng = atan (n.z, n.x);
			float v = (0.5 * PI - lat) / PI;
			alb = lng < 0.0 ? texture (atmGround0, vec2 ((lng + PI) / PI, v)).rgb
			                : texture (atmGround1, vec2 (lng / PI, v)).rgb;
		}
		vec3 sunT = atm.misc.x > 0.5 ? SunTransmittance (o + d * tg.x) : vec3 (1.0);
		col = alb * frame.sunColor.rgb * sunT * max (dot (n, atm.sunDir.xyz), 0.0);
	}
	if (atm.misc.x > 0.5) {
		vec3 T;
		vec3 L = Inscatter (d, tMax, T);
		col = col * T + AtmTonemap (L);
	}
	return col;
}

void main()
{
	vec4 texel = texture(tex, vUV);
	vec3 base = texel.rgb * draw.diffuse.rgb;
	float alpha = texel.a * draw.diffuse.a;

	if (draw.emissive.a > 0.5) { // unlit
		outColor = vec4(base, alpha);
		return;
	}

	vec3 N = normalize(vNormalView);
	vec3 V = normalize(-vPosView);
	if (dot(N, V) < 0.0) N = -N; // double-sided surfaces
	vec3 L = frame.sunDir.xyz;
	float NoL = max(dot(N, L), 0.0);
	float NoV = max(dot(N, V), 1e-4);

	float metal = draw.pbr.x;
	float rough = clamp(draw.pbr.y, 0.04, 1.0);
	vec3 F0 = mix(vec3(0.04 * draw.pbr.w), base, metal);

	// sunlight: eclipses, shadows, and extinction in the atmosphere of the
	// planet near the camera
	bool hasAtm = atm.misc.x > 0.5;
	vec3 sun = frame.sunColor.rgb * draw.light.rgb * SunShadow(vPosView, N);
	if (hasAtm) {
		vec3 pPlanet = mat3(atm.viewToPlanet) * vPosView + atm.camPos.xyz;
		sun *= SunTransmittance(pPlanet) * CloudShadow(pPlanet);
	}

	// diffuse (Orbiter's convention: albedo times N.L) and specular reflection
	// of the sun, local lights, ambient, and reflections of the environment
	vec3 diffuse = base * (1.0 - metal);
	vec3 col = diffuse * (frame.ambient.rgb + sun * NoL + LocalLighting(vPosView, N));
	col += sun * SpecularBRDF(N, V, L, F0, rough) * PI;
	if (draw.pbr.z > 0.0) {
		vec3 R = reflect(-V, N);
		R = normalize(mix(R, N, rough * rough)); // rough surfaces: towards the normal
		vec3 env = Environment(normalize(mat3(atm.viewToPlanet) * R));
		col += env * EnvBRDF(F0, rough, NoV) * draw.pbr.z;
	}
	col += draw.emissive.rgb * texel.rgb;

	if (hasAtm) { // aerial perspective
		vec3 T;
		vec3 Lin = Inscatter(normalize(mat3(atm.viewToPlanet) * vPosView), length(vPosView), T);
		col = col * T + AtmTonemap(Lin);
	}
	outColor = vec4(col, alpha);
}

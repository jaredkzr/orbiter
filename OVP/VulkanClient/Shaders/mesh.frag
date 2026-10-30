// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require

#define FRAME_AO
#include "frame.glsl"

#include "shadow.glsl"
#include "lights.glsl"

#define ATM_SET 2
#include "atmosphere.glsl"

layout(set = 1, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 4) uniform samplerCube envProbe; // surroundings of the focus vessel (global frame)

layout(push_constant) uniform Draw {
	mat4 modelView;
	vec4 diffuse;
	vec4 emissive;
	vec4 specular;
	vec4 light;     // rgb: sunlight reaching the object (eclipse)
	vec4 pbr;       // metalness, roughness, reflectivity, dielectric F0 scale
	vec4 heat;      // xyz: direction of motion through the air (view), w: heating glow 0..1
	vec4 maps;      // x: flags of the additional texture maps present
} draw;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

#include "brdf.glsl"
#include "material.glsl"

#include "meshshade.glsl"

void main()
{
	vec4 texel = texture(tex, vUV);
	if (draw.maps.z > 0.0 && texel.a < draw.maps.z) discard; // alpha-tested (foliage)
	vec3 base = texel.rgb * draw.diffuse.rgb;
	float alpha = texel.a * draw.diffuse.a;

	if (draw.emissive.a > 0.5) { // unlit
		outColor = vec4(base, alpha);
		return;
	}

	uint maps = uint(draw.maps.x + 0.5);
	Material mat = SurfaceMaterial(vNormalView, vPosView, vUV, maps, draw.pbr);
	vec3 sun;
	bool debug;
	vec3 col = SurfaceLight(base, mat, sun, debug);
	if (debug) {
		outColor = vec4(col, 1.0);
		return;
	}
	vec3 N = mat.N;
	vec3 V = normalize(-vPosView);
	vec3 L = frame.sunDir.xyz;
	col += draw.emissive.rgb * texel.rgb;
	if ((maps & MAP_TRANSL) != 0u) // sunlight scattered through from behind
		col += texture(translMap, vUV).rgb * sun * max(dot(-N, L), 0.0);
	if ((maps & MAP_TRANSM) != 0u) { // sun seen through the surface
		vec4 tm = texture(transmMap, vUV);
		float spot = pow(max(dot(V, -L), 0.0), mix(2.0, 200.0, tm.a));
		col += tm.rgb * sun * spot;
	}
	if ((maps & MAP_EMIS) != 0u) col += texture(emisMap, vUV).rgb;

	// aerodynamic heating: surfaces facing the airflow glow, from dull red
	// to orange-white
	if (draw.heat.w > 0.0) {
		float f = max(dot(normalize(vNormalView), draw.heat.xyz), 0.0);
		float t = draw.heat.w;
		col += vec3(1.0, 0.25 + 0.45 * t, 0.06 + 0.3 * t) * (4.0 * t * t * f);
	}

	outColor = vec4(AerialPerspective(col), alpha);
}

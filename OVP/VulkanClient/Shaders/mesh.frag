// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0) uniform Frame {
	mat4 proj;
	vec4 sunDir;
	vec4 sunColor;
	vec4 ambient;
	mat4 shadowMat;   // view space -> shadow map (texture coordinates, depth)
	vec4 shadowParams; // x: enabled, y: normal offset [m], z: depth bias, w: texel size
} frame;

#include "shadow.glsl"

layout(set = 1, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Draw {
	mat4 modelView;
	vec4 diffuse;
	vec4 emissive;
	vec4 specular;
	vec4 light;     // rgb: sunlight reaching the object (eclipse)
} draw;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

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
	vec3 L = frame.sunDir.xyz;
	float ndl = max(dot(N, L), 0.0);

	vec3 sun = frame.sunColor.rgb * draw.light.rgb * SunShadow(vPosView, N);
	vec3 col = base * (frame.ambient.rgb + sun * ndl) + draw.emissive.rgb * texel.rgb;

	if (draw.specular.a > 0.0 && ndl > 0.0) {
		vec3 V = normalize(-vPosView);
		vec3 H = normalize(L + V);
		col += draw.specular.rgb * sun * pow(max(dot(N, H), 0.0), draw.specular.a);
	}
	outColor = vec4(col, alpha);
}

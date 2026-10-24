// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Draw {
	mat4 modelView;  // object -> view (camera-relative, rotation and uniform scale)
	vec4 diffuse;    // material diffuse colour, alpha
	vec4 emissive;   // rgb: emissive colour, a: flags (1 = unlit)
	vec4 specular;   // rgb: specular colour, a: specular power
	vec4 light;     // rgb: sunlight reaching the object (eclipse)
	vec4 pbr;       // metalness, roughness, reflectivity, dielectric F0 scale
	vec4 heat;      // xyz: direction of motion through the air (view), w: heating glow 0..1
	vec4 maps;      // x: flags of the additional texture maps present
} draw;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec2 vUV;

void main()
{
	vec4 p = draw.modelView * vec4(inPos, 1.0);
	vPosView = p.xyz;
	vNormalView = mat3(draw.modelView) * inNormal;
	vUV = inUV;
	gl_Position = frame.proj * p;
}

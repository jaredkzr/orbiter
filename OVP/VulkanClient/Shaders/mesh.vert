// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450

layout(set = 0, binding = 0) uniform Frame {
	mat4 proj;       // view -> clip (reversed z, infinite far plane)
	vec4 sunDir;     // direction towards the sun, view space
	vec4 sunColor;
	vec4 ambient;
} frame;

layout(push_constant) uniform Draw {
	mat4 modelView;  // object -> view (camera-relative, rotation and uniform scale)
	vec4 diffuse;    // material diffuse colour, alpha
	vec4 emissive;   // rgb: emissive colour, a: flags (1 = unlit)
	vec4 specular;   // rgb: specular colour, a: specular power
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

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Buildings of the procedural cities (building.glsl)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Draw {
	mat4 modelView;  // frame of the transforms -> view
	vec4 diffuse;
	vec4 emissive;
	vec4 specular;
	vec4 light;
	vec4 pbr;
	vec4 heat;
	vec4 maps;
} draw;

#include "building.glsl"
layout(location = 8) in vec2 instPrm;  // storey height [m], part below the ground [m]
layout(location = 9) in uvec2 instBits; // looks (CityKit.h)

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec3 vObj;      // building frame [m]
layout(location = 3) out vec3 vObjN;     // normal in the building frame (0: from derivatives)
layout(location = 4) flat out vec2 vShape;  // part, facade's share of the side
layout(location = 5) flat out vec4 vDim;
layout(location = 6) flat out vec2 vPrm;
layout(location = 7) flat out uvec2 vBits;
layout(location = 8) flat out vec3 vUpView;   // the building's up (view)
layout(location = 9) flat out float vFade;    // 1: drawn, 0: gone (dithered between)

void main()
{
	mat4 M = BuildingFrame();
	vec3 pos = BuildingPos();
	vec4 p = draw.modelView * (M * vec4(pos, 1.0));
	vPosView = p.xyz;
	vNormalView = mat3(draw.modelView) * (mat3(M) * inNormal);
	vObj = pos;
	vObjN = inNormal;
	vShape = inShape.zw;
	vDim = instDim;
	vPrm = instPrm;
	vBits = instBits;
	vUpView = normalize(mat3(draw.modelView) * mat3(M)[1]);
	// out by the end of its tier's range (CityField TIERS: the set holds a
	// building at least this near, whatever the camera's move from the
	// set's anchor and the cells' size), not all at once at a new set
	float h = instDim.z + instDim.w;
	float range = h < 10.0 ? 3000.0 : h < 35.0 ? 9000.0 : 25000.0;
	float d = length((draw.modelView * vec4(M[3].xyz, 1.0)).xyz);
	vFade = 1.0 - smoothstep(range - 1700.0, range - 700.0, d);
	gl_Position = frame.proj * p;
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planet surface tiles

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec2 inDetailA; // terrain detail: noise amplitude [m], crater fade
layout(location = 4) in vec3 inMorph;   // position on the parent tile's mesh (continuous level of detail)

#include "frame.glsl"

layout(push_constant) uniform Tile {
	mat4 modelView;
	vec4 surfUV;   // offset (xy), scale (zw)
	vec4 maskUV;
	vec4 params;   // x: mask present, y: night light intensity, z: cloud layer, w: volumetric cloud range
	vec4 detail;   // terrain detail: data node spacing [m], shortest wavelength of the mesh [m], Hurst exponent, w: on
	vec4 morph;    // morph to the parent's mesh: start distance [m], 1/range [1/m] (0: none)
	vec4 vessel;   // nearest vessel on the ground, view space (w: present): morph from it too
} tile;

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec2 vDetailA;
layout(location = 4) out float vMorph;

void main()
{
	// morph to the parent tile's shape with the distance (continuous level of detail)
	// (from the camera or the nearest vessel on the ground, whichever is closer)
	vec3 p0 = (tile.modelView * vec4(inPos, 1.0)).xyz;
	float d = length(p0);
	if (tile.vessel.w > 0.5) d = min(d, length(p0 - tile.vessel.xyz));
	float m = clamp((d - tile.morph.x) * tile.morph.y, 0.0, 1.0);
	vec4 p = tile.modelView * vec4(mix(inPos, inMorph, m), 1.0);
	vMorph = m;
	vPosView = p.xyz;
	vNormalView = mat3(tile.modelView) * inNormal;
	vUV = inUV;
	vDetailA = inDetailA;
	gl_Position = frame.proj * p;
}

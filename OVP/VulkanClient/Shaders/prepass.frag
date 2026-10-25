// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Prepass of opaque mesh geometry: view-space normal (facing the camera) and
// linear view depth (ambient occlusion, temporal anti-aliasing), the screen
// motion since the last frame, and the reflective properties of the surface
// (screen-space reflections)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "material.glsl"

layout(push_constant) uniform Prepass {
	mat4 modelView;
	mat4 prevClipFromView;
	vec4 pbr;  // metalness, roughness, reflectivity
	vec4 maps; // x: flags of the additional texture maps present, y: 1 = unlit, z: alpha test (0: none)
} pp;

layout(set = 1, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec4 vClip;
layout(location = 3) in vec4 vPrevClip;
layout(location = 4) in vec2 vUV;

layout(location = 0) out vec4 outData;
layout(location = 1) out vec2 outVelocity; // screen motion since the last frame [uv]
layout(location = 2) out vec4 outMaterial; // shading normal (octahedral), roughness (negative: terrain), reflectivity

void main()
{
	if (pp.maps.z > 0.0 && texture(tex, vUV).a < pp.maps.z) discard; // alpha-tested (foliage)
	vec3 n = vNormalView;
	if (dot(n, n) < 1e-12) n = cross(dFdx(vPosView), dFdy(vPosView));
	n = normalize(n);
	if (dot(n, vPosView) > 0.0) n = -n; // double-sided: towards the camera
	outData = vec4(n, vPosView.z);
	outVelocity = (vClip.xy / vClip.w - vPrevClip.xy / max(vPrevClip.w, 1e-6)) * 0.5;

	Material m = SurfaceMaterial(n, vPosView, vUV, uint(pp.maps.x + 0.5), pp.pbr);
	float refl = pp.maps.y > 0.5 ? 0.0 : m.reflectivity;
	outMaterial = vec4(OctEncode(m.N), m.rough, refl);
}

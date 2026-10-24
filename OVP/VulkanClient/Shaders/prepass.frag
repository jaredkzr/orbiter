// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Prepass of opaque geometry: view-space normal (facing the camera) and
// linear view depth (ambient occlusion, temporal anti-aliasing), and the
// screen motion since the last frame

#version 450

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec4 vClip;
layout(location = 3) in vec4 vPrevClip;

layout(location = 0) out vec4 outData;
layout(location = 1) out vec2 outVelocity; // screen motion since the last frame [uv]

void main()
{
	vec3 n = vNormalView;
	if (dot(n, n) < 1e-12) n = cross(dFdx(vPosView), dFdy(vPosView));
	n = normalize(n);
	if (dot(n, vPosView) > 0.0) n = -n; // double-sided: towards the camera
	outData = vec4(n, vPosView.z);
	outVelocity = (vClip.xy / vClip.w - vPrevClip.xy / max(vPrevClip.w, 1e-6)) * 0.5;
}

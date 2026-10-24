// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Depth prepass for ambient occlusion: view-space normal (facing the
// camera) and linear view depth of opaque geometry

#version 450

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;

layout(location = 0) out vec4 outData;

void main()
{
	vec3 n = vNormalView;
	if (dot(n, n) < 1e-12) n = cross(dFdx(vPosView), dFdy(vPosView));
	n = normalize(n);
	if (dot(n, vPosView) > 0.0) n = -n; // double-sided: towards the camera
	outData = vec4(n, vPosView.z);
}

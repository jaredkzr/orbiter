// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450

layout(set = 0, binding = 0) uniform sampler2D cloudTex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outCoverage;

void main()
{
	outCoverage = vec4(texture(cloudTex, vUV).a);
}

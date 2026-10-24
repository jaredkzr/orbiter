// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450

layout(location = 0) in vec3 vColour;
layout(location = 0) out vec4 outColor;

void main()
{
	outColor = vec4(vColour, 1.0);
}

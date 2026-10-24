// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450

layout(push_constant) uniform Draw {
	mat4 viewProj;   // ecliptic direction -> clip (rotation and projection only)
	float pointSize;
} draw;

layout(location = 0) in vec3 inDir;    // unit vector, ecliptic frame
layout(location = 1) in vec3 inColour; // brightness-scaled colour

layout(location = 0) out vec3 vColour;

void main()
{
	vColour = inColour;
	gl_PointSize = draw.pointSize;
	gl_Position = draw.viewProj * vec4(inDir, 0.0); // at infinity
}

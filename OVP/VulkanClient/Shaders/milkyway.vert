// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Full-screen pass for the diffuse light of the Milky Way

#version 450

layout(location = 0) out vec2 vNdc;

void main ()
{
	vNdc = vec2 ((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
	gl_Position = vec4 (vNdc, 0.0, 1.0);
}

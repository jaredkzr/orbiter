// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Volumetric clouds over the scene: light added, scene attenuated by the
// clouds' transmittance (blend: src + dst * src.a)

#version 450

layout(set = 0, binding = 0) uniform sampler2D cloudTex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
	outColor = texture(cloudTex, vUV);
}

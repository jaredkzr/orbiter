// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require
#include "starfield.glsl"

layout(push_constant) uniform Draw {
	mat4 viewProj;
	vec4 observer;
	vec4 sprite;
	vec4 glare;
	vec4 misc;
} pc;

layout(location = 0) in vec3 vRadiance;
layout(location = 1) in vec2 vOfs;
layout(location = 0) out vec4 outColor;

// the point-spread function: a core and two glare halos, each a Gaussian
float Gauss (float r2, float s) { return exp (-r2 / (2.0 * s * s)) / (6.2831853 * s * s); }

void main ()
{
	float r2 = dot (vOfs, vOfs);
	vec2 f = STAR_GLARE_FRAC;
	float psf = (1.0 - f.x - f.y) * Gauss (r2, pc.sprite.z)
		+ f.x * Gauss (r2, pc.glare.x) + f.y * Gauss (r2, pc.glare.y);
	outColor = vec4 (vRadiance * psf, 0.0);
}

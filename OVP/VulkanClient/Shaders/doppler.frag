// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Relativistic view: the HDR scene Doppler shifted per pixel (doppler.glsl),
// by the direction it is seen in; the camera's own vessel (moving with it)
// unchanged. Its pixels: their position rebuilt from the prepass' linear
// depth, within the vessel's sphere.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "relativity.glsl"
#include "doppler.glsl"

layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 1, binding = 0) uniform sampler2D prepass; // view normal (xyz), linear view depth (w; 0: none)

layout(push_constant) uniform Params {
	vec4 beta;   // the camera's velocity (view space, units of c), w: 1/fx
	vec4 self;   // its own vessel: centre (view space), radius (w; 0: none)
	vec4 misc;   // x: 1/fy
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main ()
{
	vec4 c = texture (scene, vUV);
	vec2 ndc = vUV * 2.0 - 1.0;
	vec3 d = normalize (vec3 (ndc.x * pc.beta.w, -ndc.y * pc.misc.x, 1.0));
	float z = texelFetch (prepass, ivec2 (vUV * vec2 (textureSize (prepass, 0))), 0).w; // (32-bit float: no filtering)
	if (pc.self.w > 0.0 && z > 0.0) {
		vec3 p = d * (z / d.z);
		if (distance (p, pc.self.xyz) < pc.self.w) { outColor = c; return; }
	}
	float D = RelDopplerSeen (d, pc.beta.xyz);
	outColor = vec4 (DopplerShiftRGB (c.rgb, D), c.a);
}

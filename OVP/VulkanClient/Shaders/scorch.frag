// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Scorch marks: the opaque surface seen in the pixel (prepass depth) is
// darkened where it lies on the mark (close to its plane, within its
// radius), most in the middle, with burnt streaks radiating out

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
layout(set = 1, binding = 0) uniform sampler2D prepass;    // view depth (w; 0: none)
layout(set = 1, binding = 3) uniform sampler3D detailNoise;

layout(push_constant) uniform Mark {
	vec4 centre;  // view space, w: radius [m]
	vec4 normal;  // view space (unit), w: darkening 0..1
	vec4 misc;    // x: seed
} pc;

layout(location = 0) in vec3 vPos;
layout(location = 0) out vec4 outColor;

void main ()
{
	float z = texelFetch (prepass, ivec2 (gl_FragCoord.xy), 0).w;
	if (z <= 0.0) discard;
	vec3 d = vPos / vPos.z;
	vec3 P = d * z; // (the surface in the pixel)
	vec3 rel = P - pc.centre.xyz;
	float h = dot (rel, pc.normal.xyz), r = pc.centre.w;
	vec3 inPlane = rel - pc.normal.xyz * h;
	float u = length (inPlane) / r;
	if (u >= 1.0 || abs (h) > max (0.1 * r, 1.0)) discard;
	// streaks: noise stretched along the radius
	vec3 ax = normalize (cross (pc.normal.xyz, abs (pc.normal.y) < 0.9 ? vec3 (0.0, 1.0, 0.0) : vec3 (1.0, 0.0, 0.0)));
	vec3 ay = cross (pc.normal.xyz, ax);
	float ang = atan (dot (inPlane, ay), dot (inPlane, ax));
	float n = texture (detailNoise, vec3 (ang * 1.6, u * 0.35, pc.misc.x)).r;
	float dark = pc.normal.w * (1.0 - smoothstep (0.15, 1.0, u + 0.35 * (n - 0.5))) * (0.75 + 0.5 * n);
	dark = clamp (dark, 0.0, 0.92);
	// multiplied with the scene: soot is dark brownish
	outColor = vec4 (mix (vec3 (1.0), vec3 (0.10, 0.085, 0.07), dark), 1.0);
}

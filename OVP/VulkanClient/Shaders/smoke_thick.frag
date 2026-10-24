// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Screen-space smoke: a volumetric particle's thickness (sphere profile
// times its opacity) and its light times the thickness, added up in a
// half-resolution buffer; hidden behind opaque surfaces

#version 450

layout(set = 1, binding = 4) uniform sampler2D prepass; // view depth (w; 0: none)

layout(push_constant) uniform Draw {
	uint first;
	uint flags;
	float alpha;
	float pad;
	vec4 bodyPos, bodyVel, omega;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vPosView;
layout(location = 3) in float vSize;
layout(location = 4) in vec3 vSun;
layout(location = 5) in vec3 vAmb;
layout(location = 6) in vec4 vMisc;

layout(location = 0) out vec4 outAccum;

void main ()
{
	vec2 q = fract (vUV * 2.0) * 2.0 - 1.0; // (within the quadrant)
	float r2 = dot (q, q);
	if (r2 >= 1.0) discard;
	float th = sqrt (1.0 - r2) * vColor.a;
	// (the prepass at full resolution: this buffer is half)
	vec2 full = gl_FragCoord.xy * 2.0;
	float zs = texelFetch (prepass, ivec2 (full), 0).w;
	if (zs > 0.0) th *= clamp ((zs - vPosView.z) / max (0.6 * vSize, 0.05), 0.0, 1.0);
	if (th <= 1e-4) discard;
	vec3 c = vColor.rgb * (vSun + vAmb);
	outAccum = vec4 (c * th, th);
}

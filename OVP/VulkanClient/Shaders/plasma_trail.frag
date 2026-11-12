// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Reentry afterglow (PlasmaTrail.cpp): soft across the trail, meandering a
// little, streaked along it; additive, with the reactive mask

#version 450

layout(location = 0) in vec3 vPos;
layout(location = 1) in vec4 vCol;  // radiance, along it (units of its width laid)
layout(location = 2) in vec2 vSide; // across it (-1 .. 1), half width [m]

layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

float Hash (vec2 p)
{
	p = fract (p * vec2 (0.1031, 0.1030));
	p += dot (p, p.yx + 33.33);
	return fract ((p.x + p.y) * p.x);
}

float Noise (vec2 p)
{
	vec2 i = floor (p), f = fract (p);
	f = f * f * (3.0 - 2.0 * f);
	return mix (mix (Hash (i), Hash (i + vec2 (1.0, 0.0)), f.x), mix (Hash (i + vec2 (0.0, 1.0)), Hash (i + vec2 (1.0, 1.0)), f.x), f.y);
}

void main ()
{
	float u = vSide.x, hw = vSide.y, s = vCol.a;
	// (its core wandering a little across it, over tens of its widths)
	float uu = u + 0.3 * (Noise (vec2 (s * 0.12, 3.7)) - 0.5);
	// (the strip twice the trail's width: a soft glow, half its height at
	// about 0.37 of the strip's half width out - no hard edges)
	float a = exp (-5.0 * uu * uu) * (1.0 - smoothstep (0.7, 1.0, abs (u)));
	// streaks along it
	float n = 0.6 * Noise (vec2 (uu * 3.0, s * 0.08)) + 0.4 * Noise (vec2 (uu * 7.0 + 1.3, s * 0.25 + 5.1));
	a *= mix (0.35, 1.65, n);
	// (the camera in it: faded, not a band across the screen)
	a *= smoothstep (0.4 * hw, 1.5 * hw, length (vPos));
	vec3 c = vCol.rgb * a;
	if (max (c.r, max (c.g, c.b)) <= 1e-5) discard;
	outColor = vec4 (c, 0.0);
	float lum = dot (c, vec3 (0.2126, 0.7152, 0.0722));
	outReactive = clamp (lum / (1.0 + lum), 0.0, 0.9);
}

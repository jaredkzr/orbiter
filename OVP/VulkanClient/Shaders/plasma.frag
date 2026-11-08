// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Reentry plasma sheath (Plasma.cpp): the glowing shock layer ahead of the
// windward hull. Brightest where the flow meets the hull head-on (the nose,
// leading edges), the colour by the local heating (dull red, orange,
// pinkish white), its outer edge violet-blue (ionised nitrogen and oxygen),
// seen through more of the layer towards its rim; streaks drifting along
// the flow. Additive, with the reactive mask

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Draw {
	mat4 modelView;
	vec4 flow;       // direction of motion through the air (view), w: heating level 0..1
	vec4 shape;      // thickness of the layer [m], vessel radius [m], time [s], intensity
	vec4 centre;     // the vessel's centre (view)
} pc;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in float vPush;

layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

float Hash3 (vec3 p)
{
	p = fract (p * vec3 (0.1031, 0.1030, 0.0973));
	p += dot (p, p.yxz + 33.33);
	return fract ((p.x + p.y) * p.z);
}

float Noise (vec3 x)
{
	vec3 i = floor (x), f = fract (x);
	f = f * f * (3.0 - 2.0 * f);
	return mix (mix (mix (Hash3 (i), Hash3 (i + vec3 (1, 0, 0)), f.x), mix (Hash3 (i + vec3 (0, 1, 0)), Hash3 (i + vec3 (1, 1, 0)), f.x), f.y),
		mix (mix (Hash3 (i + vec3 (0, 0, 1)), Hash3 (i + vec3 (1, 0, 1)), f.x), mix (Hash3 (i + vec3 (0, 1, 1)), Hash3 (i + vec3 (1, 1, 1)), f.x), f.y), f.z);
}

vec3 Ramp (float t)
{
	// (as SheathState::Colour)
	t = clamp (t, 0.0, 1.0);
	vec3 c0 = vec3 (0.55, 0.06, 0.02), c1 = vec3 (1.0, 0.30, 0.06), c2 = vec3 (1.0, 0.52, 0.22), c3 = vec3 (1.0, 0.66, 0.62), c4 = vec3 (1.0, 0.86, 0.92);
	if (t < 0.25) return mix (c0, c1, t * 4.0);
	if (t < 0.50) return mix (c1, c2, t * 4.0 - 1.0);
	if (t < 0.75) return mix (c2, c3, t * 4.0 - 2.0);
	return mix (c3, c4, t * 4.0 - 3.0);
}

void main ()
{
	vec3 N = normalize (vNormalView);
	vec3 F = pc.flow.xyz;
	float face = dot (N, F);
	// (the local heating: the flow meeting the surface head-on heats it most)
	float local = clamp (face, 0.0, 1.0) * smoothstep (0.0, 0.3, vPush);
	if (local <= 0.002) discard;
	float level = pc.flow.w;
	float L = max (pc.shape.y, 0.5);
	// (the upstream parts, the nose and the leading edges, take the
	// hottest flow; the layer thickens and cools downstream)
	float ahead = dot (vPosView - pc.centre.xyz, F) / L;
	float lead = smoothstep (-0.7, 0.8, ahead);
	float tl = level * pow (local, 0.7) * (0.45 + 0.55 * lead);
	// (a thin glowing layer seen at a slant: through more of it)
	vec3 V = normalize (-vPosView);
	float nv = abs (dot (N, V));
	// (through more of it at a slant, up to the hull's outline; beyond, at
	// the layer's own outline, less and less: a soft outer edge)
	float path = min (1.0 / max (nv, 0.15), 4.0) * smoothstep (0.0, 0.4, nv);
	// streaks drifting downstream (turbulence in the layer)
	vec3 q = vPosView / L;
	float x = dot (q, F);
	vec3 across = q - F * x;
	float sk = 0.7;
	float s = Noise (across * 7.0 + F * (x * 1.5 - pc.shape.z * 4.0)) * 0.6 + Noise (across * 19.0 + F * (x * 4.0 - pc.shape.z * 9.0)) * 0.4;
	float I = pc.shape.w * tl * tl * tl * path * (1.0 - sk + 2.0 * sk * s);
	// (the colour by the local heating: the downstream parts, the edges
	// of the windward side, are redder)
	vec3 col = Ramp (tl);
	// the outer edge: violet-blue, seen where the view grazes the layer
	float rim = pow (1.0 - nv, 2.0) * smoothstep (0.1, 0.5, level);
	col = mix (col, vec3 (0.42, 0.32, 1.0), 0.55 * rim);
	vec3 c = col * I;
	outColor = vec4 (c, 0.0);
	float lum = dot (c, vec3 (0.2126, 0.7152, 0.0722));
	outReactive = clamp (lum / (1.0 + lum), 0.0, 0.9);
}

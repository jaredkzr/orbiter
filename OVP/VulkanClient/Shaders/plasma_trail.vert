// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Reentry afterglow (PlasmaTrail.cpp): a trail's strip, each point pushed
// out to its side across the trail, facing the camera

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(location = 0) in vec4 inPos;  // the point (view), its half width [m]
layout(location = 1) in vec4 inTan;  // along the trail (view), the side (-1, 1)
layout(location = 2) in vec4 inCol;  // radiance, along it (units of its width laid)

layout(location = 0) out vec3 vPos;
layout(location = 1) out vec4 vCol;  // radiance, along it
layout(location = 2) out vec2 vSide; // across it (-1 .. 1), half width [m]

void main ()
{
	vec3 p = inPos.xyz;
	float hw = inPos.w;
	float d = max (length (p), 1e-3);
	vec3 e = p / d;
	vec3 s = cross (inTan.xyz, e);
	float sl = length (s);
	s = sl > 1e-4 ? s / sl : normalize (cross (e, abs (e.y) < 0.9 ? vec3 (0.0, 1.0, 0.0) : vec3 (1.0, 0.0, 0.0)));
	float k = 1.0;
	// (at least about a pixel across, fainter for it - not below 0.3 of it:
	// from tens of kilometres a faint line, as the particles' were)
	float px = 2.0 * d / (abs (frame.proj[1][1]) * max (frame.jitter.w, 1.0));
	float hmin = 1.5 * px; // (the strip: its glow about a pixel across)
	if (hw < hmin) k = max (hw / hmin, 0.3), hw = hmin;
	// (seen along it, the light of a longer path through the gas on a
	// shorter stretch of the screen: brighter, as a tube of it would be)
	k /= max (sl, 0.4);
	vec3 q = p + s * (inTan.w * hw);
	vPos = q;
	vCol = vec4 (inCol.rgb * k, inCol.a);
	vSide = vec2 (inTan.w, hw);
	gl_Position = frame.proj * RelApparent (vec4 (q, 1.0));
}

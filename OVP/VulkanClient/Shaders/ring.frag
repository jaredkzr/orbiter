// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planetary rings as a layer of particles (single scattering in a slab,
// after Chandrasekhar): sunlight reflected on the lit side, and scattered
// through to the unlit side, brightest where the rings are thin; opacity
// along the view; the planet's shadow.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "ring.glsl"

layout(location = 0) in vec3 vObj;
layout(location = 1) in vec3 vPosView;

layout(location = 0) out vec4 outColor; // premultiplied

void main()
{
	float r = length (vObj.xz);
	if (r < draw.ring.x || r > draw.ring.y) discard;
	vec3 c = RingColour (r);
	float tau = RingTau (c);
	if (tau <= 0.0) discard;

	// sun and view directions in the planet frame (modelView: rotation and
	// scale)
	mat3 toObj = transpose (mat3 (draw.modelView));
	vec3 L = normalize (toObj * frame.sunDir.xyz);
	vec3 V = normalize (toObj * -vPosView);
	float mu0 = max (abs (L.y), 1e-3), mu = max (abs (V.y), 1e-3);

	// The texture gives the rings' brightness facing the sun and the viewer
	// (mu0 = mu = 1), K1; other geometries scale it by the slab's K.
	float K1 = 0.5 * (1.0 - exp (-2.0 * tau));
	float K;
	vec3 phase;
	if (L.y * V.y > 0.0) { // the lit side
		K = mu0 / (mu0 + mu) * (1.0 - exp (-tau * (1.0 / mu0 + 1.0 / mu)));
		// (the particles reflect mostly back towards the sun: a third as
		// bright at phase angles of 90 degrees)
		float a = acos (clamp (dot (L, V), -1.0, 1.0));
		phase = vec3 (0.3 + 0.7 * exp (-1.2 * a));
	} else {               // the unlit side: light passing through
		float d = mu0 - mu;
		K = abs (d) > 1e-4 ? mu0 * (exp (-tau / mu0) - exp (-tau / mu)) / d
		                   : tau / mu0 * exp (-tau / mu0);
		// the ice particles scatter backwards more than forwards; dust
		// (the thin rings) forwards towards the sun
		float fwd = max (-dot (V, L), 0.0);
		phase = vec3 (0.45) + vec3 (1.6, 1.5, 1.4) * pow (fwd, 12.0) * exp (-2.0 * tau);
	}
	vec3 albedo = c / max (K1, 1e-4);

	// the planet's shadow (a sphere; slightly soft: the sun's disc, refraction)
	float shadow = 1.0;
	float b = dot (vObj, L);
	if (b < 0.0) {
		float dc = sqrt (max (dot (vObj, vObj) - b * b, 0.0)); // closest approach of the sun ray to the centre
		shadow = smoothstep (0.995, 1.01, dc);
	}

	vec3 sun = frame.sunColor.rgb * draw.ring.z * shadow;
	vec3 col = albedo * K * phase * sun;
	float alpha = 1.0 - exp (-tau / mu);
	col += c * frame.ambient.rgb * alpha;
	outColor = vec4 (col, alpha);
}

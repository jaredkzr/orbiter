// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The rings' shadow on the planet: drawn over the planet's sphere,
// multiplying its colour by the sunlight passing through the rings

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "ring.glsl"

layout(location = 0) in vec3 vObj;
layout(location = 1) in vec3 vPosView;

layout(location = 0) out vec4 outColor; // factor

void main()
{
	vec3 p = normalize (vObj);
	vec3 L = normalize (transpose (mat3 (draw.modelView)) * frame.sunDir.xyz);
	float T = 1.0;
	if (dot (p, L) > 0.0 && abs (L.y) > 1e-4) {
		float t = -p.y / L.y; // to the ring plane along the sun ray
		if (t > 0.0) {
			float r = length (p.xz + t * L.xz);
			if (r > draw.ring.x && r < draw.ring.y)
				T = exp (-RingTau (RingColour (r)) / abs (L.y));
		}
	}
	outColor = vec4 (vec3 (T), 1.0);
}

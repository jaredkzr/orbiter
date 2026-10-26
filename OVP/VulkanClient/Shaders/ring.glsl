// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planetary rings: the ring texture by radius, and the rings' optical
// depth. Shared by ring.frag (the rings) and ringshadow.frag (their shadow
// on the planet).

layout(set = 1, binding = 0) uniform sampler2D ringTex;

// (the mesh pipelines' push constants, used for the rings)
layout(push_constant) uniform Draw {
	mat4 modelView;  // planet frame, in planet radii -> view
	vec4 ring;       // inner and outer radius [planet radii], sunlight reaching the planet (eclipse), texture layout (0: radial, 1: legacy)
	vec4 legacy;     // legacy layout: radial line from (fo, t0) to (fi, 1) for radii nrad -> irad: fo, fi, nrad
	vec4 pad[5];
} draw;

// The ring's colour at radius r [planet radii] (linear; the texture is sRGB)
vec3 RingColour (float r)
{
	float irad = draw.ring.x, orad = draw.ring.y;
	vec2 uv;
	if (draw.ring.w > 0.5) {
		// (16 straight sections: VPlanet::BuildRing)
		float nrad = draw.legacy.z;
		float tv = (nrad - r) / (nrad - irad);
		uv = vec2 (draw.legacy.x + (draw.legacy.y - draw.legacy.x) * tv, tv);
	} else {
		// one texel row from the inner to the outer radius
		float w = float (textureSize (ringTex, 0).x);
		uv = vec2 (clamp ((r - irad) / (orad - irad), 0.5 / w, 1.0 - 0.5 / w), 0.5);
	}
	return texture (ringTex, uv).rgb;
}

// Normal optical depth of the rings with colour c. The textures hold the
// rings' brightness only; brightness and optical depth go together in
// Saturn's rings (the dense B ring brightest, the C ring and the Cassini
// division dark and thin): tau 2.5 to 4 for the B ring, 0.7 for the A
// ring, 0.1 for the C ring and the Cassini division.
float RingTau (vec3 c)
{
	float l = pow (max (dot (c, vec3 (0.2126, 0.7152, 0.0722)), 0.0), 1.0 / 2.2); // (perceptual)
	return (0.03 + 5.0 * l * l * l * l) * smoothstep (0.0, 0.04, l);
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The catalogue stars selected (star_select.comp) as point-spread sprites:
// each star's light (in the four bands, dimmed and reddened by the dust)
// spread over a few pixels by a Gaussian, at its direction from the
// observer this frame.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "starfield.glsl"
#include "relativity.glsl"

layout(std430, set = 0, binding = 0) readonly buffer Stars { Star stars[]; };
layout(std430, set = 0, binding = 1) readonly buffer List { StarListHeader head; uvec2 entries[]; } list;
layout(std430, set = 0, binding = 3) readonly buffer Gen { StarGenHeader head; Star stars[]; } gen;
layout(std430, set = 0, binding = 2) readonly buffer Colours {
	vec4 rgb[3];               // band -> linear RGB rows
	vec4 extinction;           // A(band) / A(V)
	vec4 band[STAR_NTEMP];     // light per band by temperature, for M_V = 0 [M_V = 0 of the Sun's colour]
} col;

layout(push_constant) uniform Draw {
	mat4 viewProj;  // Galactic direction -> clip
	vec4 observer;  // xyz: heliocentric Galactic position [pc]; w: limiting magnitude
	vec4 sprite;    // xy: 2 / viewport size [px]; z: core sigma [px]; w: radiance of 1 S10 deg^2 on a pixel at the centre
	vec4 glare;     // xy: sigma of the two glare halos [px]; zw: their fractions of the light
	vec4 misc;      // x: radiance of 1 S10; yzw: the camera's velocity (Galactic, units of c)
} pc;

layout(location = 0) out vec3 vRadiance; // of all the light on one pixel
layout(location = 1) out vec2 vOfs;      // [px] from the centre

const float RADIUS = 3.0;     // of the core [sigma]
const float GLARE_MIN = 1.0;  // a halo ends where it is fainter than this [S10] (the darkest sky: ~40)

void main ()
{
	gl_Position = vec4 (2.0, 2.0, 2.0, 1.0); // (culled)
	vRadiance = vec3 (0.0), vOfs = vec2 (0.0);
	uint k = gl_InstanceIndex;
	if (k >= list.head.capacity) return;
	uvec2 e = list.entries[k];
	Star s = (e.x & STAR_GENERATED) != 0u ? gen.stars[e.x & ~STAR_GENERATED] : stars[e.x];
	float A = uintBitsToFloat (e.y);
	vec3 d = s.pos - pc.observer.xyz;
	float r = length (d);
	float m0 = StarAbsMag (s) + StarDistModulus (r);
	if (m0 + A >= pc.observer.w) return;
	vec4 clip = pc.viewProj * vec4 (RelAberrate (d / r, pc.misc.yzw), 0.0); // (relativistic view: as seen)
	if (clip.w <= 1e-4) return;

	// light per band [S10 deg^2], reddened
	vec4 F = exp2 (-1.328771 * (m0 - 10.0)) * col.band[StarTempIndex (s)] * exp2 (-1.328771 * A * col.extinction);
	vec3 rgb = vec3 (dot (col.rgb[0], F), dot (col.rgb[1], F), dot (col.rgb[2], F));
	// a pixel's solid angle shrinks off the axis as cos^3 (clip.w: the cosine)
	vRadiance = rgb * (pc.sprite.w / (clip.w * clip.w * clip.w));

	// the sprite covers the core, and the glare halos as far as they are
	// brighter than GLARE_MIN
	float R = RADIUS * pc.sprite.z;
	float lum = max (vRadiance.r, max (vRadiance.g, vRadiance.b));
	float thr = GLARE_MIN * pc.misc.x;
	for (int i = 0; i < 2; i++) {
		float sg = pc.glare[i];
		float peak = lum * pc.glare[i + 2] / (6.2831853 * sg * sg);
		if (peak > thr) R = max (R, sg * sqrt (2.0 * log (peak / thr)));
	}
	R = min (R, 512.0);

	vec2 corner = vec2 (float (gl_VertexIndex & 1), float (gl_VertexIndex >> 1)) * 2.0 - 1.0;
	vOfs = corner * R;
	clip.xy += vOfs * pc.sprite.xy * clip.w;
	gl_Position = clip;
}

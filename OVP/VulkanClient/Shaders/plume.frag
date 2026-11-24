// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Engine plumes: the ray through the pixel is marched through the plume's
// bounding box (plume.glsl), up to the opaque scene (prepass depth),
// accumulating the emission of the hot gas and the extinction and
// scattering of smoke; the air between the camera and the plume dims it
// and adds its own light (aerial perspective).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(set = 1, binding = 0) uniform sampler2D prepass;    // view depth (w; 0: none)
#define PLUME_SET 1
#define PLUME_BINDING 1
// (0: an attitude thruster's jet meeting a surface marched through all its
// box, as before: ORBITER_VK_PLUME_SKIP=0, for tests; Plumes.cpp)
layout(constant_id = 0) const int SKIP_ON = 1;
#define PLUME_SKIP_ON SKIP_ON
#include "plume.glsl"
layout(set = 2, binding = 0) uniform sampler2D exhaustTex; // add-on exhaust texture (tint)
#define ATM_SET 3
#include "atmosphere.glsl"

layout(push_constant) uniform Draw {
	uint index;     // into the plume buffer
	uint scale;     // 2: drawn at half resolution (plume_comp.frag)
	float exposure; // displayed (the last frame's)
} pc;

layout(location = 0) in vec3 vPos;
layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive; // how much temporal filters should rely on this frame
layout(location = 2) out vec2 outDist;     // at half resolution: where it is, by its light (sum of w t [km], sum of w)

float Hash (uvec3 v)
{
	uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
	h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
	return float (h) / 4294967296.0;
}

void main ()
{
	PlumeParams P = plumes[pc.index];
	float re = P.shape.x;
	vec3 d = normalize (vPos);
	vec3 lo, ld;
	float tIn, t1;
	if (!PlumeBox (P, vec3 (0.0), d, lo, ld, tIn, t1)) discard;
	// each pixel once: from the far side of the box
	if (length (vPos) / re < 0.5 * (tIn + t1)) discard;
	float t0 = max (tIn, 0.0);
	float zs;
	if (pc.scale == 2u) {
		// (at half resolution: up to the nearest surface of the four
		// pixels; plume_comp.frag takes the texels by that depth)
		ivec2 f = ivec2 (gl_FragCoord.xy) * 2, fmax = textureSize (prepass, 0) - 1;
		zs = 0.0;
		for (int k = 0; k < 4; k++) {
			float z = texelFetch (prepass, min (f + ivec2 (k & 1, k >> 1), fmax), 0).w;
			if (z > 0.0 && (zs <= 0.0 || z < zs)) zs = z;
		}
	} else zs = texelFetch (prepass, ivec2 (gl_FragCoord.xy), 0).w;
	if (zs > 0.0) t1 = min (t1, zs / (d.z * re));
	if (t1 <= t0) discard;
	if (!PlumeClip (P, lo, ld, t0, t1)) discard;

	// (at half resolution twice the steps: the march's noise, twice as
	// coarse, showed as blotches where the camera is in the exhaust)
	float ss = pc.scale == 2u ? 0.5 : 1.0;
	int nMax = pc.scale == 2u ? 192 : 96;
	int n = PlumeAdaptive (P) ? nMax : int (clamp (ceil ((t1 - t0) / (0.3 * ss)), 12.0, float (nMax))); // (adaptive (MarchPlume): at most nMax)
	float jit = Hash (uvec3 (uvec2 (gl_FragCoord.xy), floatBitsToUint (P.misc.x)));
	PlumeLight l = MarchPlume (P, lo, ld, t0, t1, n, jit, frame.sunColor.rgb, frame.ambient.rgb, frame.sunDir.xyz, true, ss);

	// add-on exhaust textures tint the emission
	if (P.misc2.x > 0.0) {
		vec3 tc = textureLod (exhaustTex, vec2 (0.12, 0.3), 3.0).rgb;
		float lum = dot (tc, vec3 (0.2126, 0.7152, 0.0722));
		if (lum > 1e-3) l.emit *= mix (vec3 (1.0), tc / lum, P.misc2.x);
	}
	vec3 col = l.emit + l.scat;
	float a = 1.0 - l.T;
	// aerial perspective: the air in front of the plume dims it; where the
	// plume hides the background, the air's own light (already in the
	// background) is kept
	if (atm.misc.x > 0.5 && l.tAvg > 0.0) {
		vec3 Ta;
		vec3 Lin = AtmTonemap (Inscatter (normalize (mat3 (atm.viewToPlanet) * d), l.tAvg, Ta));
		col = col * Ta + a * Lin;
	}
	// the flame's highlights rolled off: displayed above half of white its
	// light compressed logarithmically, the core about full white - its
	// streaks, cells and colour seen. Exposed as a camera for the scene the
	// flames light, they were three to ten times white, flat white (the tone
	// curve's shoulder leaves a few percent between those); the light they
	// cast, the exposure and all else as they were
	// what is behind a bright flame hidden by its glare: a camera exposed
	// for the flame records nothing behind it (an SRB flame is some 300
	// times a sunlit surface); its light rolled off below, city lights at
	// full strength shone through the flared exhaust's glow in thin air
	{
		const float VEIL = 6.0;
		float L = dot (col, vec3 (0.2126, 0.7152, 0.0722)) * pc.exposure;
		a = max (a, 1.0 - exp (-VEIL * L));
	}
	{
		const float KNEE = 0.35, SOFT = 0.15;
		float L = dot (col, vec3 (0.2126, 0.7152, 0.0722)) * pc.exposure;
		if (L > KNEE) col *= (KNEE + SOFT * log (1.0 + (L - KNEE) / SOFT)) / L;
	}
	outColor = vec4 (col, a);
	float lum = dot (col, vec3 (0.2126, 0.7152, 0.0722));
	// (weighted by what it does to what is behind: its light as displayed
	// and its opacity)
	float wz = l.tAvg > 0.0 ? lum * pc.exposure + a : 0.0;
	outDist = vec2 (wz * l.tAvg * 1e-3, wz);
	outReactive = clamp (max (a, lum / (1.0 + lum)), 0.0, 0.9);
}

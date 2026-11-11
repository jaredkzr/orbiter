// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: textured, fading out where they meet opaque surfaces
// (soft particles); premultiplied (diffuse) or additive (emissive: alpha
// 0, one pipeline for both), each ring's texture from the frame's table;
// fogged by the smoke volume in front of them (its light and transmittance
// integrated up to each depth: smoke_resolve.comp).
// Lit puffs (the smoke texture: normal, thickness, coverage) are shaded by
// their billows' normals (sunlit sides, shaded crevices, light shining
// through thin edges) and eroded by drifting noise as they age, so that
// their outlines are ragged and churn instead of round

#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "frame.glsl"
#include "smokevol.glsl"
#define PARTICLE_NO_POOL
#include "particles.glsl"
layout(set = 1, binding = 3) uniform sampler3D detailNoise;
layout(set = 1, binding = 4) uniform sampler2D prepass; // view depth (w; 0: none)
layout(set = 1, binding = 23) uniform sampler3D volInteg; // smoke in front: light (rgb), transmittance (a)
layout(set = 2, binding = 0) uniform sampler2D ptex[32];  // the rings' textures (ringInfo bits 0-7)

layout(push_constant) uniform Draw {
	uint count;
	uint flags;   // 32: fogged by the smoke volume (gpuparticle.vert)
	float alpha;
	float pad;
	vec4 bodyPos, bodyVel, omega; // (vertex shader)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vPosView;
layout(location = 3) in float vSize;
layout(location = 4) in vec3 vSun;
layout(location = 5) in vec3 vAmb;
layout(location = 6) in vec4 vMisc;
layout(location = 7) flat in uint vInfo;
layout(location = 8) flat in uint vFlags;
layout(location = 9) in vec3 vLoc;
layout(location = 10) in vec3 vLocDir;

layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

void main ()
{
	vec4 t = texture (ptex[nonuniformEXT (vInfo & 0xffu)], vUV);
	bool emissive = (vInfo & 0x100u) != 0u;
	float a;
	vec3 c;
	// (erosion noise, drifting with age: within the quadrant)
	vec2 lq = fract (vUV * 2.0);
	if ((vInfo & 0x200u) != 0u) {
		// erosion: the thinner parts go first as the puff ages; a trail's
		// smoke and a wreck's column keep their coverage late in life and
		// thin out by opacity instead (a widening band, not separate wisps)
		float n = texture (detailNoise, vec3 (lq * 1.6 + vMisc.z * 7.3, vMisc.w * 0.5 + vMisc.z)).r * 0.6
			+ texture (detailNoise, vec3 (lq * 4.0 - vMisc.z * 3.1, vMisc.w * 0.8 + 0.37)).r * 0.4;
		float thick = t.b;
		uint cls = ParticleClass (vFlags);
		bool band = cls == PC_TRAIL || cls == PC_WRECK_SMOKE || cls == PC_CONTRAIL;
		float e = 0.22 + 0.5 * (band ? min (vMisc.w, 0.2) : vMisc.w);
		float cov = smoothstep (e, e + 0.18, thick + 0.7 * (n - 0.5));
		a = t.a * cov * vColor.a;
		if (band) a *= mix (1.0, 0.5, smoothstep (0.3, 1.0, vMisc.w));
		// the billow's normal (sprite axes -> view: turned, facing the camera)
		vec2 nn = t.rg * 2.0 - 1.0;
		vec2 nr = vec2 (vMisc.x * nn.x - vMisc.y * nn.y, vMisc.y * nn.x + vMisc.x * nn.y);
		vec3 N = normalize (vec3 (nr, -sqrt (max (1.0 - dot (nn, nn), 0.0))));
		vec3 L = frame.sunDir.xyz;
		float wrap = clamp ((dot (N, L) + 0.35) / 1.35, 0.0, 1.0);
		float through = (1.0 - thick) * 0.6; // (light passing through thin edges)
		// (T3: light scattered many times inside a bright puff - dust, spray, steam - reaches its side
		// away from the light and its crevices; a dark one's stays dark: their floors rise with the
		// single-scattering albedo, about as w^2)
		float w2 = clamp (dot (vColor.rgb, vec3 (0.2126, 0.7152, 0.0722)), 0.0, 1.0);
		w2 *= w2;
		float floorL = 0.15 + 0.6 * w2;
		float occl = mix (0.55 + 0.35 * w2, 1.0, thick); // (crevices between billows are darker)
		// (the local lights likewise from their direction: the flames)
		float wrapL = clamp ((dot (N, normalize (vLocDir)) + 0.35) / 1.35, 0.0, 1.0);
		c = vColor.rgb * (vAmb * occl + vSun * (floorL + (1.0 - floorL) * wrap + through) + vLoc * (floorL + (1.0 - floorL) * wrapL + through));
	} else {
		a = t.a * vColor.a;
		// (a plain texture lit by the local lights' direction: shaded as a
		// ball of smoke, its side away from the flames in its own shadow;
		// lit evenly to its rim it read as a disc by a flame at night)
		vec3 loc = vLoc;
		if (!emissive) {
			vec2 nn = lq * 2.0 - 1.0;
			vec2 nr = vec2 (vMisc.x * nn.x - vMisc.y * nn.y, vMisc.y * nn.x + vMisc.x * nn.y);
			vec3 N = normalize (vec3 (nr, -sqrt (max (1.0 - dot (nn, nn), 0.0))));
			float wrapL = clamp ((dot (N, normalize (vLocDir)) + 0.35) / 1.35, 0.0, 1.0);
			loc *= 0.15 + 0.85 * wrapL + 0.6 * (1.0 - t.a) * wrapL;
		}
		c = t.rgb * vColor.rgb * (vSun + vAmb + loc + (emissive ? vec3 (1.0) : vec3 (0.0)));
		if ((vFlags & PF_ERODE) != 0u) {
			// a flame breaking up as it cools: its thinner parts (the
			// texture's coverage) burn away first, the threshold rising with
			// its age
			float n = texture (detailNoise, vec3 (lq * 1.4 + vMisc.z * 5.1, vMisc.w * 0.7 + vMisc.z)).r * 0.6
				+ texture (detailNoise, vec3 (lq * 3.6 - vMisc.z * 2.3, vMisc.w * 1.1 + 0.61)).r * 0.4;
			// (a soft edge: a hard one cut each flame out as a disc)
			float e = 0.06 + 0.6 * vMisc.w;
			a *= smoothstep (e, e + 0.35, t.a + 0.8 * (n - 0.5));
		}
	}
	float zs = texelFetch (prepass, ivec2 (gl_FragCoord.xy), 0).w;
	if (zs > 0.0) a *= clamp ((zs - vPosView.z) / max (0.6 * vSize, 0.05), 0.0, 1.0);
	if (a <= 0.001) discard;
	// the smoke in front of it (the volume was drawn first, over all of the
	// scene behind: this sprite's light dimmed by the smoke in front, and
	// that smoke's light in place of what the sprite hides of it)
	vec4 fog = vec4 (0.0, 0.0, 0.0, 1.0);
	if ((pc.flags & 32u) != 0u)
		fog = textureLod (volInteg, vec3 (gl_FragCoord.xy / frame.jitter.zw, VolSlice (vPosView.z)), 0.0);
	if (emissive) {
		// additive; fire: brighter, and partly opaque (it hides the sky). A
		// flame young is brightest; as it cools its light dims faster than
		// its cover, which thickens: it turns to soot - dark rims round the
		// ball, not see-through ones (the sky through them read as a pale
		// lavender halo of puffs)
		float age = vMisc.w;
		float glow = mix (4.5, 3.0, smoothstep (0.0, 0.5, age));
		float cover = a * (1.6 - 0.6 * smoothstep (0.0, 0.3, age)) * (1.0 + 0.6 * smoothstep (0.3, 0.75, age)) * (1.0 - smoothstep (0.88, 1.0, age));
		outColor = ParticleClass (vFlags) == PC_FIRE ? vec4 (glow * c * a * fog.a, min (cover, 0.92)) : vec4 (c * a * fog.a, 0.0);
		float lum = dot (c * a, vec3 (0.2126, 0.7152, 0.0722));
		outReactive = clamp (lum / (1.0 + lum), 0.0, 0.9);
	} else {
		outColor = vec4 (a * (c * fog.a + fog.rgb), a);
		outReactive = clamp (a, 0.0, 0.9);
	}
}

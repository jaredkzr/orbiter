// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: a camera-facing quad per particle of a stream's pool
// (4 vertices, triangle strip), turned by a random angle, drawn far to
// near (sorted index list); the light reaching it is passed on (sun with
// its shadows and forward scattering, ambient and local lights) for the
// fragment shader to shade lit puffs

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
struct Particle { // (particles.glsl)
	vec4 posAge, velLife, shape;
	vec3 light;
	uint flags;
};
layout(set = 1, binding = 0, std430) readonly buffer ParticlePoolR { Particle particles[]; };
layout(set = 1, binding = 8, std430) readonly buffer SortedR { uint sorted[]; }; // pool slots, far to near within each ring

layout(push_constant) uniform Draw {
	uint first;   // the stream's pool
	uint flags;   // 1: emissive, 2: lit puffs (normal and thickness texture), 4: sorted, 8: volumetric particles are in the smoke volume
	float alpha;  // opacity factor
	float pad;
	vec4 bodyPos; // the planet nearby (camera-relative), w: present
	vec4 bodyVel; // its velocity (global frame)
	vec4 omega;   // its angular velocity (global frame)
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;   // emissive/unlit colour, or albedo (rgb); opacity (a)
layout(location = 2) out vec3 vPosView;
layout(location = 3) out float vSize;
layout(location = 4) out vec3 vSun;     // sunlight on the particle (with forward scattering)
layout(location = 5) out vec3 vAmb;     // ambient and local light
layout(location = 6) out vec4 vMisc;    // sprite rotation (cos, sin), seed, age fraction

uint Hash (uint x)
{
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}

void main ()
{
	uint idx = pc.first + uint (gl_InstanceIndex);
	uint slot = (pc.flags & 4u) != 0u ? sorted[idx] : idx;
	Particle pt = particles[slot];
	vec4 posAge = pt.posAge, velLife = pt.velLife, shape = pt.shape;
	float age = posAge.w, life = velLife.w;
	float tl = life > 0.0 ? age / life : 1.0;
	float a = life > 0.0 ? pc.alpha * shape.z * (1.0 - tl) * smoothstep (0.0, 0.08, tl) : 0.0;
	if (a <= 0.002) { gl_Position = vec4 (0.0, 0.0, -2.0, 1.0); return; } // (outside the clip volume)
	uint flags = pt.flags;
	if ((flags & 64u) != 0u && (pc.flags & 8u) != 0u) { gl_Position = vec4 (0.0, 0.0, -2.0, 1.0); return; }
	vec3 vp = transpose (mat3 (frame.viewToGlobal)) * posAge.xyz;
	float h = 0.5 * (shape.x + shape.y * age);

	// corner, turned by an angle of this particle (slowly turning)
	uint hs = Hash (slot * 2654435761u);
	float ang = float (hs & 0xffffu) / 65536.0 * 6.2831853 + age * (float ((hs >> 16) & 0xffu) / 255.0 - 0.5) * 0.6;
	vec2 c = vec2 (float (gl_VertexIndex & 1), float (gl_VertexIndex >> 1)) * 2.0 - 1.0;
	vec2 cs = vec2 (cos (ang), sin (ang));
	vec2 r = vec2 (cs.x * c.x - cs.y * c.y, cs.y * c.x + cs.x * c.y) * h;
	vMisc = vec4 (cs, float (hs >> 24) / 255.0, tl);
	vec2 q = vec2 (float (flags & 1u), float ((flags >> 1) & 1u)) * 0.5;
	vUV = q + (c * 0.5 + 0.5) * 0.5;
	vSize = h;
	vec3 pos = vp + vec3 (r, 0.0);
	if ((flags & 32u) != 0u && pc.bodyPos.w > 0.5) {
		// thrown dust: streaks along its motion over the ground (as seen in
		// a frame's time)
		vec3 vrel = velLife.xyz - pc.bodyVel.xyz - cross (posAge.xyz - pc.bodyPos.xyz, pc.omega.xyz);
		vec3 vv = transpose (mat3 (frame.viewToGlobal)) * vrel;
		vec3 n = normalize (vp);
		vec3 sv = vv - n * dot (vv, n);
		float len = length (sv);
		if (len > 1e-3) {
			vec3 ax = sv / len, ay = normalize (cross (n, ax));
			float l = h + 0.05 * len;
			a *= h / l; // (the same material spread along the streak)
			pos = vp + ax * (c.x * l) + ay * (c.y * h);
			vMisc.xy = vec2 (1.0, 0.0);
		}
	}
	gl_Position = frame.proj * vec4 (pos, 1.0);
	vPosView = pos;

	vec3 albedo = (flags & 8u) != 0u
		? vec3 (float ((flags >> 8) & 0xffu), float ((flags >> 16) & 0xffu), float (flags >> 24)) / 255.0 : vec3 (1.0);
	if ((pc.flags & 1u) != 0u) {
		vColor = vec4 (1.0, 1.0, 1.0, a);
		vSun = vAmb = vec3 (0.0);
		return;
	}
	// forward scattering of sunlight (stronger through thin smoke)
	float g = 0.35, cosT = dot (normalize (vp), frame.sunDir.xyz);
	float phase = (1.0 - g * g) / pow (1.0 + g * g - 2.0 * g * cosT, 1.5); // (times 4 pi)
	vSun = pt.light * (0.75 + 0.5 * phase) * 0.9;
	vec3 amb = frame.ambient.rgb;
	// local lights (engine plumes): scattered in all directions, unshadowed
	int nl = int (frame.lightCount.x);
	for (int k = 0; k < nl; k++) {
		LocalLight l = frame.lights[k];
		float d = length (l.pos.xyz - vp);
		if (d > l.pos.w) continue;
		float la = min (1.0 / max (l.att.x + l.att.y * d + l.att.z * d * d, 1e-4), 1.0);
		float x = clamp (1.0 - pow (d / l.pos.w, 4.0), 0.0, 1.0);
		la *= x * x;
		if (l.dir.w > 1.5) la *= smoothstep (l.spot.x, l.att.w, dot (normalize (vp - l.pos.xyz), l.dir.xyz));
		amb += l.col.rgb * (0.8 * la);
	}
	vAmb = amb;
	vColor = vec4 (albedo, a);
}

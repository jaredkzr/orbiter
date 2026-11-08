// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: a camera-facing quad per particle (4 vertices, triangle
// strip), turned by a random angle, all of them drawn far to near in one
// indirect draw (the sorted list of particle_sortkeys.comp); how each
// ring's particles are drawn (texture, emissive) as registered for the
// frame (ringInfo; not registered: not drawn). The light reaching it is
// passed on (sun with its shadows and forward scattering, ambient and
// local lights) for the fragment shader to shade lit puffs

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#define PARTICLE_NO_POOL
#define PARTICLE_WIND_SET 1 // (streaks relative to the air)
#define PARTICLE_WIND_QUAL readonly
#include "particles.glsl"
layout(set = 1, binding = 0, std430) readonly buffer ParticlePoolR { Particle particles[]; };
layout(set = 1, binding = 8, std430) readonly buffer SortedR { uint sorted[]; }; // pool slots, far to near
// how each ring is drawn this frame (at its first block): bits 0-7 texture
// slot, 8: emissive, 9: lit puffs (the smoke texture), 31: registered
layout(set = 1, binding = 22, std430) readonly buffer RingInfo { uint ringInfo[]; };

layout(push_constant) uniform Draw {
	uint count;   // the sorted list's size (entries beyond: not sorted, not drawn)
	uint flags;   // 8: volumetric particles drawn otherwise (smoke volume, screen-space smoke), 16: only volumetric particles,
	              // unregistered rings too (screen-space smoke), 32: sprites fogged by the smoke volume
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
layout(location = 7) flat out uint vInfo; // the ring's (ringInfo)
layout(location = 8) flat out uint vFlags; // the particle's (PF_*, class)

uint Hash (uint x)
{
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}

void main ()
{
	if (uint (gl_InstanceIndex) >= pc.count) { gl_Position = vec4 (0.0, 0.0, -2.0, 1.0); return; }
	uint slot = sorted[gl_InstanceIndex];
	Particle pt = particles[slot];
	uint info = ringInfo[pt.ring];
	if ((info & 0x80000000u) == 0u && (pc.flags & 16u) == 0u) { gl_Position = vec4 (0.0, 0.0, -2.0, 1.0); return; } // (not drawn this frame)
	vInfo = info;
	vFlags = pt.flags;
	vec4 posAge = pt.posAge, velLife = pt.velLife, shape = pt.shape;
	float age = posAge.w, life = velLife.w;
	float tl = life > 0.0 ? age / life : 1.0;
	float a = life > 0.0 ? pc.alpha * ParticleAlpha (pt) : 0.0;
	bool trail = (pt.flags & PF_TRAIL) != 0u;
	if (a <= 0.002) { gl_Position = vec4 (0.0, 0.0, -2.0, 1.0); return; } // (outside the clip volume)
	uint flags = pt.flags;
	// (volumetric particles in the smoke volume or the screen-space smoke:
	// not drawn as sprites; the screen-space pass draws only them)
	bool vol = (flags & PF_VOLUMETRIC) != 0u;
	vec3 vp = transpose (mat3 (frame.viewToGlobal)) * posAge.xyz;
	// (the smoke volume reaches 20 km (smokevol.glsl): beyond, its
	// particles are sprites, blended over the last 4 km (smoke_splat.comp);
	// a launch's smoke seen from afar. By distance, not depth: the same as
	// the view turns, and the client leaves the volume out while all are
	// beyond)
	if (vol && (pc.flags & 8u) != 0u) {
		float far = smoothstep (16000.0, 20000.0, length (vp));
		if (far <= 0.0) { gl_Position = vec4 (0.0, 0.0, -2.0, 1.0); return; }
		a *= far;
	}
	if (!vol && (pc.flags & 16u) != 0u) { gl_Position = vec4 (0.0, 0.0, -2.0, 1.0); return; }
	float h = 0.5 * (shape.x + shape.y * age);
	if ((flags & PF_SPECK) != 0u) {
		// specks: at least about a pixel across (fainter for it: a bright
		// spark stays bright, a tiny fragment shows mostly as it glints)
		float px = 2.0 * length (posAge.xyz) / (frame.proj[1][1] * max (frame.jitter.w, 1.0));
		float hmin = 1.0 * px;
		if (h < hmin) a *= clamp (4.0 * (h / hmin) * (h / hmin), (flags & PF_EMISSIVE) != 0u ? 0.3 : 0.6, 1.0), h = hmin;
	}

	// corner, turned by an angle of this particle (slowly turning)
	uint hs = Hash (slot * 2654435761u);
	float ang = float (hs & 0xffffu) / 65536.0 * 6.2831853 + age * (float ((hs >> 16) & 0xffu) / 255.0 - 0.5) * (trail ? 0.02 : 0.6);
	vec2 c = vec2 (float (gl_VertexIndex & 1), float (gl_VertexIndex >> 1)) * 2.0 - 1.0;
	vec2 cs = vec2 (cos (ang), sin (ang));
	vec2 r = vec2 (cs.x * c.x - cs.y * c.y, cs.y * c.x + cs.x * c.y) * h;
	vMisc = vec4 (cs, float (hs >> 24) / 255.0, tl);
	vec2 q = vec2 (float (flags & 1u), float ((flags >> 1) & 1u)) * 0.5;
	vUV = q + (c * 0.5 + 0.5) * 0.5;
	vSize = h;
	vec3 pos = vp + vec3 (r, 0.0);
	// streaks: thrown dust along its motion over the ground (as seen in a
	// frame's time); sparks, plasma wakes (by class or the emission) along
	// their motion through the air
	float streak = ParticleStreak (flags);
	bool ballistic = (flags & PF_BALLISTIC) != 0u;
	if ((ballistic || streak > 0.0) && pc.bodyPos.w > 0.5) {
		vec3 vrel = ballistic ? velLife.xyz - pc.bodyVel.xyz - cross (posAge.xyz - pc.bodyPos.xyz, pc.omega.xyz)
			: velLife.xyz - AirVelocity (posAge.xyz, pc.bodyPos, pc.bodyVel.xyz, pc.omega.xyz);
		vec3 vv = transpose (mat3 (frame.viewToGlobal)) * vrel;
		vec3 n = normalize (vp);
		vec3 sv = vv - n * dot (vv, n);
		float len = length (sv);
		if (len > 1e-3) {
			vec3 ax = sv / len, ay = normalize (cross (n, ax));
			float l = h + (ballistic ? 0.05 : streak) * len;
			a *= h / l; // (the same material (or light) spread along the streak)
			pos = vp + ax * (c.x * l) + ay * (c.y * h);
			vMisc.xy = vec2 (1.0, 0.0);
		}
	}
	gl_Position = frame.proj * vec4 (RelApparent (pos), 1.0);
	vPosView = pos;

	vec3 albedo = ParticleAlbedo (pt);
	if ((info & 0x100u) != 0u) {
		// emissive: its tint (albedo when tinted), a flame's colour cooling
		// over its life (PF_RAMP)
		vColor = vec4 ((flags & PF_RAMP) != 0u ? ParticleRamp (tl, ParticleClass (flags), albedo) : albedo, a);
		vSun = vAmb = vec3 (0.0);
		return;
	}
	// forward scattering of sunlight (stronger through thin smoke)
	float g = 0.35, cosT = dot (normalize (vp), frame.sunDir.xyz);
	float phase = (1.0 - g * g) / pow (1.0 + g * g - 2.0 * g * cosT, 1.5); // (times 4 pi)
	vec3 light = UnpackRGB9E5 (pt.light);
	vSun = light * (0.75 + 0.5 * phase) * 0.9;
	float ambScale = 1.0;
	if ((flags & (PF_SPECK | PF_EMISSIVE)) == PF_SPECK) {
		// a tumbling fragment: its lit face and its dark side turning
		// towards the camera in turn (seen against any background), and
		// sunlight glinting off it now and then
		uint gh = Hash (slot * 747796405u + 1u);
		float w = 3.0 + 12.0 * float (gh & 0xffu) / 255.0, ph0 = float ((gh >> 8) & 0xffffu) / 65536.0 * 6.2831853;
		float s = abs (sin (age * w + ph0)), face = 0.5 + 0.5 * sin (age * w * 0.5 + 1.7 * ph0);
		vSun = light * (0.05 + 1.1 * face * face + 6.0 * pow (s, 40.0));
		ambScale = 0.3 + face;
	}
	vec3 amb = frame.ambient.rgb;
	// local lights (engine plumes): scattered in all directions, unshadowed
	int nl = int (frame.lightCount.x);
	for (int k = 0; k < nl; k++) {
		LocalLight l = frame.lights[k];
		if (l.dir.w > 2.5) { amb += l.col.rgb * 0.8; continue; } // (a star: from afar)
		float d = length (l.pos.xyz - vp);
		if (d > l.pos.w) continue;
		float la = min (1.0 / max (l.att.x + l.att.y * d + l.att.z * d * d, 1e-4), 1.0);
		float x = clamp (1.0 - pow (d / l.pos.w, 4.0), 0.0, 1.0);
		la *= x * x;
		if (l.dir.w > 1.5) la *= smoothstep (l.spot.x, l.att.w, dot (normalize (vp - l.pos.xyz), l.dir.xyz));
		amb += l.col.rgb * (0.8 * la);
	}
	vAmb = amb * ambScale;
	vColor = vec4 (albedo, a);
}

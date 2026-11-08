// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles (Particles.cpp): the particle pool and the emissions of a
// frame. Positions are relative to the camera (shifted every frame), so
// that floats keep their precision; velocities are in the global frame.
// (PARTICLE_NO_POOL: the record only, for shaders that bind the pool
// elsewhere)

struct Particle {  // 64 bytes
	vec4 posAge;   // position (camera-relative, global axes) [m], age [s]
	vec4 velLife;  // velocity (global frame) [m/s], lifetime [s] (0: none)
	vec4 shape;    // size at birth [m], growth [m/s], opacity at birth, drag in air [1/s] (>= 0)
	uint light;    // sunlight reaching it (RGB9E5; particle_light.glsl)
	uint flags;    // PF_* (bits 0-15), class (16-23: ParticleSystem::Class), 24-31 streak (ParticleStreak)
	uint albedo;   // colour of smoke (RGBA8; a: spare)
	uint ring;     // its ring (first block in the pool): how it is drawn (ParticleSystem)
};

// flags (bits 0-1: the texture's quadrant)
const uint PF_EMISSIVE = 4u;     // emissive
const uint PF_TINTED = 8u;       // smoke of a colour (albedo)
const uint PF_HOT = 16u;         // rises
const uint PF_BALLISTIC = 32u;   // gravity (thrown dust)
const uint PF_VOLUMETRIC = 64u;  // in the smoke volume
const uint PF_SPECK = 128u;      // fragments, sparks: at least a pixel, under gravity, not streaked
const uint PF_TRAIL = 256u;      // a trail's smoke: long-lived, carried by the wind
const uint PF_RAMP = 512u;       // emissive: cools over its life (ParticleRamp: white-yellow, orange, dark red; dimming)
const uint PF_ERODE = 1024u;     // emissive: eroded by noise as it ages (flames break up into tongues)
uint ParticleClass (uint flags) { return (flags >> 16) & 0xffu; }
// classes (ParticleSystem::Class)
const uint PC_ENGINE_SMOKE = 0u, PC_TRAIL = 1u, PC_DUST = 2u, PC_SPRAY = 3u, PC_FIRE = 4u, PC_FIREBALL_SMOKE = 5u,
	PC_SPARK = 6u, PC_FRAGMENT = 7u, PC_LEGACY = 8u, PC_PLASMA_WAKE = 9u, PC_CONTRAIL = 10u, PC_WRECK_SMOKE = 11u,
	PC_STEAM = 12u, PC_PUFF = 13u;

vec3 ParticleAlbedo (Particle p)
{
	return (p.flags & PF_TINTED) != 0u ? unpackUnorm4x8 (p.albedo).rgb : vec3 (1.0);
}

// how long a particle's motion is drawn as a streak (relative to the air)
// [s]: bits 24-31 of the flags (0: the class's, 255: none, else 2 ms
// units); sparks and plasma wakes are streaked by default
float ParticleStreak (uint flags)
{
	uint s = flags >> 24;
	if (s == 255u) return 0.0;
	if (s != 0u) return float (s) * 0.002;
	uint c = ParticleClass (flags);
	return c == PC_SPARK ? 0.03 : c == PC_PLASMA_WAKE ? 0.04 : 0.0;
}

// the colour of a cooling flame at the fraction t of its life (emission,
// times its brightness: falling as (1 - t)^1.5): white-yellow, orange, dark
// red. tint: the emission's colour (albedo; white when not tinted): fire
// is tinted by it throughout, a plasma wake starts at it and cools to the
// ramp's colours
vec3 ParticleRamp (float t, uint cls, vec3 tint)
{
	const vec3 K0 = vec3 (1.0, 0.93, 0.72), K1 = vec3 (1.0, 0.55, 0.15), K2 = vec3 (0.5, 0.08, 0.02);
	t = clamp (t, 0.0, 1.0);
	vec3 c = t < 0.35 ? mix (K0, K1, t / 0.35) : mix (K1, K2, (t - 0.35) / 0.65);
	c = cls == PC_PLASMA_WAKE ? mix (tint, c, smoothstep (0.0, 0.8, t)) : c * tint;
	return c * pow (1.0 - t, 1.5);
}

// current opacity of a particle (as drawn)
float ParticleAlpha (Particle p)
{
	float t = p.posAge.w / p.velLife.w;
	// (a cooling flame: dims by its colour, ParticleRamp; it fades in only)
	if ((p.flags & PF_RAMP) != 0u) return p.shape.z * smoothstep (0.0, 0.08, t);
	float a = p.shape.z * (1.0 - t) * smoothstep (0.0, 0.08, t);
	// (a trail's smoke: spread over more as it grows (the puffs at a fixed
	// spacing overlap the more, the trail as thick); dense at first, it
	// stays opaque until four times its size (and fading in as the engine's
	// smoke fades: over 25 s, not the lifetime's 8 percent)
	if ((p.flags & PF_TRAIL) != 0u) a *= min (4.0 * p.shape.x / max (p.shape.x + p.shape.y * p.posAge.w, 1e-3), 1.0) * smoothstep (0.0, 25.0, p.posAge.w) / max (smoothstep (0.0, 0.08, t), 1e-3);
	return a;
}
float ParticleRadius (Particle p) { return 0.5 * (p.shape.x + p.shape.y * p.posAge.w); }

#ifdef PARTICLE_WIND_SET
// the wind of the planet nearby at each kilometre of altitude (x east, y
// north) [m/s] (oapiGetModelWind: its layers, whether or not the physics
// has wind)
layout(set = PARTICLE_WIND_SET, binding = 20, std430) PARTICLE_WIND_QUAL buffer WindTable {
	vec4 windLayer[64];
	uint nearVol; // the nearest volumetric particle's distance (float bits; particle_update.glsl, read back)
	uint statsOn; // telemetry (ORBITER_VK_PARTICLE_STATS): particles counted by particle_sortkeys.comp
	uint liveCount, liveVol;
};

// the wind at altitude altKm (local east, north) [m/s]: the layers' values
// interpolated as the core does (cubic Hermite, Catmull-Rom tangents),
// fading out above 58 km (the table's end)
vec2 LayerWind (float altKm)
{
	float a = clamp (altKm, 0.0, 61.0);
	int n = int (a);
	float t = a - float (n), t2 = t * t, t3 = t2 * t;
	vec2 p0 = windLayer[max (n - 1, 0)].xy, p1 = windLayer[n].xy, p2 = windLayer[n + 1].xy, p3 = windLayer[n + 2].xy;
	vec2 m0 = 0.5 * (p2 - p0), m1 = 0.5 * (p3 - p1);
	vec2 w = (2.0 * t3 - 3.0 * t2 + 1.0) * p1 + (t3 - 2.0 * t2 + t) * m0 + (3.0 * t2 - 2.0 * t3) * p2 + (t3 - t2) * m1;
	return w * (1.0 - smoothstep (58.0, 61.0, altKm));
}

// the velocity of the air at pos (camera-relative, global frame): the
// planet's (centre bodyPos, w: radius; velocity bodyVel; angular velocity
// omega), turning with it, and its wind
vec3 AirVelocity (vec3 pos, vec4 bodyPos, vec3 bodyVel, vec3 omega)
{
	vec3 r = pos - bodyPos.xyz;
	vec3 air = bodyVel + cross (r, omega);
	float rl = length (r);
	if (rl <= 0.0 || dot (omega, omega) <= 0.0) return air;
	vec3 up = r / rl;
	vec3 e = cross (normalize (omega), up);
	float el = length (e);
	if (el < 1e-3) return air; // (at a pole)
	vec3 east = e / el, north = cross (up, east);
	vec2 w = LayerWind ((rl - bodyPos.w) * 1e-3);
	return air + east * w.x + north * w.y;
}
#endif

// shared-exponent colour (as VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
uint PackRGB9E5 (vec3 c)
{
	c = clamp (c, vec3 (0.0), vec3 (65408.0));
	float m = max (max (c.r, c.g), c.b);
	int e = max (-16, int (floor (log2 (max (m, 1e-30))))) + 1; // (shared exponent, biased below)
	float s = exp2 (float (e - 9));
	if (uint (floor (m / s + 0.5)) == 512u) e++, s *= 2.0;
	uvec3 q = min (uvec3 (floor (c / s + 0.5)), uvec3 (511u));
	return q.r | (q.g << 9) | (q.b << 18) | (uint (clamp (e + 15, 0, 31)) << 27);
}
vec3 UnpackRGB9E5 (uint v)
{
	float s = exp2 (float (int (v >> 27) - 15 - 9));
	return vec3 (float (v & 511u), float ((v >> 9) & 511u), float ((v >> 18) & 511u)) * s;
}

struct Emission { // must match ParticleSystem::GpuEmission
	vec4 p0;       // source at the start of the step (camera-relative), w: emission speed [m/s]
	vec4 p1;       // source at the end of the step, w: velocity spread [m/s]
	vec4 vel;      // source velocity (global frame), w: lifetime [s]
	vec4 dir;      // emission direction (opposite to the stream), w: step [s]
	vec4 shape;    // size, growth, opacity, drag (>= 0)
	uvec4 slab;    // first particle of the stream's pool, its capacity, first slot to write, count
	uvec4 misc;    // seed, flags (1: emissive, 2: reentry quadrant, 4: tinted (hot smoke), 8: spread along a surface,
	               //   16: ballistic, 32: cold (tinted, not rising), 64: volumetric, 128: speck, 256: spherical spread,
	               //   512: trail, 1024: temperature ramp, 2048: eroded), smoke colour (rgb8), class (bits 0-7) and streak (8-15)
};

#ifndef PARTICLE_NO_POOL
layout(set = 0, binding = 0, std430) buffer ParticlePool { Particle particles[]; };
#endif

uint ParticleHash (uint x)
{
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}
float ParticleRand (inout uint s) { s = ParticleHash (s + 0x9e3779b9u); return float (s) / 4294967296.0; }

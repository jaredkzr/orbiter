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
const uint PF_JET = 2048u;       // a jet's smoke: born narrower than its size of reference, shape.y / JET_GROWTH (ParticleAlpha)
const float JET_GROWTH = 0.65;   // (ParticleSystem::Emission::JET_GROWTH)
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
// is tinted by it throughout, a plasma wake starts at it and cools to violet
// (0.55, 0.28, 0.95), dimming as the ramp does
vec3 ParticleRamp (float t, uint cls, vec3 tint)
{
	const vec3 K0 = vec3 (1.0, 0.93, 0.72), K1 = vec3 (1.0, 0.55, 0.15), K2 = vec3 (0.5, 0.08, 0.02);
	t = clamp (t, 0.0, 1.0);
	vec3 c = t < 0.35 ? mix (K0, K1, t / 0.35) : mix (K1, K2, (t - 0.35) / 0.65);
	c = cls == PC_PLASMA_WAKE ? mix (tint, vec3 (0.55, 0.28, 0.95), smoothstep (0.1, 0.9, t)) : c * tint; // (a plasma cools from its colour to violet, not to a dull red: brown read as smoke)
	return c * pow (1.0 - t, 1.5);
}

// the distances [m] over which a volumetric particle is handed from the smoke
// volume (nearer) to sprites (farther), cross-faded: 16-20 km (the volume's
// reach); a trail's smoke (900 s, large sparse puffs) at 2.5-3.5 km — the
// volume draws it well near (no "cotton balls" at a launch), but from afar
// its sparse puffs became beads, rings and gaps (f9), which sprites draw as
// a continuous band
vec2 VolHandoff (uint flags) { return (flags & PF_TRAIL) != 0u ? vec2 (2500.0, 3500.0) : vec2 (16000.0, 20000.0); }

// current opacity of a particle (as drawn)
float ParticleAlpha (Particle p)
{
	float t = p.posAge.w / p.velLife.w;
	// (a cooling flame: dims by its colour, ParticleRamp; it fades in only)
	if ((p.flags & PF_RAMP) != 0u) return p.shape.z * smoothstep (0.0, 0.08, t);
	// (engine smoke: in within 0.08 s, the long-lived solid motor's smoke
	// too: it leaves the flame's end at the jet's speed, a longer fade-in
	// left a gap between the flame and the column)
	float fin = ParticleClass (p.flags) == PC_ENGINE_SMOKE ? min (0.08, 0.08 / p.velLife.w) : 0.08;
	float a = p.shape.z * (1.0 - t) * smoothstep (0.0, fin, t);
	// (engine smoke spreading: opaque until twice its size, then thinning
	// as the square of it - the same smoke over a larger area; neighbours
	// overlap, the column stays. The solid motors' wide smoke of 40 s grew
	// to puffs hundreds of metres across, still opaque: dark discs behind
	// the lit part of the column, seen from afar at night)
	// (a jet's smoke: born about as wide as the flame's end, narrower than its
	// size of reference, denser by as much - the same smoke; then as above
	// from that size)
	if (ParticleClass (p.flags) == PC_ENGINE_SMOKE) {
		float s0 = (p.flags & PF_JET) != 0u ? max (p.shape.x, p.shape.y / JET_GROWTH) : p.shape.x;
		float k = s0 / max (p.shape.x + p.shape.y * p.posAge.w, 1e-3);
		a *= k >= 1.0 ? k : min (4.0 * k * k, 1.0);
	}
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
	uint liveCls[16]; // (telemetry: the live particles by class, PC_*)
	// jets meeting the ground in air this frame (ParticleSystem::AddGroundJet):
	// per jet xyz the impact (camera-relative), w r0 [m]; the ground's
	// normal, w the outflow's peak speed [m/s]; x reach [m], y the layer's
	// thickness [m]
	uvec4 groundJetCount;
	vec4 groundJet[3 * 8];
	// the dust around a vessel's flames (ParticleSystem::FlameBox; flame_*.comp):
	// the grid's centre (camera-relative), w: 1 = on; its axes over its
	// size (dot (p - centre, axis) + 0.5 in [0, 1] inside), w: a cell's
	// size along each [m]; then the flames' points (camera-relative), w: their
	// weights (summing to 1; 0: none)
	vec4 flameBox[4 + 12];
	// (telemetry, ORBITER_VK_PARTICLE_STATS: particle_update.glsl this frame -
	// x smoke and dust ending the step under the ground near the camera,
	// y specks under it, z specks bounced, w the ground rays' length [m];
	// [1] x smoke and dust in air, y their speed relative to the air [cm/s], summed)
	uvec4 telemetry[2];
	// the carry (FrameState.h; particle_update.glsl CarryPrev): a point at
	// rest in the frame chosen (the ground nearby) from the last update's
	// camera-relative coordinates (C_{n-1}) to this one's (C_n): Q x + T,
	// with [0] xyz T, w the frame (0 global, 1 the body's centre, 2 its
	// ground); [1] xyz Q's rotation as sin (angle) times its axis, w 1 if
	// continuous (no time jump); [2] xyz the frame's displacement over the
	// step (global) at the last camera
	vec4 frameCarry[3];
	// x: the camera's height above the ground under it (a sphere: radius
	// pc.ground.x) [m], from doubles (particle_update.glsl GroundHeight)
	vec4 camGround;
};

// the outward wind along the ground from the jets meeting it at pos: the
// gas turned at the impact spreads as a wall jet, its speed rising to a
// peak r0 out and falling as r0 / r beyond (2 x / (1 + x^2), x = r / r0),
// gone by the reach, in a layer some thickness deep (exp (-h / thick)),
// lifting a little as it goes (a tenth of it up, the dust billowing)
vec3 GroundJetWind (vec3 pos)
{
	vec3 u = vec3 (0.0);
	uint n = min (groundJetCount.x, 8u);
	for (uint k = 0u; k < n; k++) {
		vec4 a = groundJet[3u * k], b = groundJet[3u * k + 1u], c = groundJet[3u * k + 2u];
		vec3 rel = pos - a.xyz;
		float h = dot (rel, b.xyz);
		if (h < -1.0) continue;
		vec3 rad = rel - h * b.xyz;
		float r = length (rad);
		if (r < 1e-3 || r > c.x) continue;
		float x = r / max (a.w, 0.1);
		float f = 2.0 * x / (1.0 + x * x) * (1.0 - smoothstep (0.6 * c.x, c.x, r)) * exp (-max (h, 0.0) / max (c.y, 0.1));
		u += b.w * f * (rad / r + 0.1 * b.xyz);
	}
	return u;
}

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
	if (rl <= 0.0 || dot (omega, omega) <= 0.0) return air + GroundJetWind (pos);
	vec3 up = r / rl;
	vec3 e = cross (normalize (omega), up);
	float el = length (e);
	if (el < 1e-3) return air + GroundJetWind (pos); // (at a pole)
	vec3 east = e / el, north = cross (up, east);
	vec2 w = LayerWind ((rl - bodyPos.w) * 1e-3);
	return air + east * w.x + north * w.y + GroundJetWind (pos);
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

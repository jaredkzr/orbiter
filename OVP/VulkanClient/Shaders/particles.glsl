// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles (Particles.cpp): the particle pool and the emissions of a
// frame. Positions are relative to the camera (shifted every frame), so
// that floats keep their precision; velocities are in the global frame.

struct Particle {
	vec4 posAge;   // position (camera-relative, global axes) [m], age [s]
	vec4 velLife;  // velocity (global frame) [m/s], lifetime [s] (0: none)
	vec4 shape;    // size at birth [m], growth [m/s], opacity at birth, slowdown in air [1/s]
	vec3 light;    // sunlight reaching it
	uint flags;    // bits 0-1: texture quadrant, 2: emissive, 3: tinted, 4: hot (rises), 5: ballistic (gravity), 6: volumetric (smoke volume),
	               // 8-31: colour of smoke (r, g, b 8 bits each)
};

struct Emission { // must match ParticleSystem::GpuEmission
	vec4 p0;       // source at the start of the step (camera-relative), w: emission speed [m/s]
	vec4 p1;       // source at the end of the step, w: velocity spread [m/s]
	vec4 vel;      // source velocity (global frame), w: lifetime [s]
	vec4 dir;      // emission direction (opposite to the stream), w: step [s]
	vec4 shape;    // size, growth, opacity, slowdown
	uvec4 slab;    // first particle of the stream's pool, its capacity, first slot to write, count
	uvec4 misc;    // seed, flags (1: emissive, 2: reentry quadrant, 4: tinted (hot smoke), 8: spread along a surface,
	               //   16: ballistic, 32: cold (tinted, not rising), 64: volumetric), smoke colour (rgb8), -
};

layout(set = 0, binding = 0, std430) buffer ParticlePool { Particle particles[]; };

uint ParticleHash (uint x)
{
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}
float ParticleRand (inout uint s) { s = ParticleHash (s + 0x9e3779b9u); return float (s) / 4294967296.0; }

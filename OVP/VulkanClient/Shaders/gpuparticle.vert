// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: a camera-facing quad per particle of a stream's pool
// (4 vertices, triangle strip), turned by a random angle; diffuse
// particles are lit by the sunlight that reaches them (forward
// scattering) and the ambient light

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
struct Particle { // (particles.glsl)
	vec4 posAge, velLife, shape;
	vec3 light;
	uint flags;
};
layout(set = 1, binding = 0, std430) readonly buffer ParticlePoolR { Particle particles[]; };

layout(push_constant) uniform Draw {
	uint first;   // the stream's pool
	uint flags;   // 1: emissive
	float alpha;  // opacity factor
	float pad;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;  // light on the particle (rgb), opacity (a)
layout(location = 2) out vec3 vPosView;
layout(location = 3) out float vSize;

uint Hash (uint x)
{
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}

void main ()
{
	uint slot = pc.first + uint (gl_InstanceIndex);
	Particle pt = particles[slot];
	vec4 posAge = pt.posAge, velLife = pt.velLife, shape = pt.shape;
	float age = posAge.w, life = velLife.w;
	float a = life > 0.0 ? pc.alpha * shape.z * (1.0 - age / life) * smoothstep (0.0, 0.08, age / life) : 0.0;
	if (a <= 0.002) { gl_Position = vec4 (0.0, 0.0, -2.0, 1.0); return; } // (outside the clip volume)
	uint flags = pt.flags;
	vec3 vp = transpose (mat3 (frame.viewToGlobal)) * posAge.xyz;
	float h = 0.5 * (shape.x + shape.y * age);

	// corner, turned by an angle of this particle (slowly turning)
	uint hs = Hash (slot * 2654435761u);
	float ang = float (hs & 0xffffu) / 65536.0 * 6.2831853 + age * (float ((hs >> 16) & 0xffu) / 255.0 - 0.5) * 0.6;
	vec2 c = vec2 (float (gl_VertexIndex & 1), float (gl_VertexIndex >> 1)) * 2.0 - 1.0;
	vec2 r = vec2 (cos (ang) * c.x - sin (ang) * c.y, sin (ang) * c.x + cos (ang) * c.y);
	vec3 pos = vp + vec3 (r * h, 0.0);
	gl_Position = frame.proj * vec4 (pos, 1.0);
	vec2 q = vec2 (float (flags & 1u), float ((flags >> 1) & 1u)) * 0.5;
	vUV = q + (c * 0.5 + 0.5) * 0.5;
	vPosView = pos;
	vSize = h;

	vec3 col;
	if ((pc.flags & 1u) != 0u) {
		col = vec3 (1.0);
	} else {
		float g = 0.35, cosT = dot (normalize (vp), frame.sunDir.xyz);
		float phase = (1.0 - g * g) / pow (1.0 + g * g - 2.0 * g * cosT, 1.5); // (times 4 pi)
		// (smoke of large droplets scatters light many times: bright from any side)
		col = pt.light * (0.75 + 0.5 * phase) * 0.9 + frame.ambient.rgb;
		if ((flags & 8u) != 0u) // smoke of a colour
			col *= vec3 (float ((flags >> 8) & 0xffu), float ((flags >> 16) & 0xffu), float (flags >> 24)) / 255.0;
	}
	vColor = vec4 (col, a);
}

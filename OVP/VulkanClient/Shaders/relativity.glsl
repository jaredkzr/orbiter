// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Relativistic view: aberration of light (special relativity). A source in
// direction n (unit) for an observer at rest in a frame is seen in the
// returned direction by an observer moving at velocity b (units of c,
// the same frame's axes); b = 0: unchanged. The inverse is aberration by -b.

#ifndef RELATIVITY_GLSL
#define RELATIVITY_GLSL

vec3 RelAberrate (vec3 n, vec3 b)
{
	float b2 = dot (b, b);
	if (b2 <= 0.0) return n;
	float g = inversesqrt (max (1.0 - b2, 1e-12)), nb = dot (n, b);
	return (n + b * (g * g / (g + 1.0) * nb + g)) / (g * (1.0 + nb));
}

// where a point at rest at p (relative to the observer) is seen: aberrated,
// at the distance D |p| of its light's emission in the observer's frame
vec3 RelApparentPos (vec3 p, vec3 b)
{
	float r = length (p), b2 = dot (b, b);
	if (r <= 0.0 || b2 <= 0.0) return p;
	vec3 n = p / r;
	float g = inversesqrt (max (1.0 - b2, 1e-12));
	return RelAberrate (n, b) * (r * g * (1.0 + dot (n, b)));
}

// the direction at rest (b's frame) of what the moving observer sees in direction d
vec3 RelTrueDirOf (vec3 d, vec3 b)
{
	return normalize (RelAberrate (normalize (d), -b));
}

// Doppler factor (observed over emitted frequency) of light the moving
// observer sees from direction d (unit, its own frame): 1 / (gamma (1 - b.d))
float RelDopplerSeen (vec3 d, vec3 b)
{
	float b2 = dot (b, b);
	if (b2 <= 0.0) return 1.0;
	return sqrt (max (1.0 - b2, 1e-12)) / max (1.0 - dot (b, d), 1e-12);
}

#endif

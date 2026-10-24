// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Common definitions of the screen-space reflection passes

layout(push_constant) uniform Params {
	vec2 focal;      // projection factors fx, fy
	vec2 size;       // image size [pixels]
	vec2 jitter;     // projection jitter (NDC)
	float frame;     // frame counter (noise)
	float levels;    // depth pyramid levels
	float thickness; // assumed thickness of surfaces, relative to their depth
	float maxRough;  // reflections fade out up to this roughness
	float reset;     // 1: no valid history
	float pad;
} pc;

const float PI = 3.14159265;

// view-space position of a pixel position at view depth z
vec3 ViewPos (vec2 pix, float z)
{
	vec2 ndc = pix / pc.size * 2.0 - 1.0 - pc.jitter;
	return vec3 (ndc.x * z / pc.focal.x, -ndc.y * z / pc.focal.y, z);
}

// pixel position of a view-space point
vec2 Project (vec3 p)
{
	vec2 ndc = vec2 (pc.focal.x * p.x, -pc.focal.y * p.y) / p.z + pc.jitter;
	return (ndc * 0.5 + 0.5) * pc.size;
}

vec3 OctDecode (vec2 e)
{
	vec3 n = vec3 (e, 1.0 - abs (e.x) - abs (e.y));
	if (n.z < 0.0) n.xy = (1.0 - abs (n.yx)) * vec2 (n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return normalize (n);
}

// GGX distribution (a: roughness squared)
float GGX_D (float NoH, float a)
{
	float a2 = a * a;
	float d = NoH * NoH * (a2 - 1.0) + 1.0;
	return a2 / (PI * d * d);
}

// Smith masking (separable, GGX)
float SmithG1 (float NoX, float a)
{
	float a2 = a * a;
	return 2.0 * NoX / (NoX + sqrt (a2 + (1.0 - a2) * NoX * NoX));
}

float Luma (vec3 c) { return dot (c, vec3 (0.2126, 0.7152, 0.0722)); }

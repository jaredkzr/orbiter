// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// HDR scene + bloom -> display. The curve is the identity up to SHOULDER and
// approaches 1 smoothly above, so that normally lit scenes are unchanged
// and only highlights are compressed.

#version 450

layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 1, binding = 0) uniform sampler2D bloom;
layout(set = 2, binding = 0) readonly buffer Exposure { float logExposure; float avgLogLum; } autoExp;

layout(push_constant) uniform Params {
	float exposure;
	float bloomStrength;
	float flareStrength;
	float aspect;     // width / height
	float autoExposure; // 1: scale by the adapted exposure
	float sunU, sunV; // the sun on the screen (lens flare; floats: a vec2 would be aligned to 8 bytes)
	float sunValid;   // 1: in front of the camera
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float SHOULDER = 0.6;

vec3 Curve (vec3 c)
{
	vec3 hi = SHOULDER + (1.0 - SHOULDER) * (1.0 - exp (-(c - SHOULDER) / (1.0 - SHOULDER)));
	return mix (c, hi, step (vec3 (SHOULDER), c));
}

// Lens flare of the sun: ghost images reflected through the image centre
// at several scales, while the sun's disc is visible
// (bright in the scene image where it should be; engine flames and sunlit
// clouds make none)
vec2 SunUV () { return vec2 (pc.sunU, pc.sunV); }

float SunVisible (float exposure)
{
	if (pc.sunValid < 0.5) return 0.0;
	// the brightest pixel around the sun's position (its disc is only a
	// few pixels across)
	vec2 px = 1.0 / vec2 (textureSize (scene, 0));
	float m = 0.0;
	for (int y = -4; y <= 4; y++)
		for (int x = -4; x <= 4; x++) {
			vec3 c = textureLod (scene, SunUV () + vec2 (x, y) * px, 0.0).rgb;
			m = max (m, max (c.r, max (c.g, c.b)));
		}
	return smoothstep (1.5, 3.0, m * exposure); // (displayed: far brighter than lit surfaces)
}

vec3 Flare (vec2 uv, float vis)
{
	const vec4 G[4] = vec4[] (vec4 (-0.6, 0.9, 0.7, 1.0), vec4 (-0.25, 0.6, 1.0, 0.7),
	                          vec4 (0.45, 1.0, 0.8, 0.5), vec4 (1.4, 0.5, 0.8, 1.0)); // scale, tint
	vec2 a = vec2 (pc.aspect, 1.0);
	vec3 f = vec3 (0.0);
	for (int i = 0; i < 4; i++) {
		vec2 g = vec2 (0.5) - (SunUV () - 0.5) / G[i].x; // (where the ghost of the sun appears)
		float r = 0.03 / abs (G[i].x);
		float d = length ((uv - g) * a);
		float w = pow (max (1.0 - length ((g - 0.5) * a) / 0.9, 0.0), 2.0); // (fading towards the edges)
		f += G[i].yzw * (smoothstep (r, 0.6 * r, d) * w * 2.0);
	}
	return f * vis;
}

void main ()
{
	vec3 c = texture (scene, vUV).rgb + texture (bloom, vUV).rgb * pc.bloomStrength;
	float exposure = pc.exposure * (pc.autoExposure > 0.5 ? exp2 (autoExp.logExposure) : 1.0);
	if (pc.flareStrength > 0.0) {
		float vis = SunVisible (exposure);
		if (vis > 0.0) c += Flare (vUV, vis) * pc.flareStrength / max (exposure, 1e-3); // (displayed the same at any exposure)
	}
	outColor = vec4 (Curve (max (c * exposure, vec3 (0.0))), 1.0);
}

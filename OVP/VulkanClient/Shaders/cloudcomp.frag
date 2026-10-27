// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Volumetric clouds over the scene: light added, scene attenuated by the
// clouds' transmittance (blend: src + dst * src.a)

#version 450

layout(set = 0, binding = 0) uniform sampler2D cloudTex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// the clouds (rendered at a lower resolution) upsampled by a Catmull-Rom
// filter: sharper than the bilinear one (which blurred their detail over
// two pixels); five bilinear lookups (the outer taps' weights combined),
// clamped to the nearest texels' range (no ringing about bright edges)
void main()
{
	vec2 size = vec2(textureSize(cloudTex, 0));
	vec2 p = vUV * size - 0.5;
	vec2 c = floor(p) + 0.5;
	vec2 f = p - floor(p);
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);
	vec2 w12 = w1 + w2;
	vec2 t0 = (c - 1.0) / size, t3 = (c + 2.0) / size, t12 = (c + w2 / w12) / size;
	vec4 col = texture(cloudTex, vec2(t12.x, t0.y)) * (w12.x * w0.y)
	         + texture(cloudTex, vec2(t0.x, t12.y)) * (w0.x * w12.y)
	         + texture(cloudTex, t12) * (w12.x * w12.y)
	         + texture(cloudTex, vec2(t3.x, t12.y)) * (w3.x * w12.y)
	         + texture(cloudTex, vec2(t12.x, t3.y)) * (w12.x * w3.y);
	col /= w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
	ivec2 i = ivec2(floor(p));
	ivec2 m = ivec2(size) - 1;
	vec4 a = texelFetch(cloudTex, clamp(i, ivec2(0), m), 0), b = texelFetch(cloudTex, clamp(i + ivec2(1, 0), ivec2(0), m), 0);
	vec4 d = texelFetch(cloudTex, clamp(i + ivec2(0, 1), ivec2(0), m), 0), e = texelFetch(cloudTex, clamp(i + ivec2(1, 1), ivec2(0), m), 0);
	outColor = clamp(col, min(min(a, b), min(d, e)), max(max(a, b), max(d, e)));
}

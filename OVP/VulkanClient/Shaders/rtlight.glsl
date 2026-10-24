// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Ray traced lighting visibility, common definitions (RTLighting.cpp)

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D prepass;  // view normal (xyz), view depth (w; 0: none)
layout(set = 0, binding = 1) uniform sampler2D velocity; // screen motion since the last frame [uv]
layout(set = 0, binding = 10) uniform sampler2D material; // shading normal, roughness (negative: terrain), reflectivity
layout(set = 0, binding = 2, rgba16f) uniform image2D rawA;   // sun, ambient occlusion, planet light, -
layout(set = 0, binding = 3, rgba8) uniform image2D rawL;     // local lights 0..3
layout(set = 0, binding = 4) uniform sampler2D histA;    // previous accumulation (w: view depth)
layout(set = 0, binding = 5) uniform sampler2D histL;
layout(set = 0, binding = 6, rgba16f) uniform image2D accA;   // this frame's accumulation
layout(set = 0, binding = 7, rgba8) uniform image2D accL;
layout(set = 0, binding = 8, rgba16f) uniform writeonly image2D outA; // filtered (scene shaders)
layout(set = 0, binding = 9, rgba8) uniform writeonly image2D outL;

layout(push_constant) uniform Params {
	vec2 focal;    // projection factors fx, fy
	vec2 size;     // image size [pixels]
	vec2 jitter;   // projection jitter (NDC)
	float frame;   // frame counter (noise)
	float reset;   // 1: no valid history
	float aoRadius;   // [m]
	float aoMaxDist;  // ambient occlusion fades out up to this view depth [m]
	float sunRadius;  // angular radius of the sun [rad]
	float nLights;    // local lights with shadows
} pc;

vec3 ViewPos (vec2 pix, float z)
{
	vec2 ndc = pix / pc.size * 2.0 - 1.0 - pc.jitter;
	return vec3 (ndc.x * z / pc.focal.x, -ndc.y * z / pc.focal.y, z);
}

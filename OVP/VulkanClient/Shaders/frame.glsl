// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Per-frame uniform block (set 0, binding 0); must match FrameUniforms in Scene.cpp

const int MAX_LIGHTS = 32;

struct LocalLight {
	vec4 pos;    // view space position (xyz), range (w)
	vec4 dir;    // view space direction (xyz), type (w: 1 point, 2 spot)
	vec4 col;    // colour times intensity
	vec4 att;    // attenuation coefficients (xyz), cos(umbra/2) (w)
	vec4 spot;   // cos(penumbra/2) (x)
};

layout(set = 0, binding = 0) uniform Frame {
	mat4 proj;         // view -> clip (reversed z, infinite far plane)
	vec4 sunDir;       // direction towards the sun, view space
	vec4 sunColor;
	vec4 ambient;
	mat4 shadowMat;    // view space -> shadow map (texture coordinates, depth)
	vec4 shadowParams; // x: enabled, y: normal offset [m], z: depth bias, w: texel size
	vec4 lightCount;   // x: number of local lights
	LocalLight lights[MAX_LIGHTS];
} frame;

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
	mat4 shadowMat[3];    // view space -> shadow map (texture coordinates, depth), per cascade
	vec4 shadowParams[3]; // x: enabled, y: normal offset [m], z: depth bias, w: texel size
	vec4 jitter;       // xy: projection jitter (NDC), zw: render size [pixels]
	vec4 aoParams;     // 1/width, 1/height of the ambient occlusion image, z: fraction applied to direct light, w: 1 = off
	mat4 viewToGlobal; // view space -> global frame (rotation)
	vec4 probe;        // x: environment probe valid, y: highest mip level
	vec4 lightCount;   // x: number of local lights
	LocalLight lights[MAX_LIGHTS];
} frame;

// ambient occlusion of opaque geometry (1: unoccluded); fragment shaders only
#ifdef FRAME_AO
layout(set = 0, binding = 3) uniform sampler2D aoMap;
float AmbientOcclusion () { return frame.aoParams.w > 0.5 ? 1.0 : texture(aoMap, gl_FragCoord.xy * frame.aoParams.xy).r; }
#endif

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Per-frame uniform block (set 0, binding 0; FRAME_SET selects another set);
// must match FrameUniforms in Scene.cpp

#ifndef FRAME_SET
#define FRAME_SET 0
#endif

const int MAX_LIGHTS = 32;

struct LocalLight {
	vec4 pos;    // view space position (xyz), range (w)
	vec4 dir;    // view space direction (xyz), type (w: 1 point, 2 spot)
	vec4 col;    // colour times intensity
	vec4 att;    // attenuation coefficients (xyz), cos(umbra/2) (w)
	vec4 spot;   // cos(penumbra/2) (x), id of the vessel carrying it (y), shadow rays start this far along dir (z)
};

layout(set = FRAME_SET, binding = 0) uniform Frame {
	mat4 proj;         // view -> clip (reversed z, infinite far plane)
	vec4 sunDir;       // direction towards the sun, view space
	vec4 sunColor;
	vec4 ambient;
	mat4 shadowMat[3];    // view space -> shadow map (texture coordinates, depth), per cascade
	vec4 shadowParams[3]; // x: enabled, y: normal offset [m], z: depth bias, w: texel size
	vec4 jitter;       // xy: projection jitter (NDC), zw: render size [pixels]
	vec4 aoParams;     // 1/width, 1/height of the ambient occlusion image, z: fraction applied to direct light, w: 1 = off
	mat4 viewToGlobal; // view space -> global frame (rotation)
	vec4 probe;        // x: environment probe valid, y: highest mip level, z: screen-space reflections valid, w: their debug view
	vec4 rt;           // x: ray traced lighting valid (sun shadows, ambient occlusion, planet light, light shadows), y: shadowed local lights
	vec4 planetDir;    // light of the planet nearby: direction to its centre (view space), w: sine of its angular radius
	vec4 planetLight;  // its irradiance at normal incidence (rgb), w: 1 = present
	vec4 lightCount;   // x: number of local lights
	LocalLight lights[MAX_LIGHTS];
} frame;

// ambient occlusion of opaque geometry (1: unoccluded); fragment shaders only
#ifdef FRAME_AO
layout(set = FRAME_SET, binding = 3) uniform sampler2D aoMap;
// ray traced lighting visibility of prepass geometry (same size): sun,
// ambient occlusion, planet light; local lights 0..3
layout(set = FRAME_SET, binding = 6) uniform sampler2D rtVisMap;
layout(set = FRAME_SET, binding = 7) uniform sampler2D rtLightMap;
float AmbientOcclusion ()
{
	if (frame.aoParams.w > 0.5) return 1.0;
	if (frame.rt.x > 0.5) return texture(rtVisMap, gl_FragCoord.xy * frame.aoParams.xy).g;
	return texture(aoMap, gl_FragCoord.xy * frame.aoParams.xy).r;
}
// screen-space reflections (same size as the occlusion image): reflected
// radiance (rgb, premultiplied with a) and its confidence (a); ofs: offset [uv]
layout(set = FRAME_SET, binding = 5) uniform sampler2D ssrMap;
vec4 ScreenReflection (vec2 ofs) { return frame.probe.z > 0.5 ? texture(ssrMap, gl_FragCoord.xy * frame.aoParams.xy + ofs) : vec4(0.0); }
bool RTLighting () { return frame.rt.x > 0.5; }
vec4 RTVisibility () { return texture(rtVisMap, gl_FragCoord.xy * frame.aoParams.xy); }
vec4 RTLightVisibility () { return texture(rtLightMap, gl_FragCoord.xy * frame.aoParams.xy); }
// light of the planet nearby on a surface with normal N (a large source:
// it reaches around the terminator of the surface)
vec3 PlanetLight (vec3 N)
{
	if (frame.planetLight.w < 0.5) return vec3(0.0);
	float s = frame.planetDir.w;
	float c = clamp((dot(N, frame.planetDir.xyz) + s) / (1.0 + s), 0.0, 1.0);
	return frame.planetLight.rgb * c * c;
}
#endif

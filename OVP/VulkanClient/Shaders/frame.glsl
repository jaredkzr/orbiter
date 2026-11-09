// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Per-frame uniform block (set 0, binding 0; FRAME_SET selects another set);
// must match FrameUniforms in Scene.cpp

#include "relativity.glsl"

#ifndef FRAME_SET
#define FRAME_SET 0
#endif

const int MAX_LIGHTS = 32;

struct LocalLight {
	vec4 pos;    // view space position (xyz), range (w)
	vec4 dir;    // view space direction (xyz), type (w: 1 point, 2 spot, 3 a star: directional, towards it)
	vec4 col;    // colour times intensity
	vec4 att;    // attenuation coefficients (xyz), cos(umbra/2) (w; a star: its angular radius; a point light: its source's radius [m])
	vec4 spot;   // cos(penumbra/2) (x), id of the vessel carrying it (y), shadow rays start this far along dir (z)
};

layout(set = FRAME_SET, binding = 0) uniform Frame {
	mat4 proj;         // view -> clip (reversed z, infinite far plane)
	vec4 sunDir;       // direction towards the sun, view space
	vec4 sunColor;
	vec4 ambient;      // w: the share of it left on the night side at the camera (particles)
	mat4 shadowMat[3];    // view space -> shadow map (texture coordinates, depth), per cascade
	vec4 shadowParams[3]; // x: enabled, y: normal offset [m], z: depth bias, w: texel size
	vec4 jitter;       // xy: projection jitter (NDC), zw: render size [pixels]
	vec4 aoParams;     // 1/width, 1/height of the ambient occlusion image, z: fraction applied to direct light, w: 1 = off
	mat4 viewToGlobal; // view space -> global frame (rotation)
	vec4 probe;        // x: environment probe valid, y: highest mip level, z: screen-space reflections valid, w: their debug view
	vec4 rt;           // x: ray traced lighting valid (sun shadows, ambient occlusion, planet light, light shadows), y: shadowed local lights
	vec4 planetDir;    // light of the planet nearby: direction to its centre (view space), w: sine of its angular radius
	vec4 planetLight;  // its irradiance at normal incidence (rgb), w: 1 = present
	vec4 lightCount;   // x: number of local lights; the flames the smoke's light is occluded towards (-1: none): y, z the
	                   // brightest vessel's (a point, the farthest of its points from it), w the next vessel's
	vec4 city[4];      // the cities' road map (CityField): centre (view space), w: present; axes x, y (view space); x, y, z: 1/size of its fine and coarse level and the far map [1/m]; w of [3]: the camera's altitude above the ground [m] (CityField; 0: none)
	vec4 relBeta;      // relativistic view: the camera's velocity (view space, units of c), w: 1 = on
	vec4 relSelf;      // the camera's own vessel (moving with it: seen as it is): centre (view space), w: radius
	vec4 relSunDir;    // the sunlight on that vessel, in its frame: from this direction (view space, aberrated)
	vec4 relSunColor;  //   with this colour (Doppler shifted)
	vec4 lens;         // a black hole's lens: its centre (view space), w: its mass GM/c^2 [m] (0: none)
	LocalLight lights[MAX_LIGHTS];
} frame;

// Relativistic view (the camera faster than 0.001 c): what the camera sees
// of a point at rest in the Solar System's frame at p (view space, relative
// to the camera) is the light it emitted, which the camera's motion
// aberrates towards the direction of motion and places at the distance D r
// (the emission event in the camera's frame; D: the Doppler factor). The
// camera's own vessel moves with it: seen as it is.
// A point behind a black hole (farther along its direction) is seen bent
// away from it: the point lens's primary image (weak field), at angle
// theta = (beta + sqrt (beta^2 + 4 thetaE^2)) / 2 from the hole, thetaE^2 =
// 4 M D_ls / (D_l D_s), at its distance (the sky through the hole's
// spacetime is traced, BlackHoleLens; objects are bent per vertex: their
// second image, inside the ring, left out)
vec3 LensImage (vec3 p)
{
	if (frame.lens.w <= 0.0) return p;
	vec3 h = frame.lens.xyz;
	float Dl = length (h), Ds = length (p);
	vec3 nh = h / Dl, np = p / Ds;
	float Dls = dot (p, nh) - Dl;
	if (Dls <= 0.0) return p;
	vec3 cr = cross (nh, np);
	float sb = length (cr), cb = dot (nh, np);
	if (sb < 1e-9) return p;
	float beta = atan (sb, cb);
	float tE2 = 4.0 * frame.lens.w * Dls / (Dl * Ds);
	float theta = 0.5 * (beta + sqrt (beta * beta + 4.0 * tE2));
	vec3 t = (np - nh * cb) / sb;
	return Ds * (nh * cos (theta) + t * sin (theta));
}

vec3 RelApparent (vec3 p)
{
	p = LensImage (p);
	if (frame.relBeta.w < 0.5 || distance (p, frame.relSelf.xyz) < frame.relSelf.w) return p;
	return RelApparentPos (p, frame.relBeta.xyz);
}
vec4 RelApparent (vec4 p) { return vec4 (RelApparent (p.xyz), p.w); }
// the sunlight on a surface at p (view space): on the camera's own vessel
// (moving with it), as it arrives in its frame: aberrated, Doppler shifted
bool RelOnSelf (vec3 p) { return frame.relBeta.w > 0.5 && distance (p, frame.relSelf.xyz) < frame.relSelf.w; }
vec3 SunDirAt (vec3 p) { return RelOnSelf (p) ? frame.relSunDir.xyz : frame.sunDir.xyz; }
vec3 SunColorAt (vec3 p) { return RelOnSelf (p) ? frame.relSunColor.rgb : frame.sunColor.rgb; }
// the direction (Solar System's frame) of what the camera sees in view direction d
vec3 RelTrueDir (vec3 d)
{
	return frame.relBeta.w < 0.5 ? d : RelTrueDirOf (d, frame.relBeta.xyz);
}

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
// the cities' road map: fine and coarse level (r: asphalt, g: pavement)
layout(set = FRAME_SET, binding = 9) uniform sampler2D cityMap[3]; // fine, coarse road map; far built-up map
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

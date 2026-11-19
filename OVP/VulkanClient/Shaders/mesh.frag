// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#version 450
#extension GL_GOOGLE_include_directive : require

#define FRAME_AO
#include "frame.glsl"

#include "shadow.glsl"
#include "lights.glsl"

#define ATM_SET 2
#include "atmosphere.glsl"

layout(set = 1, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 4) uniform samplerCube envProbe; // surroundings of the focus vessel (global frame)

layout(push_constant) uniform Draw {
	mat4 modelView;
	vec4 diffuse;
	vec4 emissive;
	vec4 specular;
	vec4 light;     // rgb: sunlight reaching the object (eclipse); a: how dark it is about it (night maps)
	vec4 pbr;       // metalness, roughness, reflectivity, dielectric F0 scale
	vec4 heat;      // xyz: direction of motion through the air (view), w: heating glow 0..1
	vec4 maps;      // x: flags of the additional texture maps present; w < 0: a base's
	                // ground (runways, pads; its fine detail), -1 - w > 0: a runway or pad group,
	                // its markings: main (> 0: instances' range)
} draw;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vPosObj;

layout(location = 0) out vec4 outColor;

#include "brdf.glsl"
#include "material.glsl"

#include "meshshade.glsl"

// ---------------------------------------------------------------------
// The fine detail of a base's ground (pavement: concrete, asphalt), below
// its textures' texels (some 5-10 cm): grain in octaves of 32, 8 and 2 cm in
// the base's frame (fixed on the ground), each faded out where the pixel
// covers it - a few percent of the albedo, and a relief of about a
// millimetre (bump from its screen derivatives)

float GHash (vec2 p) // (a cell's value, -1..1)
{
	p = fract (p * vec2 (0.1031, 0.1030));
	p += dot (p, p.yx + 33.33);
	return fract ((p.x + p.y) * p.x) * 2.0 - 1.0;
}

float GNoise (vec2 p)
{
	vec2 i = floor (p), f = fract (p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix (mix (GHash (i), GHash (i + vec2 (1.0, 0.0)), u.x),
	            mix (GHash (i + vec2 (0.0, 1.0)), GHash (i + vec2 (1.0, 1.0)), u.x), u.y);
}

// height [m] and albedo factor of the ground's detail at p (base frame,
// metres, the ground plane x-z); fp: the pixel's footprint [m]
float GroundDetail (vec2 p, float fp, out float alb)
{
	const float LAMBDA[3] = float[3] (0.32, 0.08, 0.02);
	const float AMP[3] = float[3] (0.0025, 0.0012, 0.0006);  // [m]
	const float ALB[3] = float[3] (0.10, 0.09, 0.07);
	float h = 0.0;
	alb = 1.0;
	for (int k = 0; k < 3; k++) {
		float w = 1.0 - smoothstep (0.25 * LAMBDA[k], 0.6 * LAMBDA[k], fp); // (resolved: faded out below the pixel)
		if (w <= 0.0) continue;
		vec2 q = p / LAMBDA[k] + vec2 (17.0 * float (k), 31.0 * float (k));
		float n = GNoise (q) * 0.7 + GNoise (q * 2.03 + 5.1) * 0.3;
		h += w * AMP[k] * n;
		alb *= 1.0 + w * ALB[k] * n;
	}
	return h;
}

#include "runway.glsl"
#include "pad.glsl"

void main()
{
	// a base's runway or pad group (draw.maps.w = -1 - (first + 128 count +
	// 16384 kind); kind 0 runways, 1 pads): the runway or pad and the slab the
	// pixel is on, where its pavement's maps are sampled
	int rw = draw.maps.w < 0.0 ? int (-draw.maps.w + 0.5) - 1 : 0;
	int gkind = rw / 16384, gfirst = (rw % 16384) % 128, gcount = (rw % 16384) / 128;
	vec3 dpx = dFdx (vPosObj), dpy = dFdy (vPosObj);
	RunwayHit hit;
	PadHit phit;
	bool onRw = false, onPad = false, lakebed = false; // (lakebed: marked out on bare ground, its lines only)
	vec2 suv = vUV, sdx = dFdx (vUV), sdy = dFdy (vUV);
	mat2 srot = mat2 (1.0);
	vec4 texel;
	if (rw > 0) { // (uniform)
		if (gkind == 0) {
			onRw = RunwayFind (vPosObj.xz, gfirst, gcount, hit);
			if (onRw) RunwaySlabUV (hit, dpx.xz, dpy.xz, suv, sdx, sdy, srot);
			lakebed = onRw && RunwayLakebed (hit);
		} else {
			onPad = PadFind (vPosObj.xz, gfirst, gcount, phit);
			if (onPad) PadSlabUV (phit, dpx.xz, dpy.xz, suv, sdx, sdy, srot);
		}
		texel = textureGrad (tex, suv, sdx, sdy);
	} else
		texel = texture(tex, vUV);
	if (draw.maps.z > 0.0 && texel.a < draw.maps.z) discard; // alpha-tested (foliage)
	vec3 base = texel.rgb * draw.diffuse.rgb;
	float alpha = texel.a * draw.diffuse.a;

	if (draw.emissive.a > 0.5) { // unlit
		outColor = vec4(base, alpha);
		return;
	}

	uint maps = uint(draw.maps.x + 0.5);
	// (a detail map: the grain of a surface whose texture is mapped once over all of it)
	if ((maps & MAP_DETAIL) != 0u && rw == 0) base *= 0.5 + texture(detailMap, DETAIL_UV(tex, vUV)).rgb;
	Material mat;
	if (rw > 0) mat = SurfaceMaterialS(vNormalView, vPosView, vUV, maps, draw.pbr, true, suv, sdx, sdy, srot);
	else mat = SurfaceMaterial(vNormalView, vPosView, vUV, maps, draw.pbr);
	if (draw.maps.w < 0.0) { // a base's ground: its fine detail (uniform: derivatives are defined)
		float fp = max (length (dpx), length (dpy));
		// a runway: its markings, the pavement's variation and grooves
		if (lakebed) {
			alpha = RunwayLakebedLines (vPosObj.xz, dpx.xz, dpy.xz, hit);
			base = RW_OIL, mat.rough = 0.9, mat.N = normalize (vNormalView);
		} else if (onRw) RunwayShade (vPosObj.xz, dpx.xz, dpy.xz, hit, base, mat.rough);
		if (onPad) PadShade (dpx.xz, dpy.xz, phit, base, mat.rough);
		float alb;
		float h = GroundDetail (vPosObj.xz, fp, alb);
		if (onRw && !lakebed) h += RunwayGrooves (hit, dpx.xz, dpy.xz);
		base *= alb;
		// bump (Mikkelsen: the surface gradient from the height's screen derivatives)
		vec3 sx = dFdx (vPosView), sy = dFdy (vPosView);
		vec3 N0 = mat.N;
		vec3 R1 = cross (sy, N0), R2 = cross (N0, sx);
		float det = dot (sx, R1);
		vec3 grad = sign (det) * (dFdx (h) * R1 + dFdy (h) * R2);
		vec3 Nb = normalize (abs (det) * N0 - grad);
		if (all (equal (Nb, Nb))) mat.N = Nb;
	}
	vec3 sun;
	bool debug;
	vec3 col = SurfaceLight(base, mat, sun, debug);
	if (debug) {
		outColor = vec4(col, 1.0);
		return;
	}
	vec3 N = mat.N;
	vec3 V = normalize(-vPosView);
	vec3 L = SunDirAt(vPosView);
	col += draw.emissive.rgb * texel.rgb;
	if ((maps & MAP_TRANSL) != 0u) // sunlight scattered through from behind
		col += texture(translMap, vUV).rgb * sun * max(dot(-N, L), 0.0);
	if ((maps & MAP_TRANSM) != 0u) { // sun seen through the surface
		vec4 tm = texture(transmMap, vUV);
		float spot = pow(max(dot(V, -L), 0.0), mix(2.0, 200.0, tm.a));
		col += tm.rgb * sun * spot;
	}
	// (emission; a night map, MAP_NIGHT: windows and lamps, lit as it gets dark - draw.light.w)
	if ((maps & MAP_EMIS) != 0u) col += texture(emisMap, vUV).rgb * ((maps & MAP_NIGHT) != 0u ? draw.light.w : 1.0);

	// aerodynamic heating: the hull's own heat, a dull red to orange where
	// the flow meets it head-on (the nose, the leading edges); the far
	// brighter glow of the gas around it, which also lights it: Plasma.cpp
	if (draw.heat.w > 0.0) {
		float f = max(dot(normalize(vNormalView), draw.heat.xyz), 0.0);
		float t = draw.heat.w;
		f *= f; f *= f;
		col += vec3(1.0, 0.18 + 0.30 * t, 0.04 + 0.10 * t) * (0.5 * t * t * f);
	}

	outColor = vec4(AerialPerspective(col), alpha);
}

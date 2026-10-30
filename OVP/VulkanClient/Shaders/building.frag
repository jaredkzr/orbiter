// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Buildings of the procedural cities (CityKit): walls, roofs and windows.
// The materials are layers of the kit's texture array, in metres of the
// building's own frame; windows are drawn here, a grid of bays and storeys
// fitted to each side (whole bays), so that they stay sharp close up and
// average out where they are finer than the pixels. Lit as mesh surfaces
// (meshshade.glsl).

#version 450
#extension GL_GOOGLE_include_directive : require

#define FRAME_AO
#include "frame.glsl"

#include "shadow.glsl"
#include "lights.glsl"

#define ATM_SET 2
#include "atmosphere.glsl"

layout(set = 1, binding = 0) uniform sampler2DArray kit; // per material: albedo (2i; a: cavity), normal and roughness (2i+1)
layout(set = 0, binding = 4) uniform samplerCube envProbe;

layout(push_constant) uniform Draw {
	mat4 modelView;
	vec4 diffuse;
	vec4 emissive;
	vec4 specular;
	vec4 light;     // rgb: sunlight reaching the object (eclipse)
	vec4 pbr;       // metalness, roughness, reflectivity, dielectric F0 scale
	vec4 heat;      // x: local (solar) time [h]
	vec4 maps;
} draw;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec3 vObj;
layout(location = 3) in vec3 vObjN;
layout(location = 4) flat in vec2 vShape;
layout(location = 5) flat in vec4 vDim;
layout(location = 6) flat in vec2 vPrm;
layout(location = 7) flat in uvec2 vBits;
layout(location = 8) flat in vec3 vUpView;
layout(location = 9) flat in float vFade;

layout(location = 0) out vec4 outColor;

#include "brdf.glsl"
#include "material.glsl"
#include "meshshade.glsl"

// parts (CityKit.h)
const int P_FACADE = 0, P_WALL = 1, P_ROOF_FLAT = 2, P_ROOF = 3, P_TRIM = 4, P_SOFFIT = 5;
// materials (CityKit.cpp): the size of their tiles [m]
const int M_ROOF_FLAT = 10;
const float TILE[12] = float[](4.0, 3.6, 3.6, 4.0, 4.0, 4.0, 4.0, 3.0, 3.0, 3.0, 4.0, 4.0);
// roof colours (factors on the material's own)
const vec3 ROOF_TINT[16] = vec3[](
	vec3(1.0), vec3(0.75, 0.42, 0.32), vec3(0.42, 0.43, 0.46), vec3(0.45, 0.68, 0.58),
	vec3(0.52, 0.58, 0.68), vec3(0.85, 0.55, 0.40), vec3(0.25, 0.25, 0.27), vec3(0.80, 0.80, 0.80),
	vec3(0.90, 0.82, 0.66), vec3(0.55, 0.25, 0.22), vec3(0.35, 0.55, 0.60), vec3(0.55, 0.45, 0.35),
	vec3(0.65, 0.65, 0.62), vec3(0.30, 0.38, 0.30), vec3(0.95, 0.95, 0.92), vec3(0.62, 0.52, 0.48));
// window styles (CityKit.h): bay width [m], window width and height (shares
// of the bay and storey), sill height (share of the storey), frame colour
const float WS_BAY[12]  = float[](3.4, 3.0, 3.0, 3.3, 3.0, 1.6, 1.5, 2.0, 6.0, 3.4, 2.8, 3.2);
const float WS_W[12]    = float[](0.34, 0.32, 0.45, 0.40, 0.50, 0.92, 0.94, 0.60, 0.80, 0.30, 0.55, 0.42);
const float WS_H[12]    = float[](0.45, 0.50, 0.46, 0.62, 0.42, 0.50, 0.80, 0.55, 0.18, 0.40, 0.50, 0.50);
const float WS_SILL[12] = float[](0.30, 0.28, 0.30, 0.12, 0.32, 0.30, 0.08, 0.25, 0.72, 0.36, 0.26, 0.28);
const vec3 WS_FRAME[12] = vec3[](
	vec3(0.85), vec3(0.85), vec3(0.6), vec3(0.35, 0.33, 0.30), vec3(0.7), vec3(0.25),
	vec3(0.22, 0.23, 0.25), vec3(0.5), vec3(0.4), vec3(0.6, 0.55, 0.45), vec3(0.65), vec3(0.3));
const int WS_BANDS = 5, WS_CURTAIN = 6, WS_GRID = 7, WS_INDUSTRIAL = 8;
// luminance of a lit window (the sun's illuminance on white: 1)
const float WIN_LUM = 0.25;

uint Hash (uint x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}
float Rnd (ivec2 c, uint seed, uint salt)
{
	return float(Hash(uint(c.x) ^ Hash(uint(c.y) ^ Hash(seed ^ salt)))) * (1.0 / 4294967296.0);
}

// share of lit windows by the local time t [h]: homes (evenings, mornings),
// offices (working hours), shops (opening hours, displays after), sheds
float Occupancy (int use, float t)
{
	float tt = t < 12.0 ? t + 24.0 : t;
	if (use == 1) return 0.12 + 0.63 * smoothstep(6.5, 8.5, t) * (1.0 - smoothstep(17.5, 21.0, t));
	if (use == 2) return mix(0.3, 0.95, smoothstep(6.5, 7.5, t) * (1.0 - smoothstep(20.5, 22.0, t)));
	if (use == 3) return 0.3;
	return 0.07 + 0.5 * smoothstep(16.5, 19.0, tt) * (1.0 - smoothstep(22.0, 25.5, tt))
		+ 0.25 * smoothstep(5.0, 6.5, t) * (1.0 - smoothstep(8.0, 9.0, t));
}

struct Surf {
	vec3 base;
	float rough, metal, refl;
	vec3 N;
	vec3 emis;
};

// Windows (and doors) on a facade: s along the side from its centre, y
// above the ground [m]
void Facade (inout Surf sf, vec3 Ng, vec3 nO, float s, float y, uint bits, uint seed)
{
	int storeys = int(bits & 255u), ws = int((bits >> 16) & 15u);
	float sh = vPrm.x;
	bool sideX = abs(nO.x) > abs(nO.z);
	float faceW = (sideX ? vDim.y : vDim.x) * vShape.y;
	float nb = max(1.0, floor(faceW / WS_BAY[ws] + 0.5));
	float bay = faceW / nb;
	vec2 g = vec2((s / faceW + 0.5) * nb, y / sh);
	ivec2 cell = ivec2(floor(g));
	vec2 f = g - vec2(cell);
	float px = max(length(fwidth(vObj)), 1e-4);
	// precast panels: joints between storeys and every second bay
	if ((bits >> 28) == 1u && y > 0.0) {
		float jx = min(fract(0.5 * g.x), 1.0 - fract(0.5 * g.x)) * 2.0 * bay, jy = min(f.y, 1.0 - f.y) * sh;
		float joint = 1.0 - smoothstep(0.015, 0.015 + px, min(jx, jy));
		sf.base *= 1.0 - 0.45 * joint * (1.0 - smoothstep(0.02, 0.06, px));
	}
	if (y <= 0.0 || y >= float(storeys) * sh) return;
	int face = sideX ? (nO.x > 0.0 ? 0 : 1) : (nO.z > 0.0 ? 2 : 3);
	uint fseed = seed * 4u + uint(face);

	float ww = WS_W[ws], wh = WS_H[ws], sill = WS_SILL[ws];
	vec3 frameCol = WS_FRAME[ws];
	bool shop = (bits & (1u << 24)) != 0u && cell.y == 0;
	bool door = false;
	if (shop) ww = 0.86, wh = 0.70, sill = 0.04, frameCol = vec3(0.2);
	// a house's door: on its front, in the bay nearest the middle
	bool front = (bits & (1u << 27)) != 0u ? face == 1 : face == 3;
	if ((bits & (1u << 25)) != 0u && front && cell.y == 0 && cell.x == int(floor(0.5 * nb))) {
		door = true;
		ww = min(1.0 / bay, 0.8), wh = min(2.1 / sh, 0.85), sill = 0.0;
	}
	// sheds: roller doors on the ground
	if (ws == WS_INDUSTRIAL && cell.y == 0 && Rnd(cell, fseed, 7u) < 0.35) {
		door = true;
		ww = 0.7, wh = min(4.5 / sh, 0.8), sill = 0.0;
	}

	// distance to the window's edge [m] (negative inside), the pixel's size
	float dx = (abs(f.x - 0.5) - 0.5 * ww) * bay;
	float dy = max(sill - f.y, f.y - (sill + wh)) * sh;
	float d = max(dx, dy);
	float fw = door ? 0.05 : (ws == WS_CURTAIN ? 0.05 : 0.08); // frame width [m]
	float opening = 1.0 - smoothstep(-0.5 * px, 0.5 * px, d);
	float glass = 1.0 - smoothstep(-fw - 0.5 * px, -fw + 0.5 * px, d);
	// where the grid is finer than the pixels: its average
	float far = smoothstep(0.25, 0.7, max(length(fwidth(g.x)), length(fwidth(g.y))));
	if (far > 0.0) {
		float ga = max(ww * bay - 2.0 * fw, 0.0) / bay * max(wh * sh - 2.0 * fw, 0.0) / sh;
		float oa = ww * wh;
		opening = mix(opening, oa, far);
		glass = mix(glass, ga, far);
	}
	float frm = max(opening - glass, 0.0);

	// behind the glass: rooms, some curtained, some blinds
	float r = Rnd(cell, fseed, 1u);
	vec3 room = vec3(0.025, 0.024, 0.022) * (0.6 + 0.8 * Rnd(cell, fseed, 2u));
	if (r < 0.18) room = vec3(0.30, 0.26, 0.20);      // curtains
	else if (r < 0.30) room = vec3(0.38, 0.38, 0.36); // blinds
	vec3 glassBase = room;
	float glassMetal = 0.0;
	if (ws == WS_CURTAIN || (bits & (1u << 26)) != 0u) { // coated glass: tinted mirror
		vec3 tint = (bits & (1u << 26)) != 0u ? vec3(0.30, 0.42, 0.45) : vec3(0.32, 0.34, 0.36);
		glassBase = mix(tint, room, 0.15);
		glassMetal = 0.6;
	}
	if (door) {
		vec3 dc = ws == WS_INDUSTRIAL ? vec3(0.45, 0.46, 0.47)
			: mix(vec3(0.25, 0.12, 0.06), vec3(0.6, 0.6, 0.58), Rnd(cell, fseed, 3u));
		sf.base = mix(sf.base, dc, opening);
		sf.rough = mix(sf.rough, 0.6, opening);
		sf.N = normalize(mix(sf.N, Ng, opening));
		return;
	}
	// the lintel's shadow in the top of the opening
	float top = smoothstep(0.0, 0.25, (sill + wh - f.y) * sh);
	glassBase *= mix(1.0, 0.55 + 0.45 * top, 1.0 - far);

	// at night: rooms lit by the local time, the building's own habits
	float dark = smoothstep(0.06, -0.06, dot(vUpView, frame.sunDir.xyz));
	if (dark > 0.0) {
		int use = shop ? 2 : ws == WS_INDUSTRIAL ? 3 : (ws == WS_BANDS || ws == WS_CURTAIN || ws == WS_GRID) ? 1 : 0;
		float occ = clamp(Occupancy(use, draw.heat.x) * (0.6 + 0.8 * Rnd(ivec2(0), seed, 13u)), 0.0, 1.0);
		// (far: not the occupancy as an even glow, which looked like a grey
		// haze over the skylines, but blocks of windows about a pixel in size,
		// each lit or not at that rate: the lights' sparkle; the blocks' sizes
		// blended between powers of two, anchored to the building)
		float litFar = 0.0;
		if (far > 0.0) {
			float L = log2(max(max(length(fwidth(g.x)), length(fwidth(g.y))), 1.0));
			int l0 = min(int(L), 8);
			float b0 = Rnd(cell >> l0, fseed, 11u + uint(l0)) < occ ? 1.0 : 0.0;
			float b1 = Rnd(cell >> (l0 + 1), fseed, 12u + uint(l0)) < occ ? 1.0 : 0.0;
			litFar = mix(b0, b1, fract(L));
		}
		float lit = mix(Rnd(cell, fseed, 11u) < occ ? 1.0 : 0.0, litFar, far);
		float c = Rnd(cell, fseed, 12u);
		vec3 lc = use == 1 ? (c < 0.8 ? vec3(0.85, 0.9, 1.0) : vec3(1.0, 0.8, 0.55))
			: use == 2 ? vec3(1.0, 0.88, 0.7)
			: c < 0.68 ? vec3(1.0, 0.66, 0.36) : c < 0.95 ? vec3(0.88, 0.88, 0.92) : vec3(0.55, 0.65, 1.0);
		float lum = WIN_LUM * (use == 2 ? 2.0 : 1.0) * (r < 0.18 ? 0.6 : 1.0) * (0.6 + 0.8 * Rnd(cell, fseed, 14u));
		lc = mix(lc, vec3(0.95, 0.8, 0.62), far); // (the average)
		sf.emis += lc * (lum * lit * glass * dark);
	}
	sf.base = mix(sf.base, frameCol * frameCol, frm);
	sf.base = mix(sf.base, glassBase, glass);
	sf.rough = mix(mix(sf.rough, 0.45, frm), 0.04, glass);
	sf.metal = mix(sf.metal, glassMetal, glass);
	sf.refl = mix(sf.refl, 1.0, glass);
	sf.N = normalize(mix(sf.N, Ng, opening));
}

void main()
{
	// fading out at the end of its range: dithered (interleaved gradient noise)
	if (vFade < 1.0 && fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) >= vFade) discard;
	uint bits = vBits.x;
	uint seed = vBits.y >> 16;
	uint t = vBits.y & 0xffffu;
	vec3 wallTint = vec3(float((t >> 11) & 31u) / 31.0, float((t >> 5) & 63u) / 63.0, float(t & 31u) / 31.0);
	int part = int(vShape.x + 0.5);
	int wallM = int((bits >> 8) & 15u), roofM = int((bits >> 12) & 15u);
	vec3 roofTint = ROOF_TINT[(bits >> 20) & 15u];
	float sunk = vPrm.y;

	// the geometric normal: given (walls, flat parts) or the surface's
	vec3 nO = vObjN;
	vec3 Ng = vNormalView;
	if (dot(nO, nO) < 0.25) {
		nO = normalize(cross(dFdx(vObj), dFdy(vObj)));
		if (nO.y < 0.0) nO = -nO;
		Ng = cross(dFdx(vPosView), dFdy(vPosView));
	}
	Ng = normalize(Ng);
	if (dot(Ng, vPosView) > 0.0) Ng = -Ng;

	// the material, in metres of the building frame
	vec2 uv;
	int m;
	vec3 tint;
	float s = 0.0;
	if (part == P_ROOF_FLAT) {
		uv = vObj.xz, m = M_ROOF_FLAT, tint = vec3(1.0);
	} else if (part == P_ROOF) { // along the eaves and down the slope
		vec2 dn = nO.xz;
		float l = length(dn);
		if (l < 0.05) uv = vObj.xz;
		else {
			dn /= l;
			uv = vec2(dot(vObj.xz, vec2(-dn.y, dn.x)), dot(vObj.xz, dn) / max(nO.y, 0.15));
		}
		m = roofM, tint = roofTint;
	} else { // walls: along the side, up from the ground
		s = abs(nO.x) > abs(nO.z) ? vObj.z * sign(nO.x) : -vObj.x * sign(nO.z);
		uv = vec2(s, vObj.y - sunk), m = wallM, tint = wallTint;
	}
	vec2 tuv = uv / TILE[m];
	vec4 alb = texture(kit, vec3(tuv, float(2 * m)));
	vec4 nr = texture(kit, vec3(tuv, float(2 * m + 1)));
	Surf sf;
	sf.base = pow(alb.rgb, vec3(2.2)) * tint;
	sf.rough = nr.b;
	sf.metal = 0.0;
	sf.refl = 0.0;
	sf.emis = vec3(0.0);
	vec3 nt;
	nt.xy = nr.xy * 2.0 - 1.0;
	nt.z = sqrt(max(0.0, 1.0 - dot(nt.xy, nt.xy)));
	sf.N = normalize(CotangentFrame(Ng, vPosView, tuv) * nt);
	if (!all(equal(sf.N, sf.N))) sf.N = Ng;

	if (part == P_TRIM) sf.base = mix(sf.base, vec3(0.55, 0.54, 0.52), 0.7);
	else if (part == P_SOFFIT) sf.base *= 0.45;
	else if (part == P_FACADE) Facade(sf, Ng, nO, s, vObj.y - sunk, bits, seed);

	Material mat;
	mat.N = sf.N;
	mat.metal = sf.metal;
	mat.rough = clamp(sf.rough, 0.04, 1.0);
	mat.reflectivity = sf.refl;
	mat.specTint = vec3(1.0);
	mat.reflTint = vec3(1.0);
	vec3 sun;
	bool debug;
	// (on the night side, as the terrain: no skylight, only moon and stars)
	ambientShare = mix(0.15, 1.0, smoothstep(-0.25, 0.0, dot(vUpView, frame.sunDir.xyz)));
	vec3 col = SurfaceLight(sf.base, mat, sun, debug);
	if (debug) {
		outColor = vec4(col, 1.0);
		return;
	}
	col += sf.emis;
	outColor = vec4(AerialPerspective(col), 1.0);
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planet surface tiles (fragment shader body: tile.frag, and tile_fp64.frag
// with the procedural terrain detail, which needs double precision)

#define FRAME_AO
#include "frame.glsl"

#include "shadow.glsl"
#include "lights.glsl"

layout(set = 1, binding = 0) uniform sampler2D surfTex;
layout(set = 2, binding = 0) uniform sampler2D maskTex;

layout(push_constant) uniform Tile {
	mat4 modelView;
	vec4 surfUV;
	vec4 maskUV;
	vec4 params;
	vec4 detail;
} tile;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;
layout(location = 3) in float vDetailA;

layout(location = 0) out vec4 outColor;

#define ATM_SET 3
#include "atmosphere.glsl"
#include "brdf.glsl"
#ifdef TERRAIN_DETAIL
#include "terraindetail.glsl"
#endif

// Periodic value noise (period in cells), for patterns anchored to the
// 1 km-snapped origin (cell sizes divide 1 km)
float Hash (ivec3 c)
{
	uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u;
	h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
	return float(h & 0xffffu) / 65535.0;
}

float ValueNoise (vec3 p, float cell, int period)
{
	vec3 q = p / cell;
	ivec3 i = ivec3(floor(q));
	vec3 f = fract(q);
	f = f * f * (3.0 - 2.0 * f);
	float n[8];
	for (int k = 0; k < 8; k++) {
		// % is undefined for negative operands in GLSL: wrap explicitly
		ivec3 c = i + ivec3(k & 1, (k >> 1) & 1, k >> 2);
		c = c - period * ivec3(floor(vec3(c) / float(period)));
		n[k] = Hash(c);
	}
	return mix(mix(mix(n[0], n[1], f.x), mix(n[2], n[3], f.x), f.y),
	           mix(mix(n[4], n[5], f.x), mix(n[6], n[7], f.x), f.y), f.z);
}

#include "surfdetail.glsl"

// Wave normal of water surfaces (view space). The waves run along integer
// directions of the planet frame with wavelengths that divide 1 km, so the
// pattern is continuous when its origin (snapped to 1 km) moves.
vec3 WaterNormal (vec3 Nv, vec3 posView, float dist)
{
	if (atm.water.w < 0.0) return Nv;
	float fade = 1.0 - smoothstep (300.0, 3000.0, dist);
	if (fade <= 0.0) return Nv;
	mat3 v2p = mat3 (atm.viewToPlanet);
	vec3 p = v2p * posView + atm.water.xyz; // relative to the pattern origin
	vec3 Np = v2p * Nv;
	// a spectrum of waves in irregular directions (golden angle steps about
	// the local vertical), wavelengths from 60 m down to 0.6 m, slopes of a
	// light breeze; waves shorter than a few pixels fade out (no aliasing)
	vec3 t1 = normalize (cross (Np, abs (Np.y) < 0.9 ? vec3 (0.0, 1.0, 0.0) : vec3 (1.0, 0.0, 0.0)));
	vec3 t2 = cross (Np, t1);
	// pixel size on the water [m] (stretched along the view at grazing angles)
	float footprint = dist * 2.0 * atm.misc.z / max (frame.jitter.w, 1.0)
		/ max (abs (dot (normalize (posView), Nv)), 0.05);
	vec3 g = vec3 (0.0);
	const int NW = 14;
	for (int i = 0; i < NW; i++) {
		float lambda = 60.0 * pow (0.7, float (i));
		float ang = float (i) * 2.39996 + 0.3 * sin (float (i) * 7.1);
		// (mostly with the wind, from about one direction)
		ang = 0.6 * sin (ang) + 0.2;
		vec3 dir = t1 * cos (ang) + t2 * sin (ang);
		float k = 2.0 * PI / lambda;
		float w = sqrt (9.81 * k);
		float ph = k * dot (p, dir) + w * atm.water.w + float (i) * 1.7;
		float aa = 1.0 - smoothstep (2.0, 6.0, footprint / lambda * 4.0);
		g += dir * (0.035 * cos (ph) * aa);
	}
	g -= Np * dot (g, Np); // slope in the tangent plane
	return transpose (v2p) * normalize (Np - g * fade);
}

void main()
{
	vec3 N = normalize(vNormalView);
	vec3 Ng = N; // (geometric: water, night side)
	vec3 L = frame.sunDir.xyz;
	// clamp to the texel centres at the tile edges (the sampler wraps around)
	vec2 hs = 0.5 / vec2(textureSize(surfTex, 0));
	vec3 base = texture(surfTex, clamp(vUV * tile.surfUV.zw + tile.surfUV.xy, hs, 1.0 - hs)).rgb;
	// procedural detail below the textures' resolution (terrain only)
	if (tile.params.z < 0.5) {
#ifdef TERRAIN_DETAIL
		TerrainDetailShading(vPosView, N, base, vDetailA);
#endif
		SurfaceDetail(vPosView, length(vPosView), N, base);
	} else if (dot(vPosView, N) < 0.0) {
		// cloud tops seen from above: relief from the cloud density (thicker
		// clouds are taller; exaggerated for the texture's smoothness), shading their sides (surface
		// gradient bump mapping from the screen-space derivatives)
		float h = 5000.0 * texture(surfTex, clamp(vUV * tile.surfUV.zw + tile.surfUV.xy, hs, 1.0 - hs)).a;
		vec3 sx = dFdx(vPosView), sy = dFdy(vPosView);
		vec3 r1 = cross(sy, N), r2 = cross(N, sx);
		float det = dot(sx, r1);
		vec3 grad = sign(det) * (dFdx(h) * r1 + dFdy(h) * r2);
		if (abs(det) > 1e-9) N = normalize(abs(det) * N - grad);
	}
	float ndl = dot(N, L);
	bool hasAtm = atm.misc.x > 0.5;
	vec3 sunT = vec3(1.0);
	vec3 pPlanet = vec3(0.0), dPlanet = vec3(0.0);
	if (hasAtm) {
		pPlanet = mat3(atm.viewToPlanet) * vPosView + atm.camPos.xyz;
		dPlanet = normalize(mat3(atm.viewToPlanet) * vPosView);
		sunT = SunTransmittance(pPlanet);
		if (tile.params.z < 0.5) sunT *= CloudShadow(pPlanet); // terrain under the clouds
	}
	// terrain receives shadows: ray traced, or from the shadow maps
	bool rtVis = RTLighting() && tile.params.z < 0.5;
	vec4 lv = rtVis ? RTLightVisibility() : vec4(1.0);
	if (tile.params.z < 0.5) sunT *= rtVis ? RTVisibility().x : SunShadow(vPosView, N);
	float ao = tile.params.z < 0.5 ? AmbientOcclusion() : 1.0; // clouds are not in the prepass
	float aoDirect = mix(1.0, ao, frame.aoParams.z); // contact darkening in direct light too
	// the night side gets no skylight: only a fraction of the ambient level
	// remains (moon and starlight)
	float ambNight = tile.params.z < 0.5 ? mix(0.15, 1.0, smoothstep(-0.25, 0.0, dot(Ng, L))) : 1.0;
	// regolith of airless bodies: no limb darkening, opposition surge
	float sunRefl = int(atm.detail.x + 0.5) == DETAIL_AIRLESS && tile.params.z < 0.5
		? RegolithReflectance(N, L, normalize(-vPosView), Ng) : max(ndl, 0.0);
	vec3 col = base * (frame.ambient.rgb * ao * ambNight + (frame.sunColor.rgb * sunT * sunRefl + LocalLighting(vPosView, N, lv)) * aoDirect);

	if (tile.params.z > 0.5) { // cloud layer: lit from above, also seen from below
		float a = texture(surfTex, clamp(vUV * tile.surfUV.zw + tile.surfUV.xy, hs, 1.0 - hs)).a;
		// near the camera the volumetric clouds replace the layer
		if (tile.params.w > 0.0) a *= smoothstep(0.75 * tile.params.w, tile.params.w, length(vPosView));
		// seen from below: the underside, shaded by the cloud's own thickness
		if (dot(vPosView, normalize(vNormalView)) > 0.0) col *= mix(1.0, 0.35, a);
		if (hasAtm) {
			vec3 T;
			vec3 L = Inscatter(dPlanet, length(vPosView), T);
			col = col * T + AtmTonemap(L);
		}
		outColor = vec4(col, a);
		return;
	}

	if (tile.params.x > 0.5) {
		vec2 hm = 0.5 / vec2(textureSize(maskTex, 0));
		vec4 m = texture(maskTex, clamp(vUV * tile.maskUV.zw + tile.maskUV.xy, hm, 1.0 - hm));
		// night lights fade in beyond the terminator
		float night = clamp(0.2 - dot(Ng, L) * 4.0, 0.0, 1.0);
		// the night light texture represents lights seen from altitude; close up
		// it would flood the ground, so it fades out near the camera
		night *= smoothstep(1000.0, 10000.0, length(vPosView));
		// (the texture's lights are blurred averages of point sources: cores
		// brightened, dim spill suppressed, so that cities stand out as in
		// photographs)
		float lm = max (m.r, max (m.g, m.b));
		col += m.rgb * (3.0 * lm * smoothstep (0.04, 0.25, lm)) * night * tile.params.y;
		// water (mask alpha 0): reflection of the sky and the sun's glitter,
		// which spreads out when seen from afar
		if (m.a < 0.5) {
			float dist = length(vPosView);
			vec3 V = normalize(-vPosView);
			vec3 Nw = WaterNormal(Ng, vPosView, dist);
			float NoV = max(dot(Nw, V), 1e-3);
			float fres = 0.02 + 0.98 * pow(1.0 - NoV, 5.0);
			if (hasAtm) {
				vec3 R = reflect(-V, Nw);
				// (reflections towards the water itself would see it again: kept
				// above the horizon)
				float rh = dot(R, N);
				if (rh < 0.02) R = normalize(R + N * (0.02 - rh));
				vec3 T;
				vec3 sky = AtmTonemap(Inscatter(normalize(mat3(atm.viewToPlanet) * R), ATM_NONE, T));
				// screen-space reflections (traced with the smooth surface),
				// displaced by the waves
				vec3 dN = Nw - N;
				vec4 s = ScreenReflection(vec2(dN.x, -dN.y) * 0.03);
				sky = sky * (1.0 - s.a) + s.rgb;
				if (frame.probe.w > 0.5) { // debug view
					outColor = vec4(s.rgb + vec3(0.0, 0.0, 0.3) * (1.0 - s.a), 1.0);
					return;
				}
				col = mix(col, sky, fres);
			}
			float rough = mix(0.05, 0.3, smoothstep(2e3, 3e5, dist));
			col += frame.sunColor.rgb * sunT * SpecularBRDF(Nw, V, L, vec3(0.02), rough) * PI;
		}
	}
	if (hasAtm) { // aerial perspective
		vec3 T;
		vec3 L = Inscatter(dPlanet, length(vPosView), T);
		col = col * T + AtmTonemap(L);
	}
	outColor = vec4(col, 1.0);
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planet surface tiles (fragment shader body: tile.frag, and tile_fp64.frag
// with the procedural terrain detail)

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
	vec4 morph;
	vec4 vessel;
	ivec4 refI;    // terrain detail: the tile's reference point d0 R [m], integer part (xyz)
	vec4 refF;     // ... fraction (xyz); w: the planet's radius R [m]
	vec4 refRel;   // d0 (R + h0) relative to the camera, planet frame (xyz); w: R + h0 [m]
	vec4 tracks;   // ground tracks on the tile: first entry, count
} tile;

// ground tracks (GroundTracks.h), view space
struct TrackSeg {
	vec4 a;  // start; w: half width [m]
	vec4 b;  // end; w: depth [m]
	vec4 up; // surface normal; w: 0 track, 1 imprint
};
layout(std430, set = 0, binding = 8) readonly buffer Tracks { TrackSeg seg[]; } tracks;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec2 vDetailA;
layout(location = 4) in float vMorph;

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

// ValueNoise (x) and its gradient (yzw, per metre)
vec4 ValueNoiseD(vec3 p, float cell, int period)
{
	vec3 q = p / cell;
	ivec3 i = ivec3(floor(q));
	vec3 f = fract(q);
	vec3 u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
	float n[8];
	for (int k = 0; k < 8; k++) {
		ivec3 c = i + ivec3(k & 1, (k >> 1) & 1, k >> 2);
		c = c - period * ivec3(floor(vec3(c) / float(period)));
		n[k] = Hash(c);
	}
	float k1 = n[1] - n[0], k2 = n[2] - n[0], k3 = n[4] - n[0];
	float k4 = n[0] - n[1] - n[2] + n[3], k5 = n[0] - n[2] - n[4] + n[6], k6 = n[0] - n[1] - n[4] + n[5];
	float k7 = -n[0] + n[1] + n[2] - n[3] + n[4] - n[5] - n[6] + n[7];
	float v = n[0] + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x + k7 * u.x * u.y * u.z;
	vec3 g = du * vec3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
	                   k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
	                   k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);
	return vec4(v, g / cell);
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

// Tracks of wheels and imprints of gear: a groove with raised edges (the
// nearest segment), the tyres' tread across it, the disturbed soil a
// little darker
void TrackShading(vec3 P, inout vec3 N, inout vec3 base)
{
	int first = int(tile.tracks.x), count = int(tile.tracks.y);
	if (count <= 0) return;
	float best = 1.8;
	vec3 off = vec3(0.0), dir = vec3(0.0);
	float along = 0.0;
	TrackSeg s;
	// scorch marks overlap: blended (the darkest burn, the craters' slopes
	// added), not the nearest alone (seams where one meets the next)
	float burnMax = 0.0;
	vec3 scorchGrad = vec3(0.0);
	for (int i = 0; i < count; i++) {
		TrackSeg c = tracks.seg[first + i];
		vec3 up = c.up.xyz;
		vec3 ab = c.b.xyz - c.a.xyz, ap = P - c.a.xyz;
		ab -= up * dot(ab, up), ap -= up * dot(ap, up);
		float L2 = dot(ab, ab);
		float t = L2 > 1e-8 ? clamp(dot(ap, ab) / L2, 0.0, 1.0) : 0.0;
		vec3 o = ap - ab * t;
		float x = length(o) / c.a.w;
		if (c.up.w > 1.5) {
			if (x >= 1.8) continue;
			// scorch mark: blackened ground, most at its centre, its rim
			// irregular (a few lobes, by the direction; phases from its size)
			// and soft, a shallow crater with a low rim
			float hw = c.a.w, depth = c.b.w;
			vec3 lat = x > 1e-4 ? o / (x * hw) : vec3(0.0);
			vec3 ref = normalize(abs(up.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0));
			float ang = atan(dot(lat, cross(up, ref)), dot(lat, ref));
			float rag = 0.12 * sin(ang * 2.0 + 1.7 * hw) + 0.08 * sin(ang * 3.0 + 2.9 * hw) + 0.05 * sin(ang * 5.0 + 4.3 * hw)
				+ 0.025 * sin(ang * 9.0 + 6.1 * hw);
			burnMax = max(burnMax, 1.0 - smoothstep(0.2 + rag, 1.0 + rag, x));
			float qc = max(1.0 - x * x, 0.0);
			float ec = exp(-pow((x - 1.05) / 0.25, 2.0));
			float dh = 4.0 * x * qc * 0.5 - 0.3 * ec * 2.0 * (x - 1.05) / 0.0625;
			vec3 g = lat * (depth * dh / hw);
			scorchGrad += g - up * dot(g, up);
			continue;
		}
		if (x < best) {
			best = x, off = o, s = c;
			dir = L2 > 1e-8 ? ab / sqrt(L2) : vec3(0.0);
			along = L2 > 1e-8 ? dot(ap, dir) : 0.0;
		}
	}
	if (burnMax > 0.0 || dot(scorchGrad, scorchGrad) > 0.0) {
		base *= mix(1.0, 0.1, burnMax * (0.7 + 0.3 * burnMax));
		vec3 g = scorchGrad - N * dot(scorchGrad, N);
		N = normalize(N - g);
	}
	if (best >= 1.8) return;
	float x = best, hw = s.a.w, depth = s.b.w;
	vec3 lat = x > 1e-4 ? off / (x * hw) : vec3(0.0);
	// profile [depths]: groove -(1 - x^2)^2, edges pushed up beside it
	float q = max(1.0 - x * x, 0.0);
	float e = exp(-pow((x - 1.2) / 0.3, 2.0));
	float dhdx = 4.0 * x * q - 0.35 * e * 2.0 * (x - 1.2) / 0.09;
	vec3 grad = lat * (depth * dhdx / hw);
	// tread across the track (faded out when finer than the pixels)
	float px = length(P) * 2.0 * atm.misc.z / max(frame.jitter.w, 1.0);
	if (s.up.w < 0.5 && x < 1.0) {
		const float PERIOD = 0.08;
		float w = (1.0 - smoothstep(0.004, 0.012, px)) * smoothstep(1.0, 0.7, x);
		grad += dir * (0.25 * depth * w * 6.2832 / PERIOD * cos(along * 6.2832 / PERIOD));
	}
	vec3 up = s.up.xyz;
	grad -= up * dot(grad, up);
	grad -= N * dot(grad, N);
	N = normalize(N - grad);
	base *= 1.0 - 0.15 * q; // (disturbed soil, darker)
}

// Streets of the cities around the camera (CityField): the road map over
// the tangent plane at its centre, a fine level within a coarse one
// irradiance under a street lamp (the sun's: 1)
const float CITY_LAMP_IRR = 0.3;

// The cities' streets on the ground (CityField's road map: r asphalt, g
// pavement, b light of the street lamps, a its share of white LED light);
// returns the lamps' light (irradiance, for the night); cover: where the
// map lies (1; fading out at its edges), where the cities' own lights
// replace the night light texture
vec3 cityEmis = vec3(0.0); // (CityGround: the roof carpet's lit windows)
float cityCarpet = 0.0;     // (CityGround: the roof carpet's share)
vec3 CityGround (vec3 posView, inout vec3 base, out float cover)
{
	cover = 0.0;
	if (frame.city[0].w < 0.5) return vec3(0.0);
	vec3 d = posView - frame.city[0].xyz;
	vec2 p = vec2(dot(d, frame.city[1].xyz), dot(d, frame.city[2].xyz));
	// Beyond the small buildings (building.vert's fade, 1.3 to 2.3 km): a
	// carpet of roofs where the ground is built up (the far map: the streets'
	// share about), in the blocks between the streets (the road map, where
	// it reaches), so that the city drawn in three dimensions continues
	// flat; lit windows at night (cityEmis)
	vec2 ud = p * frame.city[3].z + 0.5;
	float ed = max(abs(ud.x - 0.5), abs(ud.y - 0.5));
	float wc = ed < 0.5 ? smoothstep(1800.0, 2600.0, length(posView)) * smoothstep(0.5, 0.45, ed) : 0.0;
	float built = wc > 0.0 ? smoothstep(0.10, 0.30, texture(cityMap[2], ud).r) : 0.0;
	cityCarpet = wc * built;
	vec2 uc = p * frame.city[3].y + 0.5;
	float ec = max(abs(uc.x - 0.5), abs(uc.y - 0.5));
	vec4 m = vec4(0.0);
	float fade = 0.0;
	if (ec < 0.5) {
		m = texture(cityMap[1], uc);
		vec2 uf = p * frame.city[3].x + 0.5;
		float ef = max(abs(uf.x - 0.5), abs(uf.y - 0.5));
		if (ef < 0.5) m = mix(texture(cityMap[0], uf), m, smoothstep(0.4, 0.5, ef));
		fade = smoothstep(0.5, 0.4, ec);
		cover = fade;
	}
	float w = wc * built * (1.0 - max(m.r, m.g) * fade);
	if (w > 0.0) {
		vec2 q = p / 14.0;
		ivec2 c = ivec2(floor(q));
		vec2 f = fract(q);
		float h = Hash(ivec3(c, 7));
		vec3 roof = h < 0.35 ? vec3(0.40, 0.39, 0.38) : h < 0.55 ? vec3(0.27, 0.27, 0.29)
			: h < 0.72 ? vec3(0.50, 0.30, 0.22) : h < 0.88 ? vec3(0.36, 0.40, 0.44) : vec3(0.17, 0.25, 0.14);
		roof *= 0.85 + 0.3 * Hash(ivec3(c, 8));
		roof = mix(roof, vec3(0.37, 0.35, 0.33), 0.65 * smoothstep(1800.0, 6000.0, length(posView))); // (far: a town's mean, not a checkerboard)
		float gap = 1.0 - smoothstep(0.0, 0.12, min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y))); // (the buildings' sides, in shade)
		roof *= 1.0 - 0.45 * gap * (1.0 - smoothstep(1000.0, 4000.0, length(posView))); // (far: they average out)
		base = mix(base, roof, 0.85 * w);
		float lit = Hash(ivec3(c, 9)) < 0.35 ? 1.0 : 0.0;
		cityEmis = vec3(1.0, 0.78, 0.5) * (0.04 * lit * w);
	}
	if (fade <= 0.0) return vec3(0.0);
	// (from afar the streets of the coarse road map, metres wider than they
	// are, drew a black grid over the town: their contrast fades with
	// distance, as the narrow streets and their trees average out)
	float far = smoothstep(600.0, 3000.0, length(posView));
	base = mix(base, vec3(0.20, 0.19, 0.18), m.g * fade * (1.0 - 0.5 * far)); // pavement
	base = mix(base, mix(vec3(0.075, 0.075, 0.08), vec3(0.17, 0.165, 0.16), far), m.r * fade * (1.0 - 0.7 * far)); // asphalt
	// (sodium lamps orange, LEDs white)
	vec3 lamp = mix(vec3(1.0, 0.55, 0.18), vec3(0.85, 0.9, 1.0), m.a);
	return lamp * (CITY_LAMP_IRR * m.b * fade);
}

// Texture gradients towards the poles: the textures' texels (equal steps
// of longitude and latitude) narrow to slivers, cos(lat) as wide as they are
// long; the columns' own noise showed as streaks along the meridians. The
// footprint is widened east-west to at least one texel's length (in metres)
// north-south, which the anisotropic filter averages (at once, the mip levels
// further on). cos(lat): the ratio of the surface's change along u (longitude)
// to v (latitude), from the screen derivatives (a tile spans equal angles).
vec2 polarGx, polarGy;
bool polarDone = false;
void PolarGradients(vec2 uvT, vec2 hs)
{
	vec2 gx = dFdx(uvT), gy = dFdy(uvT);
	polarGx = gx, polarGy = gy, polarDone = true;
	vec3 px = dFdx(vPosView), py = dFdy(vPosView);
	vec2 ux = dFdx(vUV), uy = dFdy(vUV);
	float det = ux.x * uy.y - uy.x * ux.y;
	if (abs(det) < 1e-20) return;
	vec3 dPdu = (px * uy.y - py * ux.y) / det, dPdv = (py * ux.x - px * uy.x) / det;
	float lv = length(dPdv);
	if (lv <= 0.0) return;
	float c = clamp(length(dPdu) / lv, 1e-3, 1.0);
	float w = smoothstep(0.5, 0.25, c); // (from 60 degrees on: none at lower latitudes)
	if (w <= 0.0) return;
	float su = max(abs(gx.x), abs(gy.x)), sv = max(max(abs(gx.y), abs(gy.y)), 2.0 * hs.y);
	polarGx = mix(gx, vec2(max(su, sv / c), 0.0), w);
	polarGy = mix(gy, vec2(0.0, sv), w);
}
vec2 PolarGradX(vec2 uvT, vec2 hs) { if (!polarDone) PolarGradients(uvT, hs); return polarGx; }
vec2 PolarGradY(vec2 uvT, vec2 hs) { if (!polarDone) PolarGradients(uvT, hs); return polarGy; }

void main()
{
	vec3 N = normalize(vNormalView);
	vec3 Ng = N; // (geometric: water, night side)
	vec3 L = frame.sunDir.xyz;
	// clamp to the texel centres at the tile edges (the tiles' sampler clamps
	// there too: the footprint widened towards the poles, about a hundred
	// texels east-west, wrapped round to the tile's far edge and showed as a
	// band along every tile edge)
	vec2 hs = 0.5 / vec2(textureSize(surfTex, 0));
	vec2 uvT = clamp(vUV * tile.surfUV.zw + tile.surfUV.xy, hs, 1.0 - hs);
	vec3 base = textureGrad(surfTex, uvT, PolarGradX(uvT, hs), PolarGradY(uvT, hs)).rgb;
	if (tile.params.z < 0.5 && atm.surfPrm.x != 1.0) // (the textures' colour tints, per planet)
		base = max(mix(vec3(dot(base, vec3(0.2126, 0.7152, 0.0722))), base, atm.surfPrm.x), 0.0);
	// procedural detail below the textures' resolution (terrain only)
	float detailShadow = 1.0; // (shadows of the procedural craters)
	vec3 cityLight = vec3(0.0); // (street lamps)
	float cityCover = 0.0;
	if (tile.params.z < 0.5) {
#ifdef TERRAIN_DETAIL
		TerrainDetailShading(vPosView, N, base, vDetailA, detailShadow);
#endif
		cityLight = CityGround(vPosView, base, cityCover);
		TrackShading(vPosView, N, base);
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
	vec3 sunT = vec3(tile.morph.z); // (eclipses)
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
	if (tile.params.z < 0.5) sunT *= (rtVis ? RTVisibility().x : SunShadow(vPosView, N)) * detailShadow;
	float ao = tile.params.z < 0.5 ? AmbientOcclusion() : 1.0; // clouds are not in the prepass
	float aoDirect = mix(1.0, ao, frame.aoParams.z); // contact darkening in direct light too
	// the night side gets no skylight: only a fraction of the ambient level
	// remains (moon and starlight)
	float ambNight = tile.params.z < 0.5 ? mix(0.15, 1.0, smoothstep(-0.25, 0.0, dot(Ng, L))) : 1.0;
	// regolith of airless bodies: no limb darkening, opposition surge
	float sunRefl = int(atm.detail.x + 0.5) == DETAIL_AIRLESS && tile.params.z < 0.5
		? RegolithReflectance(N, L, normalize(-vPosView), Ng) : max(ndl, 0.0);
	vec3 col = base * (frame.ambient.rgb * ao * ambNight + (frame.sunColor.rgb * sunT * sunRefl + LocalLighting(vPosView, N, lv)) * aoDirect);
	// street lamps, lit when the sun is down
	col += base * cityLight * smoothstep(0.05, -0.03, dot(Ng, L));
	col += cityEmis * smoothstep(0.05, -0.03, dot(Ng, L));
	// sunlight bounced by the ground around onto faces tilted away from the
	// vertical (the shaded sides of stones and crater walls): a sunlit
	// Lambertian half-space of the local albedo gives the irradiance
	// albedo * E_sun * (1 - cos tilt) / 2
	if (tile.params.z < 0.5) {
		vec3 upV = normalize(transpose(mat3(atm.viewToPlanet)) * normalize(mat3(atm.viewToPlanet) * vPosView + atm.camPos.xyz));
		float sunUp = max(dot(L, upV), 0.0);
		float tiltF = 0.5 * (1.0 - clamp(dot(N, upV), -1.0, 1.0));
		col += base * base * frame.sunColor.rgb * (sunUp * tiltF * ao) * (hasAtm ? SunTransmittance(pPlanet) : vec3(1.0)) * tile.morph.z;
	}

	if (tile.params.z > 0.5) { // cloud layer: lit from above, also seen from below
		float a = texture(surfTex, clamp(vUV * tile.surfUV.zw + tile.surfUV.xy, hs, 1.0 - hs)).a * tile.params.y;
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
		// (where the cities draw their own lights: theirs; near them, the
		// texture's brightness for views from orbit toned down to theirs)
		// (the cities' lights: of the buildings drawn, of all of them only so
		// near as the small ones, building.vert's fade at 3 km, and of the
		// roof carpet beyond; elsewhere the texture's)
		night *= (1.0 - max(cityCover * (1.0 - smoothstep(1300.0, 2300.0, length(vPosView))), cityCarpet)) * mix(0.15, 1.0, smoothstep(8000.0, 80000.0, length(vPosView)));
		// (the texture's lights are blurred averages of point sources: cores
		// brightened, dim spill suppressed, so that cities stand out as in
		// photographs; the finest levels' halftone of lit texels blurred
		// further: a glow, not dots)
		vec2 muv = vUV * tile.maskUV.zw + tile.maskUV.xy, mt = 2.0 * hm; // (a texel)
		vec3 ml = vec3(0.0);
		for (int i = -1; i <= 1; i++)
			for (int j = -1; j <= 1; j++)
				ml += texture(maskTex, clamp(muv + vec2(i, j) * 1.5 * mt, hm, 1.0 - hm)).rgb * ((i == 0 ? 2.0 : 1.0) * (j == 0 ? 2.0 : 1.0) / 16.0);
		float lm = max (ml.r, max (ml.g, ml.b));
		col += ml * (3.0 * lm * smoothstep (0.04, 0.25, lm)) * night * tile.params.y;
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

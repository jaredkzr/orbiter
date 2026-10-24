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

// additional texture maps (MapSlot order); missing ones are not sampled
layout(set = 3, binding = 0) uniform sampler2D normMap;   // tangent-space normals (x: +u, y: +v)
layout(set = 3, binding = 1) uniform sampler2D rghnMap;   // smoothness (green)
layout(set = 3, binding = 2) uniform sampler2D metalMap;  // metalness (green)
layout(set = 3, binding = 3) uniform sampler2D emisMap;   // emission, added
layout(set = 3, binding = 4) uniform sampler2D specMap;   // specular colour, smoothness in alpha
layout(set = 3, binding = 5) uniform sampler2D reflMap;   // reflection colour and mask
layout(set = 3, binding = 6) uniform sampler2D translMap; // translucence (lit from behind)
layout(set = 3, binding = 7) uniform sampler2D transmMap; // transmittance (sun shining through)
const uint MAP_NORM = 1u, MAP_RGHN = 2u, MAP_METAL = 4u, MAP_EMIS = 8u,
           MAP_SPEC = 16u, MAP_REFL = 32u, MAP_TRANSL = 64u, MAP_TRANSM = 128u;

layout(push_constant) uniform Draw {
	mat4 modelView;
	vec4 diffuse;
	vec4 emissive;
	vec4 specular;
	vec4 light;     // rgb: sunlight reaching the object (eclipse)
	vec4 pbr;       // metalness, roughness, reflectivity, dielectric F0 scale
	vec4 heat;      // xyz: direction of motion through the air (view), w: heating glow 0..1
	vec4 maps;      // x: flags of the additional texture maps present
} draw;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

#include "brdf.glsl"

// Tangent frame from screen-space derivatives of position and texture
// coordinates (no per-vertex tangents needed): T along +u, B along +v
mat3 CotangentFrame (vec3 N, vec3 p, vec2 uv)
{
	vec3 dp1 = dFdx (p), dp2 = dFdy (p);
	vec2 duv1 = dFdx (uv), duv2 = dFdy (uv);
	vec3 dp2perp = cross (dp2, N), dp1perp = cross (N, dp1);
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
	float det = dot (dp1, dp2perp); // orientation of the solution
	float invmax = inversesqrt (max (max (dot (T, T), dot (B, B)), 1e-30));
	float sgn = det < 0.0 ? -1.0 : 1.0;
	return mat3 (T * (invmax * sgn), B * (invmax * sgn), N);
}

// Analytic approximation of the pre-integrated environment BRDF (Karis)
vec3 EnvBRDF (vec3 F0, float rough, float NoV)
{
	const vec4 c0 = vec4 (-1.0, -0.0275, -0.572, 0.022);
	const vec4 c1 = vec4 (1.0, 0.0425, 1.04, -0.04);
	vec4 r = rough * c0 + c1;
	float a004 = min (r.x * r.x, exp2 (-9.28 * NoV)) * r.x + r.y;
	vec2 AB = vec2 (-1.04, 1.04) * a004 + r.zw;
	return F0 * AB.x + AB.y;
}

// Light arriving from direction d (planet frame): the sky of the planet near
// the camera, or its sunlit surface
vec3 Environment (vec3 d)
{
	if (atm.radii.x <= 0.0) return vec3 (0.0);
	vec3 o = atm.camPos.xyz;
	vec2 tg = RaySphereC (o, d, atm.camC.x);
	float tMax = ATM_NONE;
	vec3 col = vec3 (0.0);
	if (tg.x > 0.0 && tg.x <= tg.y) {
		tMax = tg.x;
		col = AtmGround (normalize (o + d * tg.x)) * frame.sunColor.rgb;
	}
	if (atm.misc.x > 0.5) {
		vec3 T;
		vec3 L = Inscatter (d, tMax, T);
		col = col * T + AtmTonemap (L);
	}
	return col;
}

void main()
{
	vec4 texel = texture(tex, vUV);
	vec3 base = texel.rgb * draw.diffuse.rgb;
	float alpha = texel.a * draw.diffuse.a;

	if (draw.emissive.a > 0.5) { // unlit
		outColor = vec4(base, alpha);
		return;
	}

	uint maps = uint(draw.maps.x + 0.5);
	vec3 N = normalize(vNormalView);
	vec3 V = normalize(-vPosView);
	if (dot(N, V) < 0.0) N = -N; // double-sided surfaces
	if ((maps & MAP_NORM) != 0u) {
		vec3 n = texture(normMap, vUV).xyz * 2.0 - 1.0;
		vec3 Np = normalize(CotangentFrame(N, vPosView, vUV) * n);
		if (all(equal(Np, Np))) N = Np; // degenerate texture coordinates: keep N
	}
	vec3 L = frame.sunDir.xyz;
	float NoL = max(dot(N, L), 0.0);
	float NoV = max(dot(N, V), 1e-4);

	float metal = draw.pbr.x;
	float rough = draw.pbr.y;
	float reflectivity = draw.pbr.z;
	vec3 specTint = vec3(1.0);
	if ((maps & MAP_SPEC) != 0u) {
		vec4 sp = texture(specMap, vUV);
		specTint = sp.rgb;
		rough = 1.0 - sp.a;
	}
	if ((maps & MAP_RGHN) != 0u) rough = 1.0 - texture(rghnMap, vUV).g;
	if ((maps & MAP_METAL) != 0u) metal = texture(metalMap, vUV).g;
	rough = clamp(rough, 0.04, 1.0);
	vec3 F0 = mix(vec3(0.04 * draw.pbr.w) * specTint, base, metal);
	vec3 reflTint = vec3(1.0);
	if ((maps & MAP_REFL) != 0u) {
		reflTint = texture(reflMap, vUV).rgb;
		reflectivity = 1.0; // the map masks the reflections per pixel
	} else if ((maps & (MAP_RGHN | MAP_METAL)) != 0u) {
		reflectivity = max(reflectivity, 1.0 - rough);
	}

	// sunlight: eclipses, shadows, and extinction in the atmosphere of the
	// planet near the camera
	bool hasAtm = atm.misc.x > 0.5;
	vec3 sun = frame.sunColor.rgb * draw.light.rgb * SunShadow(vPosView, N);
	if (hasAtm) {
		vec3 pPlanet = mat3(atm.viewToPlanet) * vPosView + atm.camPos.xyz;
		sun *= SunTransmittance(pPlanet) * CloudShadow(pPlanet);
	}

	// diffuse (Orbiter's convention: albedo times N.L) and specular reflection
	// of the sun, local lights, ambient, and reflections of the environment
	vec3 diffuse = base * (1.0 - metal);
	float ao = draw.maps.y > 0.5 ? 1.0 : AmbientOcclusion();
	float aoDirect = mix(1.0, ao, frame.aoParams.z); // contact darkening in direct light too
	vec3 col = diffuse * (frame.ambient.rgb * ao + (sun * NoL + LocalLighting(vPosView, N)) * aoDirect);
	col += sun * SpecularBRDF(N, V, L, F0, rough) * PI;
	if (reflectivity > 0.0) {
		vec3 R = reflect(-V, N);
		R = normalize(mix(R, N, rough * rough)); // rough surfaces: towards the normal
		vec3 env = Environment(normalize(mat3(atm.viewToPlanet) * R));
		col += env * EnvBRDF(F0, rough, NoV) * reflectivity * reflTint * ao;
	}
	col += draw.emissive.rgb * texel.rgb;
	if ((maps & MAP_TRANSL) != 0u) // sunlight scattered through from behind
		col += texture(translMap, vUV).rgb * sun * max(dot(-N, L), 0.0);
	if ((maps & MAP_TRANSM) != 0u) { // sun seen through the surface
		vec4 tm = texture(transmMap, vUV);
		float spot = pow(max(dot(V, -L), 0.0), mix(2.0, 200.0, tm.a));
		col += tm.rgb * sun * spot;
	}
	if ((maps & MAP_EMIS) != 0u) col += texture(emisMap, vUV).rgb;

	// aerodynamic heating: surfaces facing the airflow glow, from dull red
	// to orange-white
	if (draw.heat.w > 0.0) {
		float f = max(dot(normalize(vNormalView), draw.heat.xyz), 0.0);
		float t = draw.heat.w;
		col += vec3(1.0, 0.25 + 0.45 * t, 0.06 + 0.3 * t) * (4.0 * t * t * f);
	}

	if (hasAtm) { // aerial perspective
		vec3 T;
		vec3 Lin = Inscatter(normalize(mat3(atm.viewToPlanet) * vPosView), length(vPosView), T);
		col = col * T + AtmTonemap(Lin);
	}
	outColor = vec4(col, alpha);
}

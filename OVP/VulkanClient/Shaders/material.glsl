// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Surface material of mesh groups from the material parameters and the
// additional texture maps (set 3), shared by the scene and prepass shaders

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

struct Material {
	vec3 N;             // shading normal (view space, towards the camera)
	float metal, rough;
	float reflectivity; // strength of environment reflections
	vec3 specTint;      // dielectric specular colour
	vec3 reflTint;      // colour of environment reflections
};

// pbr: metalness, roughness, reflectivity (material parameters)
Material SurfaceMaterial (vec3 normalView, vec3 posView, vec2 uv, uint maps, vec4 pbr)
{
	Material m;
	vec3 N = normalize (normalView);
	if (dot (N, posView) > 0.0) N = -N; // double-sided surfaces
	if ((maps & MAP_NORM) != 0u) {
		// (z from x and y: two-channel maps such as BC5 have none)
		vec3 n;
		n.xy = texture (normMap, uv).xy * 2.0 - 1.0;
		n.z = sqrt (max (0.0, 1.0 - dot (n.xy, n.xy)));
		vec3 Np = normalize (CotangentFrame (N, posView, uv) * n);
		if (all (equal (Np, Np))) N = Np; // degenerate texture coordinates: keep N
	}
	m.N = N;
	m.metal = pbr.x;
	m.rough = pbr.y;
	m.reflectivity = pbr.z;
	m.specTint = vec3 (1.0);
	if ((maps & MAP_SPEC) != 0u) {
		vec4 sp = texture (specMap, uv);
		m.specTint = sp.rgb;
		m.rough = 1.0 - sp.a;
	}
	if ((maps & MAP_RGHN) != 0u) m.rough = 1.0 - texture (rghnMap, uv).g;
	if ((maps & MAP_METAL) != 0u) m.metal = texture (metalMap, uv).g;
	m.rough = clamp (m.rough, 0.04, 1.0);
	m.reflTint = vec3 (1.0);
	if ((maps & MAP_REFL) != 0u) {
		m.reflTint = texture (reflMap, uv).rgb;
		m.reflectivity = 1.0; // the map masks the reflections per pixel
	} else if ((maps & (MAP_RGHN | MAP_METAL)) != 0u) {
		m.reflectivity = max (m.reflectivity, 1.0 - m.rough);
	}
	return m;
}

// Octahedral encoding of unit vectors
vec2 OctEncode (vec3 n)
{
	n /= abs (n.x) + abs (n.y) + abs (n.z);
	vec2 e = n.xy;
	if (n.z < 0.0) e = (1.0 - abs (n.yx)) * vec2 (n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return e;
}

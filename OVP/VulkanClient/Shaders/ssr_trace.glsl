// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Screen-space reflections, tracing (included by ssr_trace.comp and, with
// hardware ray queries, ssr_trace_rt.comp): one reflected ray per reflective pixel,
// its direction drawn from the visible normals of the GGX distribution, is
// marched through the depth pyramid (hierarchical: large steps across cells
// whose nearest surface lies beyond the ray). The radiance at a hit comes
// from the previous frame's image, reprojected with the motion vectors.
// Outputs: hit position (view space; or the ray direction if nothing was
// hit) with the direction's probability density (negative for misses), and
// the radiance with its confidence.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D prepass;   // view normal (xyz), view depth (w; 0: none)
layout(set = 0, binding = 1) uniform sampler2D material;  // shading normal (octahedral), roughness, reflectivity
layout(set = 0, binding = 2) uniform sampler2D hiz;       // depth pyramid
layout(set = 0, binding = 3) uniform sampler2D velocity;  // screen motion since the last frame [uv]
layout(set = 0, binding = 4) uniform sampler2D prevColor; // previous frame's scene (mipmapped)
layout(set = 0, binding = 5, rgba32f) uniform writeonly image2D hitOut;
layout(set = 0, binding = 6, rgba16f) uniform writeonly image2D colOut;
layout(set = 0, binding = 10, rgba16f) uniform writeonly image2D plumeOut; // light of engine plumes along the ray

#include "ssr.glsl"
#define PLUME_SET 0
#define PLUME_BINDING 11
#include "plume.glsl"
#ifdef RT
#include "rt.glsl"
#endif

const int MAX_ITER = 160;

uvec2 Hash2 (uvec3 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	return v.xy;
}

// Visible normal of the GGX distribution (Heitz 2018), tangent space (z: normal)
vec3 SampleVNDF (vec3 Ve, float a, vec2 u)
{
	vec3 Vh = normalize (vec3 (a * Ve.x, a * Ve.y, Ve.z));
	float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	vec3 T1 = lensq > 0.0 ? vec3 (-Vh.y, Vh.x, 0.0) * inversesqrt (lensq) : vec3 (1.0, 0.0, 0.0);
	vec3 T2 = cross (Vh, T1);
	float r = sqrt (u.x), phi = 2.0 * PI * u.y;
	float t1 = r * cos (phi), t2 = r * sin (phi);
	float s = 0.5 * (1.0 + Vh.z);
	t2 = (1.0 - s) * sqrt (1.0 - t1 * t1) + s * t2;
	vec3 Nh = t1 * T1 + t2 * T2 + sqrt (max (0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
	return normalize (vec3 (a * Nh.x, a * Nh.y, max (0.0, Nh.z)));
}

void main ()
{
	ivec2 p = ivec2 (gl_GlobalInvocationID.xy);
	if (p.x >= int (pc.size.x) || p.y >= int (pc.size.y)) return;
	vec4 d = texelFetch (prepass, p, 0);
	vec4 m = texelFetch (material, p, 0);
	m.z = abs (m.z); // (negative on terrain)
	float z = d.w;
	if (z <= 0.0 || m.w <= 0.0 || m.z >= pc.maxRough) {
		imageStore (hitOut, p, vec4 (0.0));
		imageStore (colOut, p, vec4 (0.0));
		imageStore (plumeOut, p, vec4 (0.0));
		return;
	}
	vec3 Ng = d.xyz;
	vec3 N = OctDecode (m.xy);
	float a = m.z * m.z;
	vec3 P = ViewPos (vec2 (p) + 0.5, z);
	vec3 V = normalize (-P);
	if (dot (N, V) < 1e-3) N = normalize (N + V * (1e-3 - dot (N, V))); // facing away (normal maps)
	float NoV = max (dot (N, V), 1e-3);

	// reflected direction
	vec3 T = normalize (abs (N.z) < 0.999 ? cross (vec3 (0.0, 0.0, 1.0), N) : cross (vec3 (1.0, 0.0, 0.0), N));
	vec3 B = cross (N, T);
	uvec2 h = Hash2 (uvec3 (p, uint (pc.frame)));
	vec2 u = vec2 (h) / 4294967296.0;
	u.x *= 0.85; // the far tail of the distribution only adds noise
	vec3 H = SampleVNDF (vec3 (dot (V, T), dot (V, B), dot (V, N)), a, u);
	H = T * H.x + B * H.y + N * H.z;
	vec3 L = reflect (-V, H);
	float NoH = max (dot (N, H), 0.0);
	float pdf = GGX_D (NoH, a) * SmithG1 (NoV, a) / (4.0 * NoV);
	if (dot (L, Ng) <= 0.0) L = reflect (-V, N); // below the surface
	pdf = max (pdf, 1e-4);

	// ray in screen space: pixel position linear in t, and so is 1/z
	vec3 o = P + Ng * (0.002 * z);
	float len = L.z < 0.0 ? min (1e3 * z, 0.95 * (o.z - 0.05) / -L.z) : 1e3 * z;
	vec3 e = o + L * len;
	vec2 s0 = Project (o), s1 = Project (e);
	float k0 = 1.0 / o.z, k1 = 1.0 / e.z;
	// clip the end to the screen
	vec2 ds = s1 - s0;
	float tEnd = 1.0;
	if (ds.x > 0.0) tEnd = min (tEnd, (pc.size.x - 0.5 - s0.x) / ds.x);
	if (ds.x < 0.0) tEnd = min (tEnd, (0.5 - s0.x) / ds.x);
	if (ds.y > 0.0) tEnd = min (tEnd, (pc.size.y - 0.5 - s0.y) / ds.y);
	if (ds.y < 0.0) tEnd = min (tEnd, (0.5 - s0.y) / ds.y);
	float dsLen = length (ds);
	bool hit = false;
	vec2 hitPix;
	float hitZ = 0.0;
	if (dsLen * tEnd > 1.0 && tEnd > 0.0) {
		float eps = 0.01 / max (abs (ds.x), abs (ds.y));
		float t = 1.5 / dsLen;
		int level = 0, maxLevel = int (pc.levels) - 1;
		for (int it = 0; it < MAX_ITER && t < tEnd; it++) {
			vec2 s = s0 + ds * t;
			float cell = float (1 << level);
			vec2 c = floor (s / cell);
			// parameter where the ray leaves the cell
			vec2 bnd = (c + step (0.0, ds)) * cell;
			vec2 tb = vec2 (ds.x != 0.0 ? (bnd.x - s0.x) / ds.x : 1e30, ds.y != 0.0 ? (bnd.y - s0.y) / ds.y : 1e30);
			float tn = min (min (tb.x, tb.y) + eps, tEnd);
			float za = 1.0 / mix (k0, k1, t), zb = 1.0 / mix (k0, k1, tn);
			float zMin = texelFetch (hiz, min (ivec2 (c), textureSize (hiz, level) - 1), level).r;
			if (max (za, zb) < zMin) { // in front of everything in the cell
				t = tn;
				level = min (level + 1, maxLevel);
			} else if (level > 0) {
				level--;
			} else {
				vec3 q = ViewPos (c + 0.5, zMin);
				// a hit if the ray is not too far behind the surface, and in
				// front of the reflecting surface's plane
				if (min (za, zb) <= zMin * (1.0 + pc.thickness) && dot (q - P, Ng) > 0.01 * z) {
					hit = true;
					hitPix = c + 0.5;
					hitZ = zMin;
					break;
				}
				t = tn;
				level = min (level + 1, maxLevel);
			}
		}
	}

	vec4 col = vec4 (0.0);
	vec3 q = vec3 (0.0);
	if (hit) {
		q = ViewPos (hitPix, hitZ);
		vec2 uv = hitPix / pc.size;
		// back faces of the reflected surface are not visible in the image
		vec3 nq = texelFetch (prepass, ivec2 (hitPix), 0).xyz;
		float conf = smoothstep (0.0, 0.2, -dot (nq, L));
		// the image ends at the screen edges
		vec2 edge = min (uv, 1.0 - uv);
		conf *= smoothstep (0.0, 0.06, min (edge.x, edge.y));
		vec2 prev = uv - texelFetch (velocity, ivec2 (hitPix), 0).xy;
		if (any (lessThan (prev, vec2 (0.0))) || any (greaterThan (prev, vec2 (1.0)))) conf = 0.0;
		float lod = clamp (log2 (1.0 + length (hitPix - s0) * a * 0.3), 0.0, 6.0);
		col = vec4 (textureLod (prevColor, prev, lod).rgb, conf);
	}
#ifdef RT
	// the ray against the vessels and bases: the screen-space hit is kept
	// where it is the surface the ray reaches (exact shading), or lies in
	// front of it (terrain, which is not in the ray tracing scene);
	// elsewhere the traced surface is shaded
	RTHit rh;
	if (TraceRT (o, L, 0.002 * z, rh)) {
		float ds = hit ? length (q - o) : 1e30;
		float off = hit ? length (cross (q - o, L)) : 1e30; // the visible surface's distance from the ray
		bool keep = hit && col.a > 0.5 && ds < rh.t * 1.03 + 0.01 * z && off < 0.01 * q.z + 0.05;
		if (!keep) {
			col = vec4 (rh.radiance, 1.0);
			q = o + L * rh.t;
			hit = true;
		}
	}
#endif
	// engine plumes between the surface and the hit: their emission (the
	// rays are short and few: a coarse march)
	vec3 plume = vec3 (0.0);
	uint slot = uint (pc.plumeSlot), nPlumes = plumeCount[slot];
	float tHit = hit ? length (q - o) : 1e5;
	for (uint i = 0u; i < nPlumes; i++) {
		PlumeParams P = plumes[slot * MAX_PLUMES + i];
		vec3 lo, ld;
		float t0, t1;
		if (!PlumeBox (P, o, L, lo, ld, t0, t1)) continue;
		t0 = max (t0, 0.0);
		t1 = min (t1, tHit / P.shape.x);
		if (t1 <= t0) continue;
		int n = int (clamp (ceil ((t1 - t0) / 0.6), 6.0, 24.0));
		PlumeLight l = MarchPlume (P, lo, ld, t0, t1, n, float (h.y & 0xffffu) / 65536.0, vec3 (0.0), vec3 (0.0), vec3 (0.0, 0.0, 1.0));
		plume += l.emit;
	}
	imageStore (hitOut, p, hit ? vec4 (q, pdf) : vec4 (L, -pdf));
	imageStore (colOut, p, col);
	imageStore (plumeOut, p, vec4 (plume, 0.0));
}

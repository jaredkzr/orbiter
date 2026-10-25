// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Hardware ray queries against the ray tracing scene (RayTracing.cpp):
// vessels and bases in view space. A hit is shaded from its texture and
// material: sunlight with a traced shadow, ambient light, emission, and
// the environment probe for its own reflections.
// Sets: 1 frame, 2 ray tracing scene, 3 bindless textures, 4 atmosphere.

#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_nonuniform_qualifier : require

#define FRAME_SET 1
#include "frame.glsl"
layout(set = 1, binding = 4) uniform samplerCube envProbe;

#define ATM_SET 4
#include "atmosphere.glsl"

struct RTInstance { // must match InstanceRecord in RayTracing.cpp
	uvec2 vtx, idx;     // device addresses: group vertices, mesh indices (16 bit)
	uint firstIndex, tex, flags, owner; // owner: VObject::Id (0: terrain)
	vec4 diffuse;
	vec4 emissive;      // rgb, w: sunlight reaching the object
	vec4 pbr;           // metalness, roughness, reflectivity, F0 scale
	vec4 uv;            // texture coordinates: offset, scale
};
const uint RT_UNLIT = 1u;
const uint RT_ALPHA = 2u; // alpha-tested texture (foliage): solid where the texture is

layout(set = 2, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 2, binding = 1, std430) readonly buffer Instances { RTInstance inst[]; };
layout(set = 3, binding = 0) uniform sampler2D rtTex[];

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RTFloats { float f[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RTUints { uint u[]; };

struct RTHit {
	float t;
	vec3 radiance;
};

uint RTIndex (uvec2 addr, uint i)
{
	uint w = RTUints (addr).u[i >> 1];
	return (i & 1u) != 0u ? w >> 16 : w & 0xffffu;
}

// a candidate hit of a ray query on alpha-tested geometry is solid
bool RTCandidateSolid (rayQueryEXT q)
{
	RTInstance r = inst[rayQueryGetIntersectionInstanceCustomIndexEXT (q, false) + rayQueryGetIntersectionGeometryIndexEXT (q, false)];
	if ((r.flags & RT_ALPHA) == 0u) return true;
	uint prim = uint (rayQueryGetIntersectionPrimitiveIndexEXT (q, false));
	vec2 bc = rayQueryGetIntersectionBarycentricsEXT (q, false);
	vec3 w = vec3 (1.0 - bc.x - bc.y, bc.x, bc.y);
	vec2 uv = vec2 (0.0);
	for (int k = 0; k < 3; k++) {
		uint vi = RTIndex (r.idx, r.firstIndex + prim * 3u + uint (k)) * 8u;
		RTFloats v = RTFloats (r.vtx);
		uv += w[k] * vec2 (v.f[vi + 6u], v.f[vi + 7u]);
	}
	return textureLod (rtTex[nonuniformEXT (r.tex)], uv * r.uv.zw + r.uv.xy, 0.0).a >= 0.5;
}

// the sun is not hidden from p by vessels or bases
float RTSunVisible (vec3 p, vec3 toSun)
{
	rayQueryEXT sq;
	rayQueryInitializeEXT (sq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, p, 0.0, toSun, 1e5);
	while (rayQueryProceedEXT (sq))
		if (rayQueryGetIntersectionTypeEXT (sq, false) == gl_RayQueryCandidateIntersectionTriangleEXT && RTCandidateSolid (sq))
			rayQueryConfirmIntersectionEXT (sq);
	return rayQueryGetIntersectionTypeEXT (sq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

bool TraceRT (vec3 o, vec3 d, float tMin, out RTHit h)
{
	rayQueryEXT rq;
	rayQueryInitializeEXT (rq, tlas, gl_RayFlagsNoneEXT, 0xFF, o, tMin, d, 1e5);
	while (rayQueryProceedEXT (rq))
		if (rayQueryGetIntersectionTypeEXT (rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT && RTCandidateSolid (rq))
			rayQueryConfirmIntersectionEXT (rq);
	if (rayQueryGetIntersectionTypeEXT (rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return false;
	h.t = rayQueryGetIntersectionTEXT (rq, true);
	// (one record per geometry of the instance)
	RTInstance r = inst[rayQueryGetIntersectionInstanceCustomIndexEXT (rq, true) + rayQueryGetIntersectionGeometryIndexEXT (rq, true)];
	uint prim = uint (rayQueryGetIntersectionPrimitiveIndexEXT (rq, true));
	vec2 bc = rayQueryGetIntersectionBarycentricsEXT (rq, true);
	vec3 w = vec3 (1.0 - bc.x - bc.y, bc.x, bc.y);
	vec3 n = vec3 (0.0);
	vec2 uv = vec2 (0.0);
	for (int k = 0; k < 3; k++) {
		uint vi = RTIndex (r.idx, r.firstIndex + prim * 3u + uint (k)) * 8u;
		RTFloats v = RTFloats (r.vtx);
		n += w[k] * vec3 (v.f[vi + 3u], v.f[vi + 4u], v.f[vi + 5u]);
		uv += w[k] * vec2 (v.f[vi + 6u], v.f[vi + 7u]);
	}
	mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT (rq, true);
	vec3 N = normalize (mat3 (o2w) * n);
	if (dot (N, d) > 0.0) N = -N; // double-sided
	vec3 p = o + d * h.t;

	// texture detail: coarser for distant hits and rough reflections
	float lod = clamp (log2 (1.0 + h.t * 0.05), 0.0, 6.0);
	vec4 texel = textureLod (rtTex[nonuniformEXT (r.tex)], uv * r.uv.zw + r.uv.xy, lod);
	vec3 base = texel.rgb * r.diffuse.rgb;
	if ((r.flags & RT_UNLIT) != 0u) { h.radiance = base; return true; }

	float metal = r.pbr.x, rough = r.pbr.y;
	vec3 L = frame.sunDir.xyz;
	float NoL = max (dot (N, L), 0.0);
	vec3 sun = frame.sunColor.rgb * r.emissive.w;
	if (atm.misc.x > 0.5) {
		vec3 pp = mat3 (atm.viewToPlanet) * p + atm.camPos.xyz;
		sun *= SunTransmittance (pp) * CloudShadow (pp);
	}
	if (NoL > 0.0) sun *= RTSunVisible (p + N * (0.002 * length (p) + 0.01), L);
	vec3 col = base * (1.0 - metal) * (frame.ambient.rgb + sun * NoL);
	col += r.emissive.rgb * texel.rgb;
	// own reflections: the environment probe
	if (r.pbr.z > 0.0 && frame.probe.x > 0.5) {
		vec3 R = reflect (d, N);
		vec3 env = textureLod (envProbe, mat3 (frame.viewToGlobal) * R, rough * frame.probe.y).rgb;
		vec3 F0 = mix (vec3 (0.04 * r.pbr.w), base, metal);
		col += env * F0 * r.pbr.z;
	}
	h.radiance = col;
	return true;
}

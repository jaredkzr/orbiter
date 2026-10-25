// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Instanced impostors (far plants): as mesh_inst.vert, the view of the
// impostor chosen per instance (see Impostor)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Draw {
	mat4 modelView;  // global frame relative to the camera -> view
	vec4 diffuse;
	vec4 emissive;
	vec4 specular;
	vec4 light;
	vec4 pbr;
	vec4 heat;
	vec4 maps;
} draw;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inst0;
layout(location = 4) in vec4 inst1;
layout(location = 5) in vec4 inst2;


// Impostors (far plants, ScatterAssets): the mesh is the quad of view 0
// (through the plant's axis, facing +z of the object frame; normal along
// it) and a horizontal top quad (normal +y). The view nearest the camera's
// direction in the object frame is chosen here: the side quad turned about
// the axis to face it, the texture coordinates shifted to its frame of the
// atlas (IMP_VIEWS frames of IMP_FU in a row). maps.w > 0: the range of
// the layer [m]: instances shrink away over its last 15%.
const int IMP_VIEWS = 8;
const float IMP_FU = 64.0 / 512.0;

void Impostor (mat4 modelView, mat4 M, inout vec3 pos, inout vec3 nrm, inout vec2 uv, float range)
{
	// camera in the frame of the transforms (modelView = [A | t]: at -A^T t)
	vec3 cam = -transpose(mat3(modelView)) * modelView[3].xyz;
	vec3 c = transpose(mat3(M)) * (cam - M[3].xyz); // (object frame, scaled)
	float fade = 1.0;
	if (range > 0.0) fade = clamp((range - length(cam - M[3].xyz)) / (0.15 * range), 0.0, 1.0);
	if (nrm.y > 0.5) // top quad: only from above (edge-on and in the way otherwise)
		pos *= smoothstep(0.25, 0.45, abs(c.y) / max(length(c), 1e-6));
	else { // side quad
		int k = int(floor(atan(c.x, c.z) / (6.2831853 / float(IMP_VIEWS)) + 0.5));
		k = ((k % IMP_VIEWS) + IMP_VIEWS) % IMP_VIEWS;
		float th = float(k) * 6.2831853 / float(IMP_VIEWS);
		float s = sin(th), co = cos(th);
		pos = vec3(pos.x * co, pos.y, -pos.x * s);
		nrm = vec3(s, 0.0, co);
		uv.x += float(k) * IMP_FU;
	}
	pos *= fade;
}

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec2 vUV;

void main()
{
	mat4 M = transpose(mat4(inst0, inst1, inst2, vec4(0.0, 0.0, 0.0, 1.0)));
	vec3 pos = inPos, nrm = inNormal;
	vec2 uv = inUV;
	Impostor(draw.modelView, M, pos, nrm, uv, draw.maps.w);
	vec4 p = draw.modelView * (M * vec4(pos, 1.0));
	vPosView = p.xyz;
	vNormalView = mat3(draw.modelView) * (mat3(M) * nrm);
	vUV = uv;
	gl_Position = frame.proj * p;
}

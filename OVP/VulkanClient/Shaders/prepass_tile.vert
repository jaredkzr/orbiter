// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Prepass of planet surface tiles (see prepass.vert), with the morph of
// the continuous level of detail (as tile.vert)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "swell.glsl"

layout(push_constant) uniform Prepass {
	mat4 modelView;
	mat4 prevClipFromView; // view space now -> clip space of the previous frame
	vec4 maskUV;
	vec4 params;
	vec4 morph;            // morph: start distance [m], 1/range [1/m] (0: none)
	vec4 vessel;           // nearest vessel on the ground, view space (w: present)
	vec4 scale;            // x: the compact vertices' scale [m] (tile.vert)
} pp;

layout(constant_id = 1) const bool COMPACT = false; // (tile.vert)
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 4) in vec4 inMorph;

#include "tilevertex.glsl"

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out vec4 vClip;
layout(location = 3) out vec4 vPrevClip;
layout(location = 4) out vec2 vUV;

invariant gl_Position; // (the scene's depth starts as the prepass's: the same surfaces at the same depths, Scene::RenderPrepass)

void main()
{
	vec3 pos = inPos.xyz, mor = inMorph.xyz, nrm = inNormal;
	vec2 uv = inUV;
	if (COMPACT) {
		pos *= pp.scale.x, mor *= pp.scale.x;
		nrm = TileOctDecode (inNormal.xy);
		uv = TileGridUV (uint (gl_VertexIndex));
	}
	vec3 p0 = (pp.modelView * vec4(pos, 1.0)).xyz;
	float d = length (p0);
	if (pp.vessel.w > 0.5) d = min (d, length (p0 - pp.vessel.xyz));
	float m = clamp ((d - pp.morph.x) * pp.morph.y, 0.0, 1.0);
	vec4 p = pp.modelView * vec4(mix (pos, mor, m), 1.0);
	vec3 nsw = mat3(pp.modelView) * nrm;
	p.xyz = SwellDisplace (p.xyz, nsw); // (the sea's swell, Water W8)
	vPosView = p.xyz;
	vNormalView = nsw;
	gl_Position = frame.proj * RelApparent (p); // (relativistic view: as seen)
	vClip = gl_Position - vec4(frame.jitter.xy * gl_Position.w, 0.0, 0.0);
	vPrevClip = pp.prevClipFromView * RelApparent (p);
	vUV = uv;
}

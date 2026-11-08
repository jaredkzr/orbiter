// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Reentry plasma sheath (Plasma.cpp): a vessel's mesh drawn again, its
// windward side pushed out along the normals to the layer's outer edge

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Draw {
	mat4 modelView;  // object -> view
	vec4 flow;       // direction of motion through the air (view), w: heating level 0..1
	vec4 shape;      // thickness of the layer [m], vessel radius [m], time [s], intensity
	vec4 centre;     // the vessel's centre (view)
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 vPosView;
layout(location = 1) out vec3 vNormalView;
layout(location = 2) out float vPush;   // how far this vertex was pushed out (0..1)

void main ()
{
	vec3 n = mat3 (pc.modelView) * inNormal;
	float ln = length (n);
	n = ln > 0.0 ? n / ln : vec3 (0.0);
	// (facing the flow: out to the shock's stand-off; edge-on and leeward:
	// on the hull, where the shader leaves it dark)
	float face = dot (n, pc.flow.xyz);
	float push = smoothstep (-0.05, 0.5, face);
	vec4 p = pc.modelView * vec4 (inPos, 1.0);
	// (upstream, the shock's stand-off: along the flow rather than the
	// normals, so that the vertices of a sharp edge, with their normals
	// apart, stay together where both face the flow)
	p.xyz += pc.flow.xyz * (pc.shape.x * push);
	vPosView = p.xyz;
	vNormalView = n;
	vPush = push;
	gl_Position = frame.proj * RelApparent (p);
}

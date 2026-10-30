// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Street lamps of the cities (CityField): glows facing the camera, a quad
// per instance (six vertices), for particle.frag. Lit when the sun is down
// where they stand; at least a few pixels across (dimmer then), fading out
// with the distance.

#version 450

layout(location = 0) in vec4 inLamp; // position (frame of the transforms), white LED (1) or sodium (0)

layout(push_constant) uniform Lamps {
	mat4 frameToView;
	mat4 proj;
	vec4 centre;  // xyz: origin of the frame of the transforms (planet frame), w: smallest half size per view depth
	vec4 sun;     // xyz: direction to the sun (planet frame), w: brightness
	vec4 range;   // x: fading out from, y: to [m], z: half size [m]
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

const vec2 C[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main()
{
	vec3 up = normalize(pc.centre.xyz + inLamp.xyz);
	float on = smoothstep(0.04, -0.02, dot(up, pc.sun.xyz));
	vec4 v = pc.frameToView * vec4(inLamp.xyz, 1.0);
	float fade = on * (1.0 - smoothstep(pc.range.x, pc.range.y, length(v.xyz)));
	if (fade <= 0.0 || v.z <= 1.0) { // (none: degenerate)
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		vUV = vec2(0.0), vColor = vec4(0.0);
		return;
	}
	float h = max(pc.range.z, v.z * pc.centre.w);
	float e = pc.range.z / h; // (spread over a larger glow: dimmer, not all of it)
	vec2 c = C[gl_VertexIndex % 6];
	vec3 col = inLamp.w > 0.5 ? vec3(0.9, 0.95, 1.0) : vec3(1.0, 0.58, 0.2);
	vColor = vec4(col * (pc.sun.w * fade * mix(e, 1.0, 0.4)), 1.0);
	vUV = c * 0.5 + 0.5;
	gl_Position = pc.proj * vec4(v.xy + c * h, v.z, 1.0);
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Instanced impostors (far plants) as shadow casters: as shadow_inst.vert,
// the impostor turned to face the sun (alpha.yzw: the direction to it in
// the frame of the transforms), the top quad seen from a high sun only

#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inst0;
layout(location = 4) in vec4 inst1;
layout(location = 5) in vec4 inst2;

layout(push_constant) uniform Caster {
	mat4 mvp;     // global frame relative to the camera -> shadow map clip space
	vec4 alpha;   // x: texture alpha below which there is no shadow
} caster;

layout(location = 0) out vec2 vUV;

void main()
{
	mat4 M = transpose(mat4(inst0, inst1, inst2, vec4(0.0, 0.0, 0.0, 1.0)));
	vec3 pos = inPos;
	vec2 uv = inUV;
	vec3 s = transpose(mat3(M)) * caster.alpha.yzw; // (object frame, scaled)
	if (inNormal.y > 0.5)
		pos *= smoothstep(0.25, 0.45, abs(s.y) / max(length(s), 1e-6));
	else {
		int k = int(floor(atan(s.x, s.z) / (6.2831853 / 8.0) + 0.5));
		k = ((k % 8) + 8) % 8;
		float th = float(k) * 6.2831853 / 8.0;
		pos = vec3(pos.x * cos(th), pos.y, -pos.x * sin(th));
		uv.x += float(k) * (64.0 / 512.0);
	}
	gl_Position = caster.mvp * (M * vec4(pos, 1.0));
	vUV = uv;
}

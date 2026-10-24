// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Prepass of planet surface tiles (see prepass.frag): water surfaces are
// reflective

#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 2, binding = 0) uniform sampler2D maskTex;

layout(push_constant) uniform Prepass {
	mat4 modelView;
	mat4 prevClipFromView;
	vec4 maskUV;  // offset (xy), scale (zw)
	vec4 params;  // x: mask present
} pp;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec4 vClip;
layout(location = 3) in vec4 vPrevClip;
layout(location = 4) in vec2 vUV;

layout(location = 0) out vec4 outData;
layout(location = 1) out vec2 outVelocity; // screen motion since the last frame [uv]
layout(location = 2) out vec4 outMaterial; // shading normal (octahedral), roughness (negative: terrain), reflectivity

vec2 OctEncode (vec3 n)
{
	n /= abs (n.x) + abs (n.y) + abs (n.z);
	vec2 e = n.xy;
	if (n.z < 0.0) e = (1.0 - abs (n.yx)) * vec2 (n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return e;
}

void main()
{
	vec3 n = vNormalView;
	if (dot(n, n) < 1e-12) n = cross(dFdx(vPosView), dFdy(vPosView));
	n = normalize(n);
	if (dot(n, vPosView) > 0.0) n = -n; // double-sided: towards the camera
	outData = vec4(n, vPosView.z);
	outVelocity = (vClip.xy / vClip.w - vPrevClip.xy / max(vPrevClip.w, 1e-6)) * 0.5;

	// water (mask alpha 0): smooth near the camera (as in tile.frag); far
	// away its reflections are those of the sky
	float refl = 0.0, rough = 1.0;
	float dist = length(vPosView);
	if (pp.params.x > 0.5 && dist < 5e4) {
		vec2 hm = 0.5 / vec2(textureSize(maskTex, 0));
		float a = texture(maskTex, clamp(vUV * pp.maskUV.zw + pp.maskUV.xy, hm, 1.0 - hm)).a;
		if (a < 0.5) {
			refl = 1.0;
			rough = mix(0.05, 0.3, smoothstep(2e3, 3e5, dist));
		}
	}
	outMaterial = vec4(OctEncode(n), -rough, refl); // (negative roughness: terrain)
}

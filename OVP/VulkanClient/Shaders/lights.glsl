// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Diffuse light from local light sources (vessel lights). Requires frame.glsl.

// vis: visibility of the first four lights (ray traced shadows)
vec3 LocalLighting (vec3 posView, vec3 N, vec4 vis)
{
	vec3 sum = vec3(0.0);
	int n = int(frame.lightCount.x);
	for (int i = 0; i < n; i++) {
		LocalLight l = frame.lights[i];
		vec3 L = l.pos.xyz - posView;
		float d = length(L);
		if (d > l.pos.w || d < 1e-4) continue;
		L /= d;
		// distance attenuation, at most full intensity (Orbiter's coefficients
		// make it far larger near the light); clamped before the fades below,
		// which would otherwise be lost in the clamp and leave hard edges
		float a = min(1.0 / max(l.att.x + l.att.y * d + l.att.z * d * d, 1e-4), 1.0);
		// fading to nothing at the range (the attenuation alone does not)
		float x = d / l.pos.w;
		x = clamp(1.0 - x * x * x * x, 0.0, 1.0);
		a *= x * x;
		if (l.dir.w > 1.5) // spot: full intensity inside the umbra cone, none outside the penumbra
			a *= smoothstep(l.spot.x, l.att.w, dot(-L, l.dir.xyz));
		if (i < 4) a *= vis[i];
		sum += l.col.rgb * a * max(dot(N, L), 0.0);
	}
	return sum;
}

vec3 LocalLighting (vec3 posView, vec3 N) { return LocalLighting(posView, N, vec4(1.0)); }

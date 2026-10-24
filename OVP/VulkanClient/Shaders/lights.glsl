// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Diffuse light from local light sources (vessel lights). Requires frame.glsl.

vec3 LocalLighting (vec3 posView, vec3 N)
{
	vec3 sum = vec3(0.0);
	int n = int(frame.lightCount.x);
	for (int i = 0; i < n; i++) {
		LocalLight l = frame.lights[i];
		vec3 L = l.pos.xyz - posView;
		float d = length(L);
		if (d > l.pos.w || d < 1e-4) continue;
		L /= d;
		float a = 1.0 / max(l.att.x + l.att.y * d + l.att.z * d * d, 1e-4);
		if (l.dir.w > 1.5) // spot: full intensity inside the umbra cone, none outside the penumbra
			a *= smoothstep(l.spot.x, l.att.w, dot(-L, l.dir.xyz));
		sum += l.col.rgb * min(a, 1.0) * max(dot(N, L), 0.0);
	}
	return sum;
}

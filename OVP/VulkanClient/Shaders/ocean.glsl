// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The sea field near the camera (pass 2, W3; OceanFft.h): per layer N x N slopes (dh/du, dh/dv) along the
// anchor's t1, t2 axes, periodic, written by the CPU each frame. Requires atmosphere.glsl (ATM_SET, atm).
const int OCEAN_N = 128, OCEAN_MIPS = 5;
layout(set = ATM_SET, binding = 9) uniform sampler2D oceanLong;    // the long layer, its levels 0..OCEAN_MIPS
layout(set = ATM_SET, binding = 10) uniform sampler2D oceanRipple; // the ripples

float OceanMipVar (int m) { return atm.oceanMipVar[m / 4][m % 4]; }

// the long layer at patch coordinates u (1 per period) for pixels of fp texels of level 0: the
// levels about fp (trilinear); var out: its drawn mean-square slope
vec2 OceanLong (vec2 u, float fp, out float var)
{
	float lod = clamp (log2 (max (fp, 1e-3) / 0.6), 0.0, float (OCEAN_MIPS) + 1.0);
	float fadeOut = 1.0 - clamp (lod - float (OCEAN_MIPS), 0.0, 1.0); // (past the coarsest: the mean, 0)
	float lc = min (lod, float (OCEAN_MIPS));
	int l0 = int (floor (lc)), l1 = min (l0 + 1, OCEAN_MIPS);
	var = mix (OceanMipVar (l0), OceanMipVar (l1), lc - float (l0)) * fadeOut * fadeOut;
	return textureLod (oceanLong, u, lc).rg * fadeOut;
}

// The wind's sea's slope (planet frame) at p (relative to the camera); footprint: the pixel [m].
// The long layer twice (the second turned 37 deg and scaled 1.37, slopes turned back: its period not
// seen), through its levels as the pixels grow; the ripples once, fading as their texels shrink below
// 1.5-3 pixels; what is drawn of the variance comes out in drawnVar
vec3 OceanSlope (vec3 p, float footprint, out float drawnVar)
{
	vec3 P = p + atm.oceanD.xyz;
	vec2 q = vec2 (dot (P, atm.oceanT1.xyz), dot (P, atm.oceanT2.xyz));
	float L0 = atm.oceanT1.w, L1 = atm.oceanT2.w;
	float t0 = L0 / float (OCEAN_N), t1 = L1 / float (OCEAN_N);
	const float C = 0.79863551, S = 0.60181502, K = 1.37; // (cos, sin 37 deg)
	float va, vb;
	vec2 sa = OceanLong (q / L0, footprint / t0, va);
	vec2 sb = OceanLong (vec2 (C * q.x - S * q.y, S * q.x + C * q.y) * (K / L0) + vec2 (0.291, 0.638), footprint * K / t0, vb);
	sb = vec2 (C * sb.x + S * sb.y, -S * sb.x + C * sb.y); // (turned back: R^T)
	vec2 s = 0.70710678 * (sa + sb);
	float a1 = 1.0 - smoothstep (0.35, 0.7, footprint / t1);
	if (a1 > 0.0) s += a1 * textureLod (oceanRipple, q / L1, 0.0).rg;
	drawnVar = 0.5 * (va + vb) + a1 * a1 * atm.oceanVar.y;
	return s.x * atm.oceanT1.xyz + s.y * atm.oceanT2.xyz;
}

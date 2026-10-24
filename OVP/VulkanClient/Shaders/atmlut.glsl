// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Precomputed atmosphere tables (after Hillaire 2020, "A Scalable and
// Production Ready Sky and Atmosphere Rendering Technique"):
// - transmittance from a point (radius r) to the top of the atmosphere in
//   a direction with zenith cosine mu, with the mapping of Bruneton 2017
// - multiple scattering: luminance scattered isotropically by all orders
//   beyond the first, per unit sun irradiance, as a function of r and the
//   sun's zenith cosine
// Rg, Rt: planet and atmosphere radius [m]

const vec2 ATM_TRANSMITTANCE_SIZE = vec2 (256.0, 64.0);
const vec2 ATM_MULTISCAT_SIZE = vec2 (32.0, 32.0);

// texture coordinate of a value in [0,1], covering texel centres only
float AtmUnitToUv (float x, float n) { return 0.5 / n + x * (1.0 - 1.0 / n); }
float AtmUvToUnit (float u, float n) { return (u - 0.5 / n) / (1.0 - 1.0 / n); }

// distance to the top of the atmosphere
float AtmDistanceToTop (float r, float mu, float Rt)
{
	float disc = r * r * (mu * mu - 1.0) + Rt * Rt;
	return max (-r * mu + sqrt (max (disc, 0.0)), 0.0);
}

vec2 AtmTransmittanceUv (float r, float mu, float Rg, float Rt)
{
	float H = sqrt (max (Rt * Rt - Rg * Rg, 0.0));
	float rho = sqrt (max (r * r - Rg * Rg, 0.0));
	float d = AtmDistanceToTop (r, mu, Rt);
	float dMin = Rt - r, dMax = rho + H;
	float xMu = (d - dMin) / max (dMax - dMin, 1e-3);
	float xR = rho / H;
	return vec2 (AtmUnitToUv (clamp (xMu, 0.0, 1.0), ATM_TRANSMITTANCE_SIZE.x),
	             AtmUnitToUv (clamp (xR, 0.0, 1.0), ATM_TRANSMITTANCE_SIZE.y));
}

// inverse: r, mu of a texel
void AtmTransmittanceRMu (vec2 uv, float Rg, float Rt, out float r, out float mu)
{
	float xMu = AtmUvToUnit (uv.x, ATM_TRANSMITTANCE_SIZE.x);
	float xR = AtmUvToUnit (uv.y, ATM_TRANSMITTANCE_SIZE.y);
	float H = sqrt (max (Rt * Rt - Rg * Rg, 0.0));
	float rho = H * xR;
	r = sqrt (rho * rho + Rg * Rg);
	float dMin = Rt - r, dMax = rho + H;
	float d = dMin + xMu * (dMax - dMin);
	mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
	mu = clamp (mu, -1.0, 1.0);
}

vec2 AtmMultiscatUv (float r, float muS, float Rg, float Rt)
{
	return vec2 (AtmUnitToUv (clamp (muS * 0.5 + 0.5, 0.0, 1.0), ATM_MULTISCAT_SIZE.x),
	             AtmUnitToUv (clamp ((r - Rg) / (Rt - Rg), 0.0, 1.0), ATM_MULTISCAT_SIZE.y));
}

// densities (relative to sea level) of the Rayleigh, Mie and ozone layers
// at height h; Hr, Hm: scale heights, ozone in a tent around 25 km
vec3 AtmDensities (float h, float Hr, float Hm)
{
	h = max (h, 0.0);
	return vec3 (exp (-h / Hr), exp (-h / Hm), max (1.0 - abs (h - 25e3) / 15e3, 0.0));
}

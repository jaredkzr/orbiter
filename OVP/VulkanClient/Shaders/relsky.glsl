// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Relativistic view of the sky, in its spectral bands (galaxy.cfg BANDS:
// GalaxySky checks that they are these). Light of spectral radiance I(l)
// seen at Doppler factor D (observed over emitted frequency) arrives as
// D^5 I(l D). For a black body D^5 B(l D, T) = B(l, D T): a star of
// temperature T looks like one of D T, its flux (radiance times its solid
// angle, which shrinks as 1/D^2) in a band scaled accordingly. The diffuse
// sky's bands (each relative to sunlight's) are taken as a smooth
// reflectance of sunlight: linear between the bands' centres, held beyond
// (sunlight's own shape carries the spectrum on outside 380-720 nm).

#ifndef RELSKY_GLSL
#define RELSKY_GLSL

const vec4 REL_BAND_LO = vec4 (380.0, 460.0, 540.0, 620.0); // [nm]
const vec4 REL_BAND_HI = vec4 (460.0, 540.0, 620.0, 720.0);
const vec4 REL_BAND_C = 0.5 * (REL_BAND_LO + REL_BAND_HI);
const float REL_TSUN = 5772.0; // [K]
const float REL_V0 = 505.0, REL_V1 = 595.0; // V band (as GalaxySky's) [nm]

// log of a black body's spectral radiance (relative), wavelength l [nm],
// temperature T [K]: -5 log l - log (e^x - 1), x = hc / (l k T)
float RelLogPlanck (float l, float T)
{
	float um = l * 1e-3, x = 14387.77 / (um * T);
	float lx = x > 20.0 ? x : (x < 1e-3 ? log (x) : log (exp (x) - 1.0));
	return -5.0 * log (um) - lx;
}

// the factor on a star's V band flux seen at Doppler factor D (temperature T [K]):
// its V band light at D T over that at T, over D^2 (the star's solid angle)
float RelStarBoostV (float T, float D)
{
	float s0 = 0.0, s1 = 0.0;
	for (int k = 0; k < 4; k++) {
		float l = mix (REL_V0, REL_V1, (float (k) + 0.5) * 0.25);
		s0 += exp (RelLogPlanck (l, T) - RelLogPlanck (550.0, T));
		s1 += exp (RelLogPlanck (l, D * T) - RelLogPlanck (550.0, T));
	}
	return s1 / (s0 * D * D);
}

// extinction at wavelength l [nm] per A_V, from the bands' (ext): linear in
// 1/l through the bands' centres (as optical extinction curves nearly are),
// extended so beyond them
float RelExtinction (vec4 ext, float l)
{
	float x = 1.0 / l;
	vec4 xc = 1.0 / REL_BAND_C; // (decreasing)
	if (x >= xc.y) return max (ext.x + (ext.y - ext.x) * (x - xc.x) / (xc.y - xc.x), 0.0);
	if (x >= xc.z) return ext.y + (ext.z - ext.y) * (x - xc.y) / (xc.z - xc.y);
	return max (ext.z + (ext.w - ext.z) * (x - xc.z) / (xc.w - xc.z), 0.0);
}

// the diffuse sky: weights of the reflectance's values at the bands'
// centres at wavelength l [nm] (linear between them, held beyond)
vec4 RelBandBasis (float l)
{
	if (l <= REL_BAND_C.x) return vec4 (1.0, 0.0, 0.0, 0.0);
	if (l >= REL_BAND_C.w) return vec4 (0.0, 0.0, 0.0, 1.0);
	if (l < REL_BAND_C.y) { float t = (l - REL_BAND_C.x) / (REL_BAND_C.y - REL_BAND_C.x); return vec4 (1.0 - t, t, 0.0, 0.0); }
	if (l < REL_BAND_C.z) { float t = (l - REL_BAND_C.y) / (REL_BAND_C.z - REL_BAND_C.y); return vec4 (0.0, 1.0 - t, t, 0.0); }
	float t = (l - REL_BAND_C.z) / (REL_BAND_C.w - REL_BAND_C.z);
	return vec4 (0.0, 0.0, 1.0 - t, t);
}

// the bands seen (rows) from the reflectance's values (columns) at Doppler
// factor D, each band relative to sunlight's at rest
mat4 RelBandResponse (float D)
{
	const int NS = 8; // samples per band
	mat4 M = mat4 (0.0); // (M[j][i]: column j, row i)
	for (int i = 0; i < 4; i++) {
		float dl = (REL_BAND_HI[i] - REL_BAND_LO[i]) / float (NS), sun = 0.0;
		float ref = RelLogPlanck (REL_BAND_C[i], REL_TSUN); // (scale: within range)
		vec4 row = vec4 (0.0);
		for (int k = 0; k < NS; k++) {
			float l = REL_BAND_LO[i] + dl * (float (k) + 0.5);
			sun += exp (RelLogPlanck (l, REL_TSUN) - ref);
			row += RelBandBasis (l * D) * exp (RelLogPlanck (l, REL_TSUN * D) - ref);
		}
		row /= sun;
		for (int j = 0; j < 4; j++) M[j][i] = row[j];
	}
	return M;
}

// the diffuse sky's bands b (relative to sunlight's) seen at Doppler factor D
vec4 RelShiftBands (vec4 b, float D)
{
	if (abs (D - 1.0) < 1e-5) return b;
	vec4 rho = max (inverse (RelBandResponse (1.0)) * b, vec4 (0.0));
	return max (RelBandResponse (D) * rho, vec4 (0.0));
}

#endif

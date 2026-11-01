// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The extragalactic background light (cosmicbg.frag): its spectral radiance
// at rest, and as seen at Doppler factor D in the sky's bands (S10 per band,
// each relative to sunlight's). Needs relsky.glsl.

#ifndef COSMICBG_GLSL
#define COSMICBG_GLSL

const float S10_V = 1.19e-11; // [W m^-2 sr^-1 nm^-1] 1 S10 (V = 10 per square degree) at 550 nm

// log of the background's spectral radiance at rest [W m^-2 sr^-1 nm^-1],
// at wavelength l [nm]
float LogBackground (float l)
{
	float um = l * 1e-3, lum = log (um);
	// black body: B_lambda = 1.191e8 / um^5 / (e^x - 1) [W m^-2 sr^-1 um^-1], x = 14388 / (um T)
	float x = 14387.77 / (um * 2.7255);
	float lcmb = log (1.191e8) - 5.0 * lum - (x > 20.0 ? x : log (exp (x) - 1.0)) - log (1e3);
	float xd = 14387.77 / (um * 18.5);
	float lcib = log (1.3e-5) + 0.64 * log (100.0 / um) + log (1.191e8) - 5.0 * lum - (xd > 20.0 ? xd : log (exp (xd) - 1.0)) - log (1e3);
	// (nu I_nu [W m^-2 sr^-1] / lambda [nm])
	float lcob = log (11e-9) - 0.5 * lum * lum / (0.75 * 0.75) - log (l);
	// (the sum of three, in logs: their largest, and the others relative to it)
	float m = max (lcmb, max (lcib, lcob));
	return m + log (exp (lcmb - m) + exp (lcib - m) + exp (lcob - m));
}

// the mean spectral radiance seen in each band (D^5 I(l D)), relative to
// sunlight's in it, sunlight of 1 S10 in V
vec4 CosmicBands (float D)
{
	const int NS = 6;
	float lsunV = 0.0;
	for (int k = 0; k < 4; k++) {
		float l = mix (REL_V0, REL_V1, (float (k) + 0.5) * 0.25);
		lsunV += exp (RelLogPlanck (l, REL_TSUN) - RelLogPlanck (550.0, REL_TSUN));
	}
	lsunV *= 0.25;
	vec4 b;
	float lD = log (D);
	for (int i = 0; i < 4; i++) {
		float dl = (REL_BAND_HI[i] - REL_BAND_LO[i]) / float (NS), s = 0.0, sun = 0.0;
		for (int k = 0; k < NS; k++) {
			float l = REL_BAND_LO[i] + dl * (float (k) + 0.5);
			s += exp (5.0 * lD + LogBackground (l * D) - log (S10_V));
			sun += exp (RelLogPlanck (l, REL_TSUN) - RelLogPlanck (550.0, REL_TSUN));
		}
		b[i] = s / sun * lsunV;
	}
	return b;
}

#endif

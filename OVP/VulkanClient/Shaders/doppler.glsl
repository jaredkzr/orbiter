// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Relativistic Doppler shift of a linear RGB colour. Seen at Doppler factor
// D (observed over emitted frequency), light of spectral radiance I(lambda)
// arrives as D^5 I(lambda D): bluer and brighter ahead (D > 1), redder and
// dimmer behind. RGB does not hold a spectrum: the model takes the colour as
// a smooth reflectance (from the three channels, linear between their
// wavelengths, held beyond) times sunlight (a blackbody of the Sun's
// temperature), for which D^5 B(lambda D, T) = B(lambda, D T); the camera's
// channels are Gaussian filters (as the sky's camera colours). Exact for
// the Sun and anything lit by it with a flat reflectance outside the visible;
// an estimate for the rest (the reflectance in the infrared and ultraviolet
// is unknown). D = 1 gives the colour back unchanged.

#ifndef DOPPLER_GLSL
#define DOPPLER_GLSL

const int DOP_N = 16;                       // wavelengths sampled
const float DOP_L0 = 380.0, DOP_L1 = 720.0; // [nm]
const float DOP_TSUN = 5772.0;              // [K]

// blackbody spectral radiance (relative) at wavelength l [nm], temperature T [K]
float DopPlanck (float l, float T)
{
	float um = l * 1e-3;
	float x = min (14387.77 / (um * T), 80.0);
	float u5 = um * um * um * um * um;
	return 1.0 / (u5 * (exp (x) - 1.0));
}

// the camera's channels (Gaussian filters), l [nm]
vec3 DopFilter (float l)
{
	vec3 d = (vec3 (l) - vec3 (600.0, 545.0, 455.0)) / vec3 (45.0, 40.0, 35.0);
	return exp (-0.5 * d * d);
}

// interpolation of the reflectance between the channels' wavelengths
// (600, 545, 455 nm; held beyond): the weights of its three values at l [nm]
vec3 DopBasis (float l)
{
	if (l >= 600.0) return vec3 (1.0, 0.0, 0.0);
	if (l <= 455.0) return vec3 (0.0, 0.0, 1.0);
	if (l >= 545.0) { float t = (l - 545.0) / 55.0; return vec3 (t, 1.0 - t, 0.0); }
	float t = (l - 455.0) / 90.0;
	return vec3 (0.0, t, 1.0 - t);
}

// the camera's response (rows: channels) to sunlight seen at Doppler factor D
// reflected with the reflectance basis' values (columns)
mat3 DopResponse (float D)
{
	float dl = (DOP_L1 - DOP_L0) / float (DOP_N);
	mat3 M = mat3 (0.0);
	for (int k = 0; k < DOP_N; k++) {
		float l = DOP_L0 + dl * (float (k) + 0.5);
		M += outerProduct (DopFilter (l), DopBasis (l * D)) * DopPlanck (l, DOP_TSUN * D); // (row i: channel i, column j: basis j)
	}
	return M;
}

vec3 DopplerShiftRGB (vec3 rgb, float D)
{
	if (abs (D - 1.0) < 1e-5) return rgb;
	// the reflectance whose colour at rest is rgb (colours beyond the model's
	// reach: their reflectance held at 0)
	vec3 rho = max (inverse (DopResponse (1.0)) * rgb, vec3 (0.0));
	return max (DopResponse (D) * rho, vec3 (0.0));
}

#endif

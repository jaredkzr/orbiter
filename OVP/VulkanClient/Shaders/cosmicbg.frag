// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The extragalactic background light, seen moving: isotropic in the frame
// of the cosmic microwave background, invisible at rest (the optical part
// some hundredths of the darkest sky), but Doppler shifted and beamed ahead
// of a fast observer: light of wavelength l D at rest arrives at l, D^5
// times the spectral radiance (relsky.glsl). Three parts (Hill, Masui &
// Scott 2018, "The spectrum of the Universe"):
// - the cosmic microwave background: a black body at 2.7255 K (Fixsen 2009);
//   at D ~ 1000 it is one at 2700 K, a red star's surface over the sky ahead
// - the cosmic infrared background: a modified black body, tau (100 um /
//   l)^0.64 B(l, 18.5 K), tau 1.3e-5 (Fixsen et al. 1998)
// - the cosmic optical and ultraviolet background: a smooth bump, nu I_nu
//   11 nW m^-2 sr^-1 at 1 um, log-normal (sigma 0.75 in ln l) (galaxy counts
//   and gamma-ray attenuation, ~11 nW m^-2 sr^-1)
// The X-ray and gamma-ray backgrounds (behind: shifted down) are some
// hundred thousandths of the microwave's energy: nothing to see.
// Given in the sky's units: S10 per band, each relative to sunlight's
// (galaxy.glsl), added to the scene through the bands' colours.

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

#include "relativity.glsl"
#include "relsky.glsl"

layout(push_constant) uniform Params {
	vec4 beta;      // xyz: the camera's velocity relative to the background (view, units of c)
	vec4 proj;      // 1/fx, 1/fy
	vec4 colour[3]; // bands (S10) -> scene RGB (incl. the eye's adaptation)
} pc;

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

void main ()
{
	vec3 v = normalize (vec3 (vNdc.x * pc.proj.x, -vNdc.y * pc.proj.y, 1.0));
	float D = RelDopplerSeen (v, pc.beta.xyz);
	// each band: the mean spectral radiance seen (D^5 I(l D)), relative to
	// sunlight's in it, sunlight of 1 S10 in V
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
	vec3 c = max (vec3 (dot (pc.colour[0], b), dot (pc.colour[1], b), dot (pc.colour[2], b)), vec3 (0.0));
	outColor = vec4 (min (c, vec3 (65000.0)), 0.0);
}

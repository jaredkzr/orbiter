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
#include "cosmicbg.glsl"

layout(push_constant) uniform Params {
	vec4 beta;      // xyz: the camera's velocity relative to the background (view, units of c)
	vec4 proj;      // 1/fx, 1/fy
	vec4 colour[3]; // bands (S10) -> scene RGB (incl. the eye's adaptation)
} pc;

void main ()
{
	vec3 v = normalize (vec3 (vNdc.x * pc.proj.x, -vNdc.y * pc.proj.y, 1.0));
	vec4 b = CosmicBands (RelDopplerSeen (v, pc.beta.xyz));
	vec3 c = max (vec3 (dot (pc.colour[0], b), dot (pc.colour[1], b), dot (pc.colour[2], b)), vec3 (0.0));
	outColor = vec4 (min (c, vec3 (65000.0)), 0.0);
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: the sunlight reaching each particle: through the
// atmosphere, in the planet's shadow, through the smoke between it and the
// sun (shadow grid), and (with ray queries) in the shadows of vessels and
// bases (included by particle_light.comp and particle_light_rt.comp)

layout(local_size_x = 64) in;

#include "particle_common.glsl"

void main ()
{
	uint i = gl_GlobalInvocationID.x;
	if (i >= uint (pc.omega.w)) return;
	Particle p = particles[i];
	if (p.velLife.w <= 0.0 || (p.flags & 4u) != 0u) return; // (none, or emissive)
	vec3 pos = p.posAge.xyz;
	mat3 toView = transpose (mat3 (frame.viewToGlobal));
	vec3 vp = toView * pos;
	vec3 sun = frame.sunColor.rgb;
	if (atm.misc.x > 0.5) {
		vec3 pp = mat3 (atm.viewToPlanet) * vp + atm.camPos.xyz;
		sun *= SunTransmittance (pp) * CloudShadow (pp);
	} else if (pc.bodyPos.w > 0.0) {
		// airless: the planet's shadow (a cylinder behind it)
		vec3 d = pos - pc.bodyPos.xyz, s = mat3 (frame.viewToGlobal) * frame.sunDir.xyz;
		float along = dot (d, s);
		if (along < 0.0) sun *= smoothstep (0.995 * pc.bodyPos.w, 1.005 * pc.bodyPos.w, length (d - s * along));
	}
	// smoke towards the sun (the grid holds the optical depth from each
	// cell towards the sun, half of the cell's own included)
	vec3 sc = ShadowCoord (pos);
	// (light scattered many times inside dense smoke still gets through:
	// a strong attenuation and a weak one, after cloud renderers)
	if (all (greaterThan (sc, vec3 (0.0))) && all (lessThan (sc, vec3 (1.0)))) {
		float tau = textureLod (shadowDepthTex, sc, 0.0).r;
		sun *= 0.55 * exp (-tau) + 0.45 * exp (-0.08 * tau);
	}
#ifdef RT
	sun *= RTSunVisible (vp + frame.sunDir.xyz * 0.5, frame.sunDir.xyz);
#endif
	particles[i].light = sun;
}

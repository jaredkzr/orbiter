// Copyright (c) Martin Schweiger
// Licensed under the MIT License

const float PI = 3.14159265;

// Microfacet specular reflection (GGX distribution, Smith-Schlick
// visibility, Schlick Fresnel), times N.L
vec3 SpecularBRDF (vec3 N, vec3 V, vec3 L, vec3 F0, float rough)
{
	vec3 H = normalize (V + L);
	float NoL = max (dot (N, L), 0.0), NoV = max (dot (N, V), 1e-4);
	float NoH = max (dot (N, H), 0.0), VoH = max (dot (V, H), 0.0);
	float a = rough * rough, a2 = a * a;
	float d = NoH * NoH * (a2 - 1.0) + 1.0;
	float D = a2 / (PI * d * d);
	float k = (rough + 1.0) * (rough + 1.0) / 8.0;
	float G = NoV / (NoV * (1.0 - k) + k) * NoL / (NoL * (1.0 - k) + k);
	vec3 F = F0 + (1.0 - F0) * pow (1.0 - VoH, 5.0);
	return D * G * F / (4.0 * NoV * NoL + 1e-4) * NoL;
}

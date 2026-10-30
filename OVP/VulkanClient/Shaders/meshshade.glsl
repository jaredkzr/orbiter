// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Lighting of opaque mesh surfaces (mesh.frag, building.frag): include
// after the Draw push constants, vPosView, envProbe, brdf.glsl and
// material.glsl

// Analytic approximation of the pre-integrated environment BRDF (Karis)
vec3 EnvBRDF (vec3 F0, float rough, float NoV)
{
	const vec4 c0 = vec4 (-1.0, -0.0275, -0.572, 0.022);
	const vec4 c1 = vec4 (1.0, 0.0425, 1.04, -0.04);
	vec4 r = rough * c0 + c1;
	float a004 = min (r.x * r.x, exp2 (-9.28 * NoV)) * r.x + r.y;
	vec2 AB = vec2 (-1.04, 1.04) * a004 + r.zw;
	return F0 * AB.x + AB.y;
}

// Light arriving from direction d (planet frame): the sky of the planet near
// the camera, or its sunlit surface
vec3 Environment (vec3 d)
{
	if (atm.radii.x <= 0.0) return vec3 (0.0);
	vec3 o = atm.camPos.xyz;
	vec2 tg = RaySphereC (o, d, atm.camC.x);
	float tMax = ATM_NONE;
	vec3 col = vec3 (0.0);
	if (tg.x > 0.0 && tg.x <= tg.y) {
		tMax = tg.x;
		col = AtmGround (normalize (o + d * tg.x)) * frame.sunColor.rgb;
	}
	if (atm.misc.x > 0.5) {
		vec3 T;
		vec3 L = Inscatter (d, tMax, T);
		col = col * T + AtmTonemap (L);
	}
	return col;
}

// share of the ambient light (buildings on the night side: the terrain's)
float ambientShare = 1.0;

// Light reflected by a surface of albedo base at vPosView: sun (shadows,
// eclipses, extinction), local lights, ambient, planet light, environment
// reflections. sun receives the sunlight reaching it; debug is set (and the
// debug view returned) when the reflections' debug view is on.
vec3 SurfaceLight (vec3 base, Material mat, out vec3 sun, out bool debug)
{
	debug = false;
	vec3 N = mat.N;
	vec3 V = normalize(-vPosView);
	vec3 L = frame.sunDir.xyz;
	float NoL = max(dot(N, L), 0.0);
	float NoV = max(dot(N, V), 1e-4);
	float metal = mat.metal, rough = mat.rough, reflectivity = mat.reflectivity;
	vec3 F0 = mix(vec3(0.04 * draw.pbr.w) * mat.specTint, base, metal);
	vec3 reflTint = mat.reflTint;

	// sunlight: eclipses, shadows, and extinction in the atmosphere of the
	// planet near the camera
	bool hasAtm = atm.misc.x > 0.5;
	// ray traced visibility for prepass geometry, else the shadow maps
	bool rtVis = RTLighting() && draw.maps.y < 0.5;
	vec4 rv = rtVis ? RTVisibility() : vec4(1.0);
	vec4 lv = rtVis ? RTLightVisibility() : vec4(1.0);
	sun = frame.sunColor.rgb * draw.light.rgb * (rtVis ? rv.x : SunShadow(vPosView, N));
	if (hasAtm) {
		vec3 pPlanet = mat3(atm.viewToPlanet) * vPosView + atm.camPos.xyz;
		sun *= SunTransmittance(pPlanet) * CloudShadow(pPlanet);
	}

	// diffuse (Orbiter's convention: albedo times N.L) and specular reflection
	// of the sun, local lights, ambient, and reflections of the environment
	vec3 diffuse = base * (1.0 - metal);
	float ao = draw.maps.y > 0.5 ? 1.0 : AmbientOcclusion();
	float aoDirect = mix(1.0, ao, frame.aoParams.z); // contact darkening in direct light too
	vec3 col = diffuse * (frame.ambient.rgb * ao * ambientShare + (sun * NoL + LocalLighting(vPosView, N, lv)) * aoDirect);
	col += diffuse * PlanetLight(N) * rv.z; // sunlight reflected by the planet nearby
	col += sun * SpecularBRDF(N, V, L, F0, rough) * PI;
	if (reflectivity > 0.0) {
		vec3 R = reflect(-V, N);
		R = normalize(mix(R, N, rough * rough)); // rough surfaces: towards the normal
		vec3 env = frame.probe.x > 0.5
			? textureLod(envProbe, mat3(frame.viewToGlobal) * R, rough * frame.probe.y).rgb
			: Environment(normalize(mat3(atm.viewToPlanet) * R));
		env *= ao;
		// screen-space reflections where they found the reflected surface
		// (prepass geometry only)
		if (draw.maps.y < 0.5) {
			vec4 s = ScreenReflection(vec2(0.0));
			env = env * (1.0 - s.a) + s.rgb;
			if (frame.probe.w > 0.5) { // debug view: reflections, blue where none
				debug = true;
				return s.rgb + vec3(0.0, 0.0, 0.3) * (1.0 - s.a);
			}
		}
		col += env * EnvBRDF(F0, rough, NoV) * reflectivity * reflTint;
	}
	return col;
}

// Aerial perspective of the atmosphere of the planet near the camera
vec3 AerialPerspective (vec3 col)
{
	if (atm.misc.x <= 0.5) return col;
	vec3 T;
	vec3 Lin = Inscatter(normalize(mat3(atm.viewToPlanet) * vPosView), length(vPosView), T);
	return col * T + AtmTonemap(Lin);
}

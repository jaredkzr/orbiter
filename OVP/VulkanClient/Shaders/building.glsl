// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Buildings of the procedural cities (CityKit): shapes of a few vertices,
// stretched to each building's size by the instance (vertex and instance
// layout: CityKit.h)

layout(location = 0) in vec3 inUnit;   // x, z: share of width, depth (-0.5 to 0.5); y: share of the walls' height
layout(location = 1) in vec3 inMetric; // offset [m]
layout(location = 2) in vec4 inShape;  // x: share of the roof's height, y: x offset in shares of the depth (hip ridges), z: part, w: facade's share of the side
layout(location = 3) in vec3 inNormal; // building frame (0: the surface's, from derivatives)
layout(location = 4) in vec4 inst0;    // rows of the building's frame (unscaled) and position:
layout(location = 5) in vec4 inst1;    //   building frame -> frame of the transforms
layout(location = 6) in vec4 inst2;
layout(location = 7) in vec4 instDim;  // width, depth, walls' height (from the base), roof height [m]

/// position in the building frame [m] (x along its front, y up from its base)
vec3 BuildingPos ()
{
	return vec3(inUnit.x * instDim.x + inShape.y * instDim.y,
	            inUnit.y * instDim.z + inShape.x * instDim.w,
	            inUnit.z * instDim.y) + inMetric;
}

mat4 BuildingFrame ()
{
	return transpose(mat4(inst0, inst1, inst2, vec4(0.0, 0.0, 0.0, 1.0)));
}

/// the hand-off to the night light texture by the camera's altitude above
/// the ground alt [m] (CityField FADE_LO, FADE_HI, TALL_HI; tile_main.glsl
/// CityHandOff): this building's share, 1 drawn, 0 gone (the buildings
/// under 35 m out over 3 to 6 km, the taller by 8 km, from 5)
float BuildingAltFade (float alt)
{
	float h = instDim.z + instDim.w;
	return 1.0 - (h < 35.0 ? smoothstep(3000.0, 6000.0, alt) : smoothstep(5000.0, 8000.0, alt));
}

/// passes without the colour pass's dithered fade (the prepass, shadow
/// maps): a building is drawn while its fade exceeds its own threshold
/// (stable, from its position), so that across the band they go one by one
bool BuildingFadedOut (float fade)
{
	if (fade >= 1.0) return false;
	vec3 o = vec3(inst0.w, inst1.w, inst2.w);
	float cut = fract(sin(dot(floor(o), vec3(12.9898, 78.233, 37.719))) * 43758.5453);
	return fade <= cut;
}

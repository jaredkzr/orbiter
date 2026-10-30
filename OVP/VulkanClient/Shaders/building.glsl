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

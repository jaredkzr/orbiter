// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// A cell of the lens map (lens_trace.comp) between four corners: smooth
// (the pixels in it interpolated from the corners) when they all see one
// image of the sky (directions within COHERENT) or of the disc (radii
// within 5%), or all the shadow. Elsewhere (the edges of images, of the
// shadow and of the disc, the thin images wound about the photon orbit)
// the cell's pixels are traced again, each (the refining pass: every
// pixel's corners).

const float LENS_COHERENT = 0.99966; // cos 1.5 deg

bool LensCellSmooth (vec4 c00, vec4 c10, vec4 c01, vec4 c11)
{
	if (c00.w == 0.0 && c10.w == 0.0 && c01.w == 0.0 && c11.w == 0.0) return true;
	if (c00.w > 0.0 && c10.w > 0.0 && c01.w > 0.0 && c11.w > 0.0)
		return min (min (dot (c00.xyz, c10.xyz), dot (c00.xyz, c01.xyz)), dot (c00.xyz, c11.xyz)) > LENS_COHERENT;
	if (c00.w < 0.0 && c10.w < 0.0 && c01.w < 0.0 && c11.w < 0.0)
		return max (max (abs (c00.z - c10.z), abs (c00.z - c01.z)), abs (c00.z - c11.z)) < 0.05 * c00.z;
	return false;
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The surface tiles' compact vertices (PlanetSurface::Pack): the normal's
// octahedral encoding, the texture coordinates of a vertex from its place
// in the tile's grid (G quads a side, then the skirts of the north, south,
// west and east edges)

const uint TILE_G = 64u, TILE_V = TILE_G + 1u; // (TileRenderer::GRID)

vec3 TileOctDecode (vec2 e)
{
	vec3 n = vec3 (e, 1.0 - abs (e.x) - abs (e.y));
	if (n.z < 0.0) n.xy = (1.0 - abs (n.yx)) * vec2 (n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return normalize (n);
}

vec2 OctEncode (vec3 n)
{
	n /= abs (n.x) + abs (n.y) + abs (n.z);
	vec2 e = n.xy;
	if (n.z < 0.0) e = (1.0 - abs (n.yx)) * vec2 (n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return e;
}

vec2 TileGridUV (uint v)
{
	if (v < TILE_V * TILE_V) return vec2 (float (v % TILE_V), float (v / TILE_V)) / float (TILE_G);
	uint k = v - TILE_V * TILE_V, e = k / TILE_V, m = k % TILE_V;
	uvec2 ij = e == 0u ? uvec2 (0u, m) : e == 1u ? uvec2 (TILE_G, m) : e == 2u ? uvec2 (m, 0u) : uvec2 (m, TILE_G);
	return vec2 (float (ij.y), float (ij.x)) / float (TILE_G);
}

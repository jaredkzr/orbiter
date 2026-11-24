// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Vertices of the ray tracing scene's geometries: NTVERTEX (8 floats), or a
// terrain tile's compact vertex (RT_COMPACT, 6 words: PlanetSurface::Pack):
// position and morph target snorm16 (in the tile's scale: the geometry's
// transform), octahedral normal snorm16, detail halves; the texture
// coordinates from the vertex's place in the tile's grid (and its skirts).
// (needs RTFloats, RTUints)

const uint RT_COMPACT = 4u;
const uint RT_TILE_G = 64u, RT_TILE_V = RT_TILE_G + 1u; // (TileRenderer::GRID)

vec3 RTOctDecode (vec2 e)
{
	vec3 n = vec3 (e, 1.0 - abs (e.x) - abs (e.y));
	if (n.z < 0.0) n.xy = (1.0 - abs (n.yx)) * vec2 (n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return normalize (n);
}

vec2 RTTileUV (uint v)
{
	const uint V = RT_TILE_V, G = RT_TILE_G;
	if (v < V * V) return vec2 (float (v % V), float (v / V)) / float (G);
	uint k = v - V * V, e = k / V, m = k % V;
	uvec2 ij = e == 0u ? uvec2 (0u, m) : e == 1u ? uvec2 (G, m) : e == 2u ? uvec2 (m, 0u) : uvec2 (m, G);
	return vec2 (float (ij.y), float (ij.x)) / float (G);
}

vec3 RTVertexPos (uvec2 vtx, uint flags, uint v)
{
	if ((flags & RT_COMPACT) != 0u) {
		RTUints u = RTUints (vtx);
		return vec3 (unpackSnorm2x16 (u.u[v * 6u]), unpackSnorm2x16 (u.u[v * 6u + 1u]).x);
	}
	RTFloats f = RTFloats (vtx);
	return vec3 (f.f[v * 8u], f.f[v * 8u + 1u], f.f[v * 8u + 2u]);
}

vec3 RTVertexNormal (uvec2 vtx, uint flags, uint v)
{
	if ((flags & RT_COMPACT) != 0u) return RTOctDecode (unpackSnorm2x16 (RTUints (vtx).u[v * 6u + 4u]));
	RTFloats f = RTFloats (vtx);
	return vec3 (f.f[v * 8u + 3u], f.f[v * 8u + 4u], f.f[v * 8u + 5u]);
}

vec2 RTVertexUV (uvec2 vtx, uint flags, uint v)
{
	if ((flags & RT_COMPACT) != 0u) return RTTileUV (v);
	RTFloats f = RTFloats (vtx);
	return vec2 (f.f[v * 8u + 6u], f.f[v * 8u + 7u]);
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planet surface tiles, with the procedural terrain detail finer than the
// meshes (terraindetail.glsl)

#version 450
#extension GL_GOOGLE_include_directive : require

#define TERRAIN_DETAIL
#include "tile_main.glsl"

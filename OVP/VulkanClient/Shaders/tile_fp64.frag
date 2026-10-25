// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planet surface tiles, with the procedural terrain detail finer than the
// meshes (terraindetail.glsl; needs double precision)

#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_gpu_shader_fp64 : require

#define TERRAIN_DETAIL
#include "tile_main.glsl"

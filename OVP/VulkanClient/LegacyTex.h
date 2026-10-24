// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Planet textures in the legacy .tex format: a sequence of DDS images,
// one per patch, from the whole-sphere levels 1-3 to the patches of the
// higher levels. Used for bodies without tiled surfaces and for rings.
// =======================================================================

#ifndef __LEGACYTEX_H
#define __LEGACYTEX_H

#include <cstdint>
#include <string>
#include <vector>

namespace legacytex {

/// Read the images of a .tex file
bool Read (const std::string &path, std::vector<std::vector<uint8_t>> &images);

/// Decode a DXT1/DXT3/DXT5 image (top mip level) to RGBA8
bool Decode (const std::vector<uint8_t> &dds, uint32_t &w, uint32_t &h, std::vector<uint8_t> &rgba);

/// Whole-sphere map (longitude -180..180 left to right, north at the top)
/// of the highest available level up to 4 (level 4: the two hemispheres
/// side by side)
bool GlobalMap (const std::vector<std::vector<uint8_t>> &images, uint32_t &w, uint32_t &h, std::vector<uint8_t> &rgba);

} // namespace legacytex

#endif // !__LEGACYTEX_H

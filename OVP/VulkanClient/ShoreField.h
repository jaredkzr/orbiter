// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Shore distance field of the mask tiles (pass 2, F1).
//
// A mask tile's alpha says land (1) or water (0): Earth's are DXT1 with
// 1-bit alpha, the procedural worlds' RGBA8 with the water's share of the
// texel. The tile shader needs the distance from the water's edge for the
// surf band, the wet sand and the shallows, and a 2 x 2 texel look-up gives
// it only within about a texel (a hairline on fine coasts, whose texels are
// 1-10 m). So when a mask tile is loaded (on the loader thread), its alpha
// is replaced by the signed distance to the edge (the 0.5 contour), in
// texels, positive on land:
//
//   alpha = 0.5 + sd / (2 SD_MAX), clamped to 0..1
//
// The edge stays at alpha 0.5 (every "water: a < 0.5" test holds), and the
// bilinear filter of a distance field is a good distance between the texels.
// Distances are within the tile (an edge in the neighbouring tile is not
// seen: near the tile's border the field may read farther than it is).
// Block-compressed tiles become BC3 (DXT5: the colour re-encoded, the field
// in the interpolated alpha), RGBA8 tiles keep their format.
// =======================================================================

#ifndef __SHOREFIELD_H
#define __SHOREFIELD_H

#include <cstdint>
#include <vector>

namespace vkshore {

constexpr float SD_MAX = 32.0f; ///< texels on either side of the edge (tile_main.glsl SHORE_SD_MAX)

/// the field is made (ORBITER_VK_SHORE_FIELD=0: masks as they are; the shader then reads them as before)
bool Enabled ();

/// replace a mask tile's alpha (a DDS file in memory) by the signed distance field; false if the
/// format is not handled (the tile is then left as it was)
bool EncodeShoreField (std::vector<uint8_t> &dds);

}

#endif // !__SHOREFIELD_H

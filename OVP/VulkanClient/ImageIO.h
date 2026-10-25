// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Reading and writing image files (PNG)
// =======================================================================

#ifndef __IMAGEIO_H
#define __IMAGEIO_H

#include <cstdint>
#include <string>
#include <vector>

namespace image {

	/// Read a PNG file into 8-bit RGBA pixels (rows top to bottom).
	/// Supports 8-bit greyscale, greyscale+alpha, RGB, RGBA and palette images (non-interlaced).
	bool ReadPNG (const std::string &path, uint32_t &w, uint32_t &h, std::vector<uint8_t> &rgba);

	/// Write 8-bit RGB pixels (rows top to bottom) as a PNG file
	/// fast: quick compression (larger files), e.g. for video frames
	bool WritePNG (const std::string &path, uint32_t w, uint32_t h, const uint8_t *rgb, bool fast = false);

} // namespace image

#endif // !__IMAGEIO_H

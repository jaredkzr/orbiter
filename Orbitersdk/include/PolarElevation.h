// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Elevation tiles towards the poles. The tiles' nodes are equal steps of
// longitude and latitude: towards a pole a row's nodes close up to cos(lat)
// of the rows' spacing, and the data (reprojected from polar maps) differ
// from node to node along a row by noise of their own, which became ridges
// along the meridians (slopes of tens of degrees, at the Moon's south pole).
//
// Each row is resampled at every m-th node (m: powers of two about
// 0.5/cos(lat), the two nearest blended), linearly in between: nodes about as far apart along the rows
// as across them. The nodes kept are multiples of m in each tile, whose
// edges (node 0 and 256) are too: no seams between tiles.
//
// For the core (ElevationManager) and the graphics clients alike: the same
// ground for the vessels as on screen.
// =======================================================================

#ifndef __POLARELEVATION_H
#define __POLARELEVATION_H

#include <cmath>
#include <type_traits>
#include <vector>

namespace oapi {

	/// Resample the rows of an elevation tile near the poles (above).
	/// h: the tile's nodes, stride x stride (a border of one node before and
	/// two after the 256 intervals: stride 259), rows south to north;
	/// lvl, ilat: the tile's level (4: the hemispheres) and latitude index.
	template<typename T>
	void PolarElevationRows (T *h, int stride, int lvl, int ilat)
	{
		const double PI = 3.14159265358979323846;
		const int G = stride - 3;                  // intervals (256)
		const int nlat = 1 << (lvl - 4);
		const double latmax = PI * (0.5 - (double)ilat / nlat);
		const double dlat = PI / nlat / G;
		const double latmin = latmax - dlat * G;
		std::vector<double> row (stride);
		for (int r = 0; r < stride; r++) {
			double c = std::cos (latmin + (r - 1) * dlat);
			if (c >= 0.5) continue;
			// (spacing level: 0 at 60 degrees; between powers of two the rows
			// resampled at both, blended, so that the rows change gradually)
			double L = std::log2 (1.0 / (c > 1e-6 ? c : 1e-6)) - 1.0;
			int l0 = (int)std::floor (L);
			double f = L - l0;
			int m0 = 1 << (l0 < 0 ? 0 : l0 > 8 ? 8 : l0), m1 = m0 * 2 > G ? G : m0 * 2;
			T *p = h + (size_t)r * stride;
			for (int k = 0; k < stride; k++) row[k] = (double)p[k];
			auto at = [&](int j) { // (node j: -1 to G+1)
				int k = j + 1;
				return row[k < 0 ? 0 : k >= stride ? stride - 1 : k];
			};
			auto dec = [&](int j, int m) { // (row at every m-th node, linear between)
				if (m < 2) return at (j);
				int j0 = (int)std::floor ((double)j / m) * m;
				double t = (double)(j - j0) / m;
				return at (j0) * (1.0 - t) + at (j0 + m) * t;
			};
			for (int k = 0; k < stride; k++) {
				double v = dec (k - 1, m0) * (1.0 - f) + dec (k - 1, m1) * f;
				if (std::is_integral<T>::value) p[k] = (T)std::lround (v);
				else p[k] = (T)v;
			}
		}
	}

} // namespace oapi

#endif // !__POLARELEVATION_H

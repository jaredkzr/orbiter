// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The swell of a planet's sea: the waves the physics feels (the long end of
// the wind's sea, down to a few metres), shared by the core (buoyancy,
// water entry: Sea.h) and the graphics client (the sea's surface drawn
// displaced by it). Linear (Airy) deep-water waves from a Pierson-Moskowitz
// spectrum of the wind 10 m above the water, spread about its direction
// (cos^2), discretised into a few dozen components.
//
// Deterministic: a function of the planet-fixed position and the time
// alone, so every vessel, a replay and a reloaded scenario see the same
// sea. The sphere is divided into anchor cells (1 deg of latitude, rows of
// about 1 deg of longitude); each cell has its own components (seeded by the
// planet's seed and the cell) in the tangent plane at its centre, and the
// cells are cross-faded near their borders (the sum normalised by the
// root of the summed squared weights: the variance kept).
//
// Frames: points are unit vectors in the planet frame (y north, longitude
// from x towards z); results are in the local horizon frame of the point
// (x east, y up, z north), as Vessel's L2H.
// =======================================================================

#ifndef __SEASWELL_H
#define __SEASWELL_H

#include "Vecmat.h"
#include <cstdint>
#include <vector>

namespace sea {

	struct SwellParams {
		double wind = 0.0;       ///< [m/s] wind 10 m above the water (0-25)
		double bearing = 0.0;    ///< [rad] direction the wind (and the waves) go towards, clockwise from north
		double g = 9.81;         ///< [m/s^2] surface gravity
		double radius = 6.371e6; ///< [m] the planet's mean radius
		uint32_t seed = 1;
		double lambdaMin = 4.0;  ///< [m] the shortest wave of the swell (shorter: the client's alone)
	};

	/// The surface at a point
	struct SwellSample {
		double h = 0.0;          ///< [m] height above the mean level
		double sx = 0.0, sz = 0.0; ///< slopes dh/d(east), dh/d(north)
		Vector vel;              ///< [m/s] the water's orbital velocity (x east, y up, z north) at the depth asked
	};

	class Swell {
	public:
		static constexpr int NFREQ = 16;  ///< frequency bins
		static constexpr int NDIR = 3;    ///< directions per bin
		static constexpr double CELL_DEG = 1.0;
		static constexpr double BLEND = 0.1; ///< of a cell: the cross-fade on either side of a border

		void Setup (const SwellParams &prm);
		const SwellParams &Prm () const { return prm; }

		/// no waves of the swell's band (glassy, or the wind's sea all shorter than lambdaMin)
		bool Calm () const { return calm; }
		/// [m] significant wave height of the swell as discretised (4 sqrt (variance))
		double Hs () const { return hs; }
		/// [m] significant wave height of the whole Pierson-Moskowitz sea of the wind
		double HsFull () const { return hsFull; }
		/// [s] peak period
		double Tp () const { return tp; }
		/// [m] bound of the height (the sum of the amplitudes)
		double CrestMax () const { return crestMax; }
		/// [m] the longest wave
		double LambdaMax () const { return lambdaMax; }

		/// The surface over the point at unit vector u (planet frame) at time t [s];
		/// the orbital velocity at depth z [m] below the mean level (z <= 0;
		/// above it: as at the mean level). Thread-safe.
		void Sample (const Vector &u, double t, double z, SwellSample &s) const;
		/// height only (cheaper)
		double Height (const Vector &u, double t) const;

		/// The components about a reference point (unit vector u, planet frame)
		/// at time t, for evaluating the surface near it (a few km: the graphics
		/// client): the height at a point x east, z north [m] of it (its tangent
		/// plane) is sum a cos (k (dx x + dz z) + phase). The cells' weights at
		/// the reference are in the amplitudes; the phases include the time.
		struct Comp2 { double a, k, dx, dz, phase; };
		void Components (const Vector &u, double t, std::vector<Comp2> &out) const;

	private:
		struct Comp {
			double a, k, w;    // amplitude [m], wavenumber [1/m], angular frequency [1/s]
			double dx, dz;     // unit direction of travel (cell frame: east, north)
			double phase;
		};
		struct Cell {
			Vector e, n;       // the tangent frame at the centre (planet frame, unit): east, north
			std::vector<Comp> comp;
		};
		const Cell &GetCell (int row, int col) const;
		template<bool FULL> void Accumulate (const Cell &c, double w, const Vector &u, double t, double z, SwellSample &s) const;
		template<bool FULL> void Eval (const Vector &u, double t, double z, SwellSample &s) const;
		void Rows (double lat, int &r0, int &r1, double &w1) const;
		void Cols (int row, double lng, int &c0, int &c1, double &w1) const;
		int RowCols (int row) const;

		SwellParams prm;
		bool calm = true;
		double hs = 0.0, hsFull = 0.0, tp = 0.0, crestMax = 0.0, lambdaMax = 0.0;
		// the spectrum's bins (shared by all cells; each cell jitters within them)
		double wlo = 0.0, whi = 0.0;
		struct Bin { double w0, w1, S; };
		std::vector<Bin> bins;
		int nrows = 180;
		// (cells made on demand; a small cache, locked)
		struct Entry { int row, col; Cell cell; uint64_t used; };
		mutable std::vector<Entry> cache;
		mutable uint64_t clock = 0;
	};

	/// smooth step 0..1 between a and b
	inline double SmoothStep (double a, double b, double x)
	{
		double t = (x - a) / (b - a);
		t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
		return t * t * (3.0 - 2.0 * t);
	}

} // namespace sea

#endif // !__SEASWELL_H

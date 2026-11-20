// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// GroundFilter: the ground under a surface model's trees and buildings.
//
// Earth's elevation data are surface models: they hold the tops of the
// trees and buildings, not the ground. A ground filter estimates the
// ground from the data alone: at nodes on a regular latitude/longitude
// grid (step), a low percentile of n x n samples spacing metres apart
// about the node (the sea's samples, |h| <= 0.25 m, left out),
// interpolated between the nodes; NaN where too few samples are land. On
// slopes the percentile of the heights over the samples' plane, at steps
// that of the samples on the node's side (Params).
//
// Nodes are made in blocks, on demand, in parallel, by elevation managers
// of the filter's own (the data alone: no detail, no levelling), and
// cached; any thread may ask.
//
// Users: the cities (CityMap::Ground: built-up ground levelled to it) and
// the terrain detail (bases with BAREEARTH: their surroundings levelled to
// it).
// =======================================================================

#ifndef __GROUNDFILTER_H
#define __GROUNDFILTER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

class Planet;
class ElevationManager;
struct ElevationTile;

class GroundFilter {
public:
	struct Params {
		double step;       ///< node spacing [rad]
		double spacing;    ///< the samples' spacing about a node [m]
		int n;             ///< samples per side (odd: n x n about the node)
		double percentile; ///< the ground: this fraction of the land samples lie below it (0-1)
		int minLand;       ///< land samples a node needs (fewer: NaN)
		/// Slopes: on sloping ground the low percentile of the samples is the
		/// foot of the slope, not the ground under the trees (hills and
		/// plateaus were cut down by 10 m and more). Where the samples' plane
		/// (least squares) slopes more than slope0, the percentile is taken
		/// of their heights over that plane instead, and set on the plane at
		/// the node - in full from slope1 (rise over run; 0, 0: never)
		double slope0 = 0.0, slope1 = 0.0;
		/// Steps (cliffs, banks): where the samples scatter about their plane
		/// by more than step0 [m, rms] (a step is no plane: its upper edge
		/// came out metres low), the ground of the half of them on one side
		/// of the node that scatters least is taken, if that is much less -
		/// in full from step1 (0: never; needs slope1 > 0)
		double step0 = 0.0, step1 = 0.0;
	};

	GroundFilter (const Planet *planet, const Params &prm);
	~GroundFilter ();

	/// The ground at a point [m] (any thread); NaN where there is none.
	/// land: the share of the nodes' samples that are land (0-1), interpolated
	double Ground (double lat, double lng, double *land = nullptr);

	const Params &Prm () const { return prm; }

private:
	const Planet *planet;
	const double R;
	const Params prm;
	const uint64_t serial; // (this filter, for the threads' caches of its blocks: not its address, reused)

	static const int GBLOCK = 8; // nodes per block side
	struct NodeVal { float g, land; }; // the ground [m] (NaN: none), the share of land samples
	typedef std::array<NodeVal, GBLOCK * GBLOCK> Block;
	std::shared_mutex mtx;
	std::unordered_map<int64_t, std::shared_ptr<const Block>> blocks;
	// (blocks being made: other threads needing one wait for it)
	std::unordered_map<int64_t, std::shared_future<std::shared_ptr<const Block>>> pending;
	std::atomic<int> making{ 0 }; // blocks being made

	struct RawElev;
	std::vector<std::unique_ptr<RawElev>> rawElev;
	std::mutex rawMtx;

	NodeVal Node (int64_t i, int64_t j);
	std::shared_ptr<const Block> MakeBlock (int64_t bi, int64_t bj);
	static int64_t NodeKey (int64_t i, int64_t j) { return i * 1000003 + j; }
};

#endif // !__GROUNDFILTER_H

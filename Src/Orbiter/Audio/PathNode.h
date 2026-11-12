// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The ways a sound takes to the listener, as a node of the audio graph:
// one mono input (the source), two stereo outputs:
//
//   0: through the air  (to the Outside bus)
//   1: through the hull (to the Cockpit bus: the listener in the vessel
//      the source is on hears it through the structure, in vacuum too)
//
// Each way has a gain, a low-pass (air absorption, the hull: two poles,
// 12 dB/octave) and a
// direction (panning). The simulation sets their targets every frame
// (atomics: no locks on the audio thread); the node eases towards them
// over a block, so nothing clicks.
// =======================================================================

#ifndef __AUDIOPATHNODE_H
#define __AUDIOPATHNODE_H

#include <atomic>
#include <memory>
#include <vector>

struct ma_node_graph;
struct ma_engine;

namespace audio {

class PathNode {
public:
	enum Way { AIR = 0, HULL = 1, NWAY = 2 };

	struct Target {
		float gain = 0.0f;      // linear
		float cutoff = 20000.0f;// low-pass [Hz]
		float pan = 0.0f;       // -1 left .. 1 right
	};

	PathNode ();
	~PathNode ();
	PathNode (const PathNode&) = delete;
	PathNode &operator= (const PathNode&) = delete;

	/// Create the node in the engine's graph; dcBlock: the input without
	/// its DC and infrasound (below dcBlock Hz; 0: as it is)
	bool Init (ma_engine *engine, float dcBlock = 5.0f);
	void Uninit ();
	bool Valid () const { return valid; }

	/// The node (for attaching: a source to input 0, outputs to buses)
	void *Node ();

	/// Set a way's target (simulation thread)
	void Set (Way w, const Target &t);
	/// Silence both ways at once (no easing: the source vanished)
	void Cut ();

	/// The levels last reached (for the readout), linear
	float Level (Way w) const { return way[w].cur.gain.load (std::memory_order_relaxed); }

private:
	struct Impl;
	static void Process (void *node, const float **in, unsigned *nIn, float **out, unsigned *nOut);

	struct WayState {
		std::atomic<float> gain{0.0f}, cutoff{20000.0f}, pan{0.0f};
		struct { std::atomic<float> gain{0.0f}; } cur;
		float g = 0.0f, fc = 20000.0f, p = 0.0f; // (audio thread: where the easing is)
		float y = 0.0f, y2 = 0.0f;                // low-pass state (two poles)
	} way[NWAY];
	std::atomic<bool> cut{false};
	std::vector<float> dcBuf;  // (audio thread)
	float dcLp = 0.0f, dcHz = 5.0f;
	std::unique_ptr<Impl> impl;
	unsigned rate = 48000;
	bool valid = false;
};

} // namespace audio

#endif // !__AUDIOPATHNODE_H

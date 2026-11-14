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
// 12 dB/octave) and a direction: panned left to right, or - for
// headphones, with head-related transfer functions loaded (SetHrtf) and
// the way given its direction - binaural, each ear's filter and delay
// (Hrtf.h), crossfaded over a block as the direction moves. The
// simulation sets their targets every frame (atomics: no locks on the
// audio thread); the node eases towards them over a block, so nothing
// clicks.
// =======================================================================

#ifndef __AUDIOPATHNODE_H
#define __AUDIOPATHNODE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct ma_node_graph;
struct ma_engine;

namespace audio {

class Hrtf;

class PathNode {
public:
	enum Way { AIR = 0, HULL = 1, NWAY = 2 };

	struct Target {
		float gain = 0.0f;      // linear
		float cutoff = 20000.0f;// low-pass [Hz]
		float pan = 0.0f;       // -1 left .. 1 right
		/// where it comes from, in the listener's frame (x right, y up, z
		/// ahead; 0: nowhere in particular - panned): binaural with HRTFs
		float dir[3] = { 0.0f, 0.0f, 0.0f };
	};

	/// The HRTFs for every node (null: panning only); kept while any node
	/// may use them
	static void SetHrtf (const Hrtf *h);
	static const Hrtf *GetHrtf ();

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

	/// Set a way's target (simulation thread): its level and pan reached over
	/// `ramp` seconds (the simulation's step: till the next target), linearly;
	/// 0: within a block. The low-pass eases by itself (CUTOFF_TAU)
	void Set (Way w, const Target &t, double ramp = 0.0);
	/// Silence both ways at once (no easing: the source vanished)
	void Cut ();
	/// Stopped (silent, not processed: its source stopped - World's voice
	/// cap) or running
	void SetRunning (bool on);

	/// The levels last reached (for the readout), linear
	float Level (Way w) const { return way[w].cur.gain.load (std::memory_order_relaxed); }

private:
	struct Impl;
	static void Process (void *node, const float **in, unsigned *nIn, float **out, unsigned *nOut);

	struct WayState {
		std::atomic<float> gain{0.0f}, cutoff{20000.0f}, pan{0.0f};
		std::atomic<float> dir[3] = {0.0f, 0.0f, 0.0f};
		std::atomic<unsigned> ramp{0};     // [frames] the target reached over this
		std::atomic<uint32_t> gen{0};      // (bumped by each Set: a new target)
		uint32_t seen = 0;                 // (audio thread: the target taken; its level, pan; frames left of its ramp)
		float tg = 0.0f, tp = 0.0f;
		unsigned left = 0;
		struct { std::atomic<float> gain{0.0f}; } cur;
		float g = 0.0f, fc = 20000.0f, p = 0.0f; // (audio thread: where the easing is)
		float y = 0.0f, y2 = 0.0f;                // low-pass state (two poles)
		// binaural (audio thread): the filters in use (each ear's delay's
		// fraction folded in), the delays' whole samples, the direction;
		// the signal's past (the filters' reach)
		bool binaural = false;
		float hl[64], hr[64];
		int dl = 0, dr = 0;
		float u[3] = {0.0f, 0.0f, 0.0f};
		std::vector<float> past;
	} way[NWAY];
	unsigned pastLen = 0;   // (samples of the past kept)
	std::atomic<bool> cut{false};
	std::vector<float> dcBuf;  // (audio thread; sized at Init)
	std::vector<float> scratch;// (binaural: the ears' blocks; sized at Init)
	void ProcessBlock (const float *x, float **out, unsigned n);
	float dcLp = 0.0f, dcHz = 5.0f;
	std::unique_ptr<Impl> impl;
	unsigned rate = 48000;
	bool valid = false;
};

} // namespace audio

#endif // !__AUDIOPATHNODE_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The bookkeeping of the GPU particle pool (ParticleSystem), CPU only:
// rings of particles (whole blocks of the pool) allocated to emitters,
// freed, retired (their owner gone, freed when their particles have died)
// and released when idle; the list of allocated blocks the GPU passes go
// over; the sizing of rings from an emitter's rate, and how emitters thin
// out as the pool fills (ctest Particles.Pool).
// =======================================================================

#ifndef __PARTICLEPOOL_H
#define __PARTICLEPOOL_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class ParticlePool {
public:
	/// a ring: its record is kept at its first block
	struct Ring {
		uint32_t first = 0, capacity = 0; // [particles]
		uint32_t id = 0;                  // unique for the pool's lifetime (0: none)
		double lastEmit = -1e30;          // last emission [s sim]
		double life = 0.0;                // longest lifetime specified for its particles [s] (they live up to 1.5 of it)
		int prio = 1;                     // (PressureFactor)
		bool idleRelease = false;         // freed when idle (Expire); else only by its owner
		bool retired = false;             // owner gone: freed after until
		double until = 0.0;
		uint64_t tag = 0;                 // the owner's (e.g. the texture of a retired ring)
		float thin = 1.0f;                // fraction of the particles asked for that are emitted (pool short)
		float thinAcc = 0.0f;             // (rounding of thinned counts)
	};

	ParticlePool (uint32_t capacity, uint32_t block);
	void Reset ();

	/// A ring of want particles (rounded up to whole blocks), first fit; if
	/// none is free, of half as many and so on down to minWant. Returns its
	/// capacity (0: none) and first particle
	uint32_t Allocate (uint32_t want, uint32_t minWant, uint32_t &first, bool idleRelease = false);
	void Free (uint32_t first);
	/// the owner is gone: freed when its particles have died (sim time until)
	void Retire (uint32_t first, double until, uint64_t tag);
	/// Frees retired rings past their time and idle rings (no emission for
	/// 1.5 lifetimes + 1 s); all of them if time went back by more than
	/// 1e5 s (a new session). Returns how many were freed.
	size_t Expire (double simt);
	/// an emission into the ring at sim time simt of particles living life
	void Touch (uint32_t first, double simt, double life);

	/// the ring starting at particle first (null: none)
	Ring *RingAt (uint32_t first);
	const Ring *RingAt (uint32_t first) const;
	/// is the ring (first, id) still allocated (and not retired)?
	bool Valid (uint32_t first, uint32_t id) const;
	/// the first block of the ring holding block b (~0u: free)
	uint32_t RingOf (uint32_t b) const { return blockRing[b]; }

	uint32_t Capacity () const { return capacity; }
	uint32_t Block () const { return block; }
	uint32_t Allocated () const { return allocated; }
	double Fill () const { return (double)allocated / (double)capacity; }
	uint32_t FreeTotal () const { return capacity - allocated; }
	uint32_t Largest () const;
	size_t Fragments () const { return freeList.size (); }
	uint32_t Rings () const { return rings; }
	uint32_t RetiredCount () const;
	/// the allocated blocks, ascending
	const std::vector<uint32_t> &Blocks ();
	/// changes whenever rings are allocated or freed
	uint32_t Version () const { return version; }
	template<class F> void ForEachRing (F f) const { for (uint32_t b = 0; b < nblocks; b++) if (table[b].id) f (table[b]); }

	/// consistency (tests): free + allocated = capacity, no overlaps, the
	/// block table matches the rings
	bool Check (std::string *err = nullptr) const;

	// --- sizing and degradation (no state) ---
	/// particles alive at once at an emission rate [1/s] of particles living
	/// life [s] (up to 1.5 of it), with a margin
	static double Need (double rate, double life) { return rate * 1.5 * life * 1.1; }
	/// the time of emissions a ring holds [s] for particles living life [s]
	/// (up to 1.5 of it): all of them for short lives (the ring wraps over
	/// dead ones), else a segment (chained when full), a quarter of that
	/// within 2..30 s
	static double SegmentTime (double life) { double t = 1.5 * life; return t < 2.0 ? t : std::min (t, std::max (2.0, std::min (0.25 * t, 30.0))); }
	/// a ring's new capacity for a need (smoothed): 0 = keep it. Grows when
	/// the need exceeds it, shrinks when below 30 %; the new one holds 1.5
	/// times the need
	static uint32_t Resize (uint32_t capacity, double need, uint32_t block);
	/// density factor for emitters of priority prio (0: trails, kept
	/// longest; 1: engine smoke, fire; 2: dust, spray, legacy streams; 3:
	/// sparks, fragments, puffs) with the pool filled to fill (0..1): 1 up
	/// to 70 % (trails 85 %), then less, down to 0.25 when full
	static double PressureFactor (double fill, int prio);
	/// lifetime factor: the least visible classes (prio >= 2) live shorter
	/// above 80 %, down to half at 95 %
	static double LifeFactor (double fill, int prio);
	/// emissions of the lowest priority are dropped above 95 %
	static bool Shed (double fill, int prio) { return prio >= 3 && fill > 0.95; }

private:
	uint32_t capacity, block, nblocks;
	uint32_t allocated = 0, rings = 0, version = 0, nextId = 1;
	std::vector<std::pair<uint32_t, uint32_t>> freeList; // (first block, blocks), ascending, merged
	std::vector<uint32_t> blockRing;                     // first block of the ring of each block (~0u: free)
	std::vector<Ring> table;                             // ring records at their first block
	std::vector<uint32_t> blocks;                        // allocated blocks (Blocks)
	uint32_t blocksVersion = ~0u;
	double lastSimt = -1e30;
};

#endif // !__PARTICLEPOOL_H

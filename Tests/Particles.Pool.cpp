// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The particle pool's bookkeeping (OVP/VulkanClient/ParticlePool): rings
// allocated, freed, retired and released when idle under random use; the
// block list the GPU passes go over; halving on exhaustion; sizing and the
// degradation under pool pressure

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ParticlePool.h"
#include <algorithm>
#include <map>
#include <random>
#include <set>

namespace {
	constexpr uint32_t CAP = 1u << 18, BLK = 256;
}

TEST_CASE ("allocate and free keep the pool consistent", "[Particles.Pool]")
{
	ParticlePool p (CAP, BLK);
	std::string err;
	REQUIRE (p.Check (&err));
	uint32_t a, b, c;
	REQUIRE (p.Allocate (1000, 1000, a) == 1024);
	REQUIRE (p.Allocate (1, 1, b) == 256);
	REQUIRE (p.Allocate (5000, 5000, c) == 5120);
	CHECK (a == 0);
	CHECK (b == 1024);
	CHECK (c == 1280);
	CHECK (p.Allocated () == 1024 + 256 + 5120);
	CHECK (p.Rings () == 3);
	CHECK (p.Check (&err));
	p.Free (b);
	CHECK (p.Fragments () == 2);
	CHECK (p.Check (&err));
	p.Free (a);
	CHECK (p.Fragments () == 2); // (a and b merged)
	p.Free (c);
	CHECK (p.Fragments () == 1);
	CHECK (p.Allocated () == 0);
	CHECK (p.Largest () == CAP);
	CHECK (p.Check (&err));
	p.Free (12345); // (no ring there: ignored)
	CHECK (p.Check (&err));
}

TEST_CASE ("random use: invariants, block list, ids", "[Particles.Pool]")
{
	ParticlePool p (CAP, BLK);
	std::mt19937 rng (12345);
	std::map<uint32_t, uint32_t> live; // first -> id
	std::set<uint32_t> ids;
	double simt = 0.0;
	std::string err;
	for (int step = 0; step < 100000; step++) {
		simt += 0.02;
		int op = rng () % 10;
		if (op < 4) {
			uint32_t want = 1 + rng () % 20000, first;
			uint32_t got = p.Allocate (want, want / 4, first, (rng () & 1) != 0);
			if (got) {
				REQUIRE (got % BLK == 0);
				REQUIRE (got >= std::min (want, (want / 4 + BLK - 1) / BLK * BLK));
				const ParticlePool::Ring *r = p.RingAt (first);
				REQUIRE (r);
				REQUIRE (ids.insert (r->id).second); // (unique ids)
				live[first] = r->id;
				p.Touch (first, simt, 1.0 + rng () % 10);
			}
		} else if (op < 6 && !live.empty ()) {
			auto it = live.begin ();
			std::advance (it, rng () % live.size ());
			p.Free (it->first);
			REQUIRE_FALSE (p.Valid (it->first, it->second));
			live.erase (it);
		} else if (op < 7 && !live.empty ()) {
			auto it = live.begin ();
			std::advance (it, rng () % live.size ());
			p.Retire (it->first, simt + (rng () % 100) * 0.1, 7);
			REQUIRE_FALSE (p.Valid (it->first, it->second)); // (retired: not its owner's any more)
			live.erase (it);
		} else if (op < 9 && !live.empty ()) {
			auto it = live.begin ();
			std::advance (it, rng () % live.size ());
			p.Touch (it->first, simt, 2.0);
		} else {
			p.Expire (simt);
			for (auto it = live.begin (); it != live.end (); )
				if (!p.Valid (it->first, it->second)) it = live.erase (it);
				else ++it;
		}
		if (step % 97 == 0) {
			REQUIRE (p.Check (&err));
			// the block list: exactly the blocks of the rings
			const auto &bl = p.Blocks ();
			uint32_t n = 0;
			p.ForEachRing ([&n](const ParticlePool::Ring &r) { n += r.capacity / BLK; });
			REQUIRE (bl.size () == n);
			REQUIRE (std::is_sorted (bl.begin (), bl.end ()));
			for (uint32_t b : bl) REQUIRE (p.RingOf (b) != ~0u);
			REQUIRE (bl.size () * BLK == p.Allocated ());
		}
	}
	INFO (err);
	REQUIRE (p.Check (&err));
}

TEST_CASE ("retired rings are freed after their time, or when time goes back", "[Particles.Pool]")
{
	ParticlePool p (CAP, BLK);
	uint32_t a, b;
	REQUIRE (p.Allocate (1000, 1000, a));
	REQUIRE (p.Allocate (1000, 1000, b));
	p.Retire (a, 10.0, 0);
	p.Retire (b, 20.0, 0);
	CHECK (p.RetiredCount () == 2);
	CHECK (p.Expire (5.0) == 0);
	CHECK (p.Expire (10.0) == 0);   // (freed after until)
	CHECK (p.Expire (10.01) == 1);
	CHECK (p.RingAt (a) == nullptr);
	CHECK (p.RingAt (b) != nullptr);
	CHECK (p.Expire (-2e5) == 1);   // (time went back: a new session)
	CHECK (p.Allocated () == 0);
	CHECK (p.Check ());
}

TEST_CASE ("idle rings are released 1.5 lifetimes + 1 s after the last emission", "[Particles.Pool]")
{
	ParticlePool p (CAP, BLK);
	uint32_t a, b;
	REQUIRE (p.Allocate (1000, 1000, a, true));
	REQUIRE (p.Allocate (1000, 1000, b, false)); // (owner frees it)
	p.Touch (a, 100.0, 10.0);
	p.Touch (b, 100.0, 10.0);
	uint32_t id = p.RingAt (a)->id;
	CHECK (p.Expire (115.9) == 0);
	CHECK (p.Valid (a, id));
	CHECK (p.Expire (116.1) == 1);
	CHECK_FALSE (p.Valid (a, id));
	CHECK (p.RingAt (b) != nullptr);
	CHECK (p.Check ());
}

TEST_CASE ("exhaustion: halving down to the minimum", "[Particles.Pool]")
{
	ParticlePool p (CAP, BLK);
	uint32_t f;
	// fragment the pool: rings of 8 blocks, every other one freed
	std::vector<uint32_t> firsts;
	while (p.Allocate (8 * BLK, 8 * BLK, f)) firsts.push_back (f);
	CHECK (p.Allocated () == CAP);
	CHECK (p.Allocate (BLK, BLK, f) == 0);
	for (size_t i = 0; i < firsts.size (); i += 2) p.Free (firsts[i]);
	CHECK (p.Largest () == 8 * BLK);
	// 64 blocks wanted: halved to 8
	uint32_t got = p.Allocate (64 * BLK, 4 * BLK, f);
	CHECK (got == 8 * BLK);
	// minimum above what is free: none
	CHECK (p.Allocate (64 * BLK, 16 * BLK, f) == 0);
	CHECK (p.Check ());
}

TEST_CASE ("sizing from the rate, grow and shrink", "[Particles.Pool]")
{
	// 100 particles/s living 10 s (up to 15): 1650 with the margin
	CHECK (ParticlePool::Need (100.0, 10.0) == Catch::Approx (1650.0));
	// grow when the need exceeds the ring, to 1.5 times it
	CHECK (ParticlePool::Resize (1024, 1000.0, BLK) == 0);
	CHECK (ParticlePool::Resize (1024, 1100.0, BLK) == 1792);
	// shrink below 30 %
	CHECK (ParticlePool::Resize (16384, 6000.0, BLK) == 0);
	CHECK (ParticlePool::Resize (16384, 4000.0, BLK) == 6144);
	// never below a block, nor shrinking small rings
	CHECK (ParticlePool::Resize (512, 1.0, BLK) == 0);
	CHECK (ParticlePool::Resize (256, 0.0, BLK) == 0);
	// grow then shrink keeps a ring holding its need
	for (double need : { 10.0, 300.0, 5000.0, 60000.0, 20000.0, 3000.0, 100.0 }) {
		uint32_t cap = 1024, n = ParticlePool::Resize (cap, need, BLK);
		if (n) cap = n;
		CHECK (cap >= need);
		CHECK (cap % BLK == 0);
	}
}

TEST_CASE ("degradation under pool pressure", "[Particles.Pool]")
{
	// density: full below 70 % (trails 85 %), then less, a quarter when full
	CHECK (ParticlePool::PressureFactor (0.5, 1) == 1.0);
	CHECK (ParticlePool::PressureFactor (0.7, 1) == 1.0);
	CHECK (ParticlePool::PressureFactor (0.85, 1) == Catch::Approx (0.625));
	CHECK (ParticlePool::PressureFactor (1.0, 1) == Catch::Approx (0.25));
	CHECK (ParticlePool::PressureFactor (0.85, 0) == 1.0);
	CHECK (ParticlePool::PressureFactor (0.9, 0) > ParticlePool::PressureFactor (0.9, 1));
	// lifetimes: only the least visible classes, above 80 %
	CHECK (ParticlePool::LifeFactor (0.95, 1) == 1.0);
	CHECK (ParticlePool::LifeFactor (0.8, 2) == 1.0);
	CHECK (ParticlePool::LifeFactor (0.95, 2) == Catch::Approx (0.5));
	// dropped last: the lowest priority only, above 95 %
	CHECK_FALSE (ParticlePool::Shed (0.99, 2));
	CHECK_FALSE (ParticlePool::Shed (0.9, 3));
	CHECK (ParticlePool::Shed (0.96, 3));
	// monotonic
	double prev = 1.0;
	for (double f = 0.0; f <= 1.0; f += 0.01) {
		double x = ParticlePool::PressureFactor (f, 2);
		CHECK (x <= prev + 1e-12);
		prev = x;
	}
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ParticlePool.h"
#include <algorithm>
#include <cmath>

ParticlePool::ParticlePool (uint32_t cap, uint32_t blk)
	: capacity(cap), block(blk), nblocks(cap / blk)
{
	Reset ();
}

void ParticlePool::Reset ()
{
	freeList.assign (1, { 0u, nblocks });
	blockRing.assign (nblocks, ~0u);
	table.assign (nblocks, Ring{});
	allocated = rings = 0;
	version++;
}

uint32_t ParticlePool::Allocate (uint32_t want, uint32_t minWant, uint32_t &first, bool idleRelease)
{
	uint32_t n = std::max (1u, (want + block - 1) / block);              // [blocks]
	uint32_t nmin = std::min (n, std::max (1u, (minWant + block - 1) / block));
	for (;;) {
		for (size_t i = 0; i < freeList.size (); i++) {
			auto &f = freeList[i];
			if (f.second < n) continue;
			uint32_t b0 = f.first;
			f.first += n, f.second -= n;
			if (!f.second) freeList.erase (freeList.begin () + i);
			for (uint32_t k = b0; k < b0 + n; k++) blockRing[k] = b0;
			Ring &r = table[b0];
			r = Ring{};
			r.first = b0 * block, r.capacity = n * block, r.id = nextId++;
			r.idleRelease = idleRelease;
			allocated += n * block, rings++, version++;
			first = r.first;
			return r.capacity;
		}
		if (n <= nmin) return 0;
		n = std::max (nmin, n / 2);
	}
}

void ParticlePool::Free (uint32_t first)
{
	uint32_t b0 = first / block;
	if (b0 >= nblocks || !table[b0].id) return;
	uint32_t n = table[b0].capacity / block;
	for (uint32_t k = b0; k < b0 + n; k++) blockRing[k] = ~0u;
	table[b0] = Ring{};
	allocated -= n * block, rings--, version++;
	auto it = std::lower_bound (freeList.begin (), freeList.end (), std::make_pair (b0, 0u));
	it = freeList.insert (it, { b0, n });
	// (merged with its neighbours)
	if (it + 1 != freeList.end () && it->first + it->second == (it + 1)->first) {
		it->second += (it + 1)->second;
		freeList.erase (it + 1);
	}
	if (it != freeList.begin () && (it - 1)->first + (it - 1)->second == it->first) {
		(it - 1)->second += it->second;
		freeList.erase (it);
	}
}

void ParticlePool::Retire (uint32_t first, double until, uint64_t tag)
{
	Ring *r = RingAt (first);
	if (!r) return;
	r->retired = true, r->until = until, r->tag = tag;
}

size_t ParticlePool::Expire (double simt)
{
	bool back = simt < lastSimt - 1e5; // (a new session)
	lastSimt = simt;
	std::vector<uint32_t> gone;
	for (uint32_t b = 0; b < nblocks; b++) {
		const Ring &r = table[b];
		if (!r.id) continue;
		if (r.retired) {
			if (simt > r.until || simt < r.until - 1e5 || back) gone.push_back (r.first);
		} else if (r.idleRelease) {
			double end = r.lastEmit + 1.5 * r.life + 1.0;
			if (simt > end || simt < r.lastEmit - 1e5 || back) gone.push_back (r.first);
		}
	}
	for (uint32_t f : gone) Free (f);
	return gone.size ();
}

void ParticlePool::Touch (uint32_t first, double simt, double life)
{
	if (Ring *r = RingAt (first)) r->lastEmit = simt, r->life = std::max (r->life, life);
}

ParticlePool::Ring *ParticlePool::RingAt (uint32_t first)
{
	uint32_t b = first / block;
	return b < nblocks && table[b].id && table[b].first == first ? &table[b] : nullptr;
}

const ParticlePool::Ring *ParticlePool::RingAt (uint32_t first) const
{
	uint32_t b = first / block;
	return b < nblocks && table[b].id && table[b].first == first ? &table[b] : nullptr;
}

bool ParticlePool::Valid (uint32_t first, uint32_t id) const
{
	const Ring *r = RingAt (first);
	return r && id && r->id == id && !r->retired;
}

uint32_t ParticlePool::Largest () const
{
	uint32_t l = 0;
	for (auto &f : freeList) l = std::max (l, f.second);
	return l * block;
}

uint32_t ParticlePool::RetiredCount () const
{
	uint32_t n = 0;
	ForEachRing ([&n](const Ring &r) { n += r.retired ? 1 : 0; });
	return n;
}

const std::vector<uint32_t> &ParticlePool::Blocks ()
{
	if (blocksVersion != version) {
		blocks.clear ();
		for (uint32_t b = 0; b < nblocks; b++)
			if (blockRing[b] != ~0u) blocks.push_back (b);
		blocksVersion = version;
	}
	return blocks;
}

bool ParticlePool::Check (std::string *err) const
{
	auto fail = [err](const std::string &m) { if (err) *err = m; return false; };
	uint32_t freeBlocks = 0, prevEnd = 0;
	for (size_t i = 0; i < freeList.size (); i++) {
		auto &f = freeList[i];
		if (!f.second) return fail ("empty free range");
		if (i && f.first <= prevEnd) return fail ("free ranges unsorted or not merged");
		if (f.first + f.second > nblocks) return fail ("free range beyond the pool");
		for (uint32_t k = f.first; k < f.first + f.second; k++)
			if (blockRing[k] != ~0u) return fail ("free block owned by a ring");
		freeBlocks += f.second, prevEnd = f.first + f.second;
	}
	uint32_t ringBlocks = 0, n = 0;
	for (uint32_t b = 0; b < nblocks; b++) {
		const Ring &r = table[b];
		if (!r.id) continue;
		if (r.first != b * block || !r.capacity || r.capacity % block) return fail ("bad ring record");
		for (uint32_t k = b; k < b + r.capacity / block; k++) {
			if (k >= nblocks || blockRing[k] != b) return fail ("block table does not match a ring");
			if (k > b && table[k].id) return fail ("overlapping rings");
		}
		ringBlocks += r.capacity / block, n++;
	}
	uint32_t owned = 0;
	for (uint32_t b = 0; b < nblocks; b++) owned += blockRing[b] != ~0u ? 1 : 0;
	if (owned != ringBlocks) return fail ("blocks owned without a ring");
	if (freeBlocks + ringBlocks != nblocks) return fail ("free + allocated != capacity");
	if (ringBlocks * block != allocated) return fail ("allocated count wrong");
	if (n != rings) return fail ("ring count wrong");
	return true;
}

uint32_t ParticlePool::Resize (uint32_t cap, double need, uint32_t blk)
{
	double want = std::max (1.5 * need, (double)blk);
	if (need > cap || (need < 0.3 * cap && cap > 2 * blk)) {
		uint32_t n = (uint32_t)std::min (std::ceil (want / blk) * blk, 4294967040.0);
		return n != cap ? n : 0u;
	}
	return 0u;
}

double ParticlePool::PressureFactor (double fill, int prio)
{
	double start = prio <= 0 ? 0.85 : 0.70;
	return 1.0 - 0.75 * std::clamp ((fill - start) / (1.0 - start), 0.0, 1.0);
}

double ParticlePool::LifeFactor (double fill, int prio)
{
	if (prio < 2) return 1.0;
	return 1.0 - 0.5 * std::clamp ((fill - 0.8) / 0.15, 0.0, 1.0);
}

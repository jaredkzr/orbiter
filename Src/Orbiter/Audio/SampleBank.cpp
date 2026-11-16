// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "SampleBank.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace audio {

namespace {
	constexpr float RATE_JITTER = 0.06f, GAIN_JITTER_DB = 1.5f;
}

SampleBank::Tier SampleBank::TierNamed (const std::string &s)
{
	if (s == "soft") return SOFT;
	if (s == "mid") return MID;
	if (s == "hard") return HARD;
	return ANY;
}

SampleBank::Blend SampleBank::ForStrength (float s)
{
	s = std::clamp (s, 0.0f, 1.0f);
	Blend b;
	const int t = std::min (2, (int)(s * 3.0f));
	b.tier = b.next = (Tier)t;
	const float centre = (t + 0.5f) / 3.0f;
	const float off = (s - centre) * 3.0f;     // (-0.5 .. 0.5 across the tier)
	const int n = off < 0.0f ? t - 1 : t + 1;
	if (n >= 0 && n <= 2) b.next = (Tier)n, b.w = std::fabs (off);
	return b;
}

bool SampleBank::Load (const std::string &manifest, unsigned r)
{
	layers.clear (), decks.clear ();
	rate = r;
	std::ifstream f (manifest);
	if (!f) return false;
	const size_t slash = manifest.find_last_of ('/');
	const std::string dir = slash == std::string::npos ? std::string (".") : manifest.substr (0, slash);
	std::string line;
	int n = 0;
	while (std::getline (f, line)) {
		const size_t hash = line.find ('#');
		if (hash != std::string::npos) line.resize (hash);
		std::istringstream is (line);
		std::string file, layer, tier;
		double energy = 0.0;
		if (!(is >> file >> layer >> tier >> energy)) continue;
		std::string path = Assets::Get ().Resolve (dir + "/" + file);
		if (path.empty ()) {
			fprintf (stderr, "Audio: %s: %s not found\n", manifest.c_str (), file.c_str ());
			continue;
		}
		Entry e{path, energy};
		layers[layer][(int)TierNamed (tier)].push_back (e);
		if (TierNamed (tier) != ANY) layers[layer][(int)ANY].push_back (e);
		Assets::Get ().Prefetch (path, rate, 1);
		n++;
	}
	return n > 0;
}

bool SampleBank::Has (const std::string &layer) const
{
	return layers.count (layer) > 0;
}

const std::vector<SampleBank::Entry> *SampleBank::Entries (const std::string &layer, Tier tier) const
{
	auto l = layers.find (layer);
	if (l == layers.end ()) return nullptr;
	auto t = l->second.find ((int)tier);
	return t == l->second.end () || t->second.empty () ? nullptr : &t->second;
}

int SampleBank::Count (const std::string &layer, Tier tier) const
{
	const auto *e = Entries (layer, tier);
	return e ? (int)e->size () : 0;
}

void SampleBank::Shuffle (Deck &d, size_t n)
{
	d.order.resize (n);
	for (size_t i = 0; i < n; i++) d.order[i] = (int)i;
	for (size_t i = n; i > 1; i--) std::swap (d.order[i - 1], d.order[rng.Next () % i]);
	// (never the last of the old order first in the new: no take twice running)
	if (n > 1 && d.order[0] == d.last) std::swap (d.order[0], d.order[1 + rng.Next () % (n - 1)]);
	d.at = 0;
}

SampleBank::Take SampleBank::Pick (const std::string &layer, Tier tier)
{
	Take t;
	const auto *es = Entries (layer, tier);
	if (!es) {
		if (warned.insert (layer).second && !Has (layer))
			fprintf (stderr, "Audio: no recordings of %s: silent\n", layer.c_str ());
		return t;
	}
	Deck &d = decks[layer][(int)tier];
	const size_t n = es->size ();
	// (the next decoded: a take still decoding is passed over this time)
	for (size_t tries = 0; tries < n; tries++) {
		if (d.order.size () != n || d.at >= n) Shuffle (d, n);
		const int i = d.order[d.at++];
		const Entry &e = (*es)[i];
		ClipRef c;
		auto h = held.find (e.path);
		if (h != held.end ()) c = h->second;
		else {
			bool failed = false;
			c = Assets::Get ().Peek (e.path, rate, 1, failed);
			if (c) held[e.path] = c;
		}
		if (!c || c->frames < 2) continue;
		d.last = i;
		t.clip = c;
		t.energy = e.energy;
		t.file = e.path;
		t.rate = 1.0f + RATE_JITTER * rng.White ();
		t.gain = std::pow (10.0f, GAIN_JITTER_DB * rng.White () / 20.0f);
		return t;
	}
	return t;
}

} // namespace audio

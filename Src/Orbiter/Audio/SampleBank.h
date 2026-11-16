// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Recordings of one kind of sound in several takes and strengths, played
// so that no two in a row are the same: a manifest lists them -
//
//   # comments
//   <file, relative to the manifest's folder> <layer> <tier> <energy>
//
// (Sound/Contacts/contacts.txt: impact_gear/impact_gear_hard_1.flac
// impact_gear hard 0.0123). A layer is a kind of sound (impact_gear,
// strut, rattle); its tier a strength (soft, mid, hard; "-": one only);
// its energy the file's as stored, sum(x^2) / 48000 [full scale^2 s], so a
// caller sets a take's gain from the energy it should carry.
//
// Pick: a take of a layer and tier - shuffled, each in turn, never the
// same twice running - at a rate and gain a little off (+-6 %, +-1.5 dB),
// repeatably (a seed). A layer not there is silent (one warning), never
// another's sound.
//
// The files are decoded through Assets (in the background on the device);
// a take not decoded yet is passed over. The bank keeps every clip it has
// handed out while it lives: a one-shot may carry a clip's address
// (OneShot::ref) to the audio thread.
// =======================================================================

#ifndef __AUDIOSAMPLEBANK_H
#define __AUDIOSAMPLEBANK_H

#include "Assets.h"
#include "Synth.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace audio {

class SampleBank {
public:
	enum Tier { ANY = -1, SOFT = 0, MID = 1, HARD = 2 };

	explicit SampleBank (uint32_t seed = 1): rng (seed) {}

	/// Read a manifest (a resolved path); its files decoded at `rate`.
	/// False: none (not there, or nothing in it)
	bool Load (const std::string &manifest, unsigned rate);
	bool Has (const std::string &layer) const;
	/// Takes of a layer and tier (ANY: all of the layer's)
	int Count (const std::string &layer, Tier tier = ANY) const;

	struct Take {
		ClipRef clip;             ///< (null: none - silent)
		float rate = 1.0f;        ///< its playback rate (jittered)
		float gain = 1.0f;        ///< its gain (jittered)
		double energy = 0.0;      ///< its energy as stored [FS^2 s]
		std::string file;
		explicit operator bool () const { return clip != nullptr; }
	};
	/// The next take of a layer at a tier (none of that tier: none - a
	/// caller wanting another asks for it)
	Take Pick (const std::string &layer, Tier tier = ANY);

	/// A strength (0..1) as tiers: soft below 1/3, mid to 2/3, hard above;
	/// near a tier's edge some of the next (w: its share, 0 at a tier's
	/// middle, 0.5 at its edge - crossfade the two)
	struct Blend { Tier tier = MID, next = MID; float w = 0.0f; };
	static Blend ForStrength (float s);
	static Tier TierNamed (const std::string &s);

private:
	struct Entry { std::string path; double energy = 0.0; };
	struct Deck {
		std::vector<int> order;   // (shuffled indices into entries)
		size_t at = 0;
		int last = -1;
	};
	// (by layer, then tier: ANY's deck holds them all)
	std::map<std::string, std::map<int, std::vector<Entry>>> layers;
	std::map<std::string, std::map<int, Deck>> decks;
	std::map<std::string, ClipRef> held;    // (clips handed out: kept)
	std::set<std::string> warned;
	unsigned rate = 48000;
	dsp::Rng rng;
	void Shuffle (Deck &d, size_t n);
	const std::vector<Entry> *Entries (const std::string &layer, Tier tier) const;
};

} // namespace audio

#endif // !__AUDIOSAMPLEBANK_H

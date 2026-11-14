// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Sound files, one way: every file the sound plays - a callout, a click,
// an emitter's clip, a radio transmission, the contacts' recordings - is
// found, decoded and measured here, once, and shared.
//
//  - Resolve: a file's path as named (backslashes, any case: add-ons'
//    cfgs name files Windows' way), remembered (found or not) until Forget
//  - Load: decoded (f32, at a rate, the file's own channels or as many as
//    asked), remembered; a ClipRef keeps it while it plays
//  - Prefetch / Peek: decoded on a worker thread, so the simulation thread
//    never waits on a disk (playing on the device). Synchronous instead
//    (SetAsync (false)) for renders: repeatable to the sample
//  - Measured: a file's loudness (BS.1770) and peak, as recorded
//  - ClipSound: an ma_sound playing a clip (on a bus, or into a node)
//
// Unused clips are dropped, oldest first, past a budget (BUDGET bytes).
// =======================================================================

#ifndef __AUDIOASSETS_H
#define __AUDIOASSETS_H

#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ma_engine;
struct ma_sound;
typedef struct ma_sound ma_sound_group;

namespace audio {

/// A sound file decoded: interleaved f32 frames
struct Clip {
	std::vector<float> pcm;
	unsigned channels = 0, rate = 0;
	size_t frames = 0;
	/// Its loudness [LUFS] (-inf: silent): of these samples, as decoded
	double lufs = -INFINITY;
};
using ClipRef = std::shared_ptr<const Clip>;

class Assets {
public:
	/// The process's cache
	static Assets &Get ();
	~Assets ();

	/// A file's path: as given, backslashes as slashes, its case as on the
	/// disk (empty: none); remembered
	std::string Resolve (const std::string &file);

	/// A file (a resolved path) decoded at `rate` with `channels` (0: its
	/// own): now, waiting for it if it is being loaded (null: not readable)
	ClipRef Load (const std::string &path, unsigned rate, unsigned channels = 0);
	/// The same, in the background (synchronous: SetAsync (false));
	/// measure: its loudness too (Measured), before the clip is ready
	void Prefetch (const std::string &path, unsigned rate, unsigned channels = 0, bool measure = false);
	/// Loaded yet? The clip, or null while it loads (failed: true when
	/// it can't be read - nothing will come)
	ClipRef Peek (const std::string &path, unsigned rate, unsigned channels, bool &failed);

	/// A file's loudness [LUFS] and peak, as recorded (at its own rate and
	/// channels; NAN: not readable); remembered
	struct Measure { double lufs = NAN, peak = 0.0; };
	Measure Measured (const std::string &path);

	/// Some work of the sound's, off the simulation thread (after the files asked
	/// for before it; at once when not async)
	void Run (std::function<void ()> task);

	/// Decode in the background (the device) or at once (renders, tests)
	void SetAsync (bool on);
	bool Async () const { return async; }
	/// Forget the paths found (a new session: files may have come or gone)
	/// and the clips not playing
	void Forget ();
	/// Stop the worker (the engine shuts down)
	void Shutdown ();

	/// Clips not playing are kept up to this many bytes
	static constexpr size_t BUDGET = 96u << 20;

	/// (tests) files decoded so far
	int Decoded () const { return decoded; }

private:
	Assets () = default;
	struct Entry {
		ClipRef clip;
		bool loading = false, failed = false;
		uint64_t used = 0;     // (the last use: an LRU's clock)
	};
	static std::string Key (const std::string &path, unsigned rate, unsigned channels);
	static ClipRef Decode (const std::string &path, unsigned rate, unsigned channels);
	void Worker ();
	void Trim ();              // (with the lock held)
	std::mutex m;
	std::condition_variable cv, done;
	std::map<std::string, Entry> clips;
	std::map<std::string, std::string> paths;
	std::map<std::string, Measure> measures;
	struct Job { std::string path, key; unsigned rate, channels; bool measure; std::function<void ()> task; };
	void Run (std::unique_lock<std::mutex> &l, const Job &j);
	std::deque<Job> jobs;
	std::thread worker;
	bool stopping = false, async = false;
	uint64_t clock = 0;
	int decoded = 0;
};

/// An ma_sound playing a clip: on a bus (a sound group), or not attached
/// (into an emitter's node: Attach it yourself); the clip held while it plays
class ClipSound {
public:
	ClipSound ();
	~ClipSound ();
	ClipSound (const ClipSound&) = delete;
	ClipSound &operator= (const ClipSound&) = delete;
	/// group: its bus (null: not attached); channelsOut: 0 the engine's;
	/// spatialized never
	bool Init (ma_engine *engine, ClipRef clip, ma_sound_group *group, unsigned channelsOut, bool loop);
	void Uninit ();
	ma_sound *Sound () { return sound.get (); }
	const ma_sound *Sound () const { return sound.get (); }
	explicit operator bool () const { return valid; }

private:
	struct Ref;
	ClipRef clip;
	std::unique_ptr<Ref> ref;
	std::unique_ptr<ma_sound> sound;
	bool valid = false;
};

} // namespace audio

#endif // !__AUDIOASSETS_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Assets.h"
#include "Loudness.h"
#include "miniaudio.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace audio {

namespace {
	/// A path as given, its backslashes as slashes, its case as on the disk
	/// (files named in Windows' manner: add-ons' cfgs)
	std::string ResolveCase (std::string f)
	{
		std::replace (f.begin (), f.end (), '\\', '/');
		std::error_code ec;
		if (f.empty () || std::filesystem::exists (f, ec)) return f;
		namespace fs = std::filesystem;
		fs::path in (f), out;
		for (const fs::path &part : in) {
			fs::path next = out.empty () ? part : out / part;
			if (!fs::exists (next, ec)) {
				bool found = false;
				std::string want = part.string ();
				for (const auto &de : fs::directory_iterator (out.empty () ? fs::path (".") : out, ec)) {
					std::string name = de.path ().filename ().string ();
					if (name.size () == want.size () && std::equal (name.begin (), name.end (), want.begin (),
						[](char a, char b) { return std::tolower ((unsigned char)a) == std::tolower ((unsigned char)b); })) {
						next = out.empty () ? fs::path (name) : out / name;
						found = true;
						break;
					}
				}
				if (!found) return std::string ();
			}
			out = next;
		}
		return out.string ();
	}
}

Assets &Assets::Get ()
{
	static Assets a;
	return a;
}

Assets::~Assets ()
{
	Shutdown ();
}

std::string Assets::Resolve (const std::string &file)
{
	if (file.empty ()) return std::string ();
	{
		std::lock_guard<std::mutex> l (m);
		auto it = paths.find (file);
		if (it != paths.end ()) return it->second;
	}
	std::string r = ResolveCase (file);
	std::lock_guard<std::mutex> l (m);
	paths[file] = r;
	return r;
}

std::string Assets::Key (const std::string &path, unsigned rate, unsigned channels)
{
	return path + '@' + std::to_string (rate) + '/' + std::to_string (channels);
}

ClipRef Assets::Decode (const std::string &path, unsigned rate, unsigned channels)
{
	// (as miniaudio's resource manager decodes a file for a sound: f32, at
	// the engine's rate, its own channels - the same samples)
	ma_decoder_config c = ma_decoder_config_init (ma_format_f32, channels, rate);
	ma_decoder d;
	if (ma_decoder_init_file (path.c_str (), &c, &d) != MA_SUCCESS) return nullptr;
	auto clip = std::make_shared<Clip>();
	clip->channels = d.outputChannels;
	clip->rate = d.outputSampleRate;
	ma_uint64 len = 0;
	if (ma_decoder_get_length_in_pcm_frames (&d, &len) == MA_SUCCESS && len > 0 && len < (ma_uint64)clip->rate * 3600)
		clip->pcm.reserve ((size_t)len * clip->channels);
	float tmp[4096];
	const ma_uint64 chunk = 4096 / std::max (1u, clip->channels);
	for (;;) {
		ma_uint64 got = 0;
		ma_result r = ma_decoder_read_pcm_frames (&d, tmp, chunk, &got);
		if (got) clip->pcm.insert (clip->pcm.end (), tmp, tmp + got * clip->channels);
		if (r != MA_SUCCESS || got == 0) break;
	}
	ma_decoder_uninit (&d);
	clip->frames = clip->channels ? clip->pcm.size () / clip->channels : 0;
	if (!clip->frames) return nullptr;
	clip->lufs = Loudness (clip->pcm.data (), clip->frames, clip->channels, clip->rate);
	return clip;
}

ClipRef Assets::Load (const std::string &path, unsigned rate, unsigned channels)
{
	if (path.empty ()) return nullptr;
	const std::string key = Key (path, rate, channels);
	std::unique_lock<std::mutex> l (m);
	Entry &e = clips[key];
	e.used = ++clock;
	while (e.loading) done.wait (l);
	if (e.clip || e.failed) return e.clip;
	e.loading = true;
	l.unlock ();
	ClipRef c = Decode (path, rate, channels);
	l.lock ();
	Entry &f = clips[key];
	f.clip = c, f.failed = !c, f.loading = false;
	decoded++;
	done.notify_all ();
	Trim ();
	return c;
}

void Assets::Prefetch (const std::string &path, unsigned rate, unsigned channels, bool measure)
{
	if (path.empty ()) return;
	if (!async) { if (measure) Measured (path); Load (path, rate, channels); return; }
	const std::string key = Key (path, rate, channels);
	std::lock_guard<std::mutex> l (m);
	if (measure && !measures.count (path)) jobs.push_back ({path, std::string (), 0, 0, true}); // (first: ready with the clip)
	auto it = clips.find (key);
	if (it != clips.end () && (it->second.clip || it->second.failed || it->second.loading)) { it->second.used = ++clock; }
	else {
		clips[key].loading = true;
		clips[key].used = ++clock;
		jobs.push_back ({path, key, rate, channels, false});
	}
	if (jobs.empty ()) return;
	if (!worker.joinable ()) stopping = false, worker = std::thread (&Assets::Worker, this);
	cv.notify_one ();
}

ClipRef Assets::Peek (const std::string &path, unsigned rate, unsigned channels, bool &failed)
{
	failed = false;
	if (path.empty ()) { failed = true; return nullptr; }
	if (!async) { ClipRef c = Load (path, rate, channels); failed = !c; return c; }
	{
		std::lock_guard<std::mutex> l (m);
		auto it = clips.find (Key (path, rate, channels));
		if (it != clips.end () && !it->second.loading && (it->second.clip || it->second.failed)) {
			it->second.used = ++clock;
			failed = it->second.failed;
			return it->second.clip;
		}
		if (it != clips.end () && it->second.loading) return nullptr;
	}
	Prefetch (path, rate, channels);
	return nullptr;
}

void Assets::Worker ()
{
	std::unique_lock<std::mutex> l (m);
	for (;;) {
		cv.wait (l, [this] { return stopping || !jobs.empty (); });
		if (stopping) return;
		Job j = jobs.front ();
		jobs.pop_front ();
		Run (l, j);
	}
}

void Assets::Run (std::unique_lock<std::mutex> &l, const Job &j)
{
	l.unlock ();
	if (j.measure) { Measured (j.path); l.lock (); return; }
	ClipRef c = Decode (j.path, j.rate, j.channels);
	l.lock ();
	Entry &e = clips[j.key];
	e.clip = c, e.failed = !c, e.loading = false;
	decoded++;
	done.notify_all ();
	Trim ();
}

void Assets::Trim ()
{
	// (clips playing - held elsewhere too - stay; of the rest, the oldest go)
	size_t idle = 0;
	for (auto &kv : clips)
		if (kv.second.clip && kv.second.clip.use_count () == 1) idle += kv.second.clip->pcm.size () * sizeof (float);
	while (idle > BUDGET) {
		auto oldest = clips.end ();
		for (auto it = clips.begin (); it != clips.end (); ++it)
			if (it->second.clip && it->second.clip.use_count () == 1 && (oldest == clips.end () || it->second.used < oldest->second.used)) oldest = it;
		if (oldest == clips.end ()) break;
		idle -= oldest->second.clip->pcm.size () * sizeof (float);
		clips.erase (oldest);
	}
}

Assets::Measure Assets::Measured (const std::string &path)
{
	{
		std::lock_guard<std::mutex> l (m);
		auto it = measures.find (path);
		if (it != measures.end ()) return it->second;
	}
	// (as recorded: its own rate and channels)
	Measure r;
	ma_decoder_config cfg = ma_decoder_config_init (ma_format_f32, 0, 0);
	ma_decoder dec;
	if (ma_decoder_init_file (path.c_str (), &cfg, &dec) == MA_SUCCESS) {
		ma_uint64 n = 0;
		const unsigned ch = dec.outputChannels, rate = dec.outputSampleRate;
		if (ma_decoder_get_length_in_pcm_frames (&dec, &n) == MA_SUCCESS && n > 0 && n < (ma_uint64)rate * 600 && ch > 0) {
			std::vector<float> buf ((size_t)n * ch);
			ma_uint64 got = 0;
			ma_decoder_read_pcm_frames (&dec, buf.data (), n, &got);
			r.lufs = Loudness (buf.data (), (size_t)got, ch, rate);
			for (size_t i = 0; i < (size_t)got * ch; i++) r.peak = std::max (r.peak, (double)std::fabs (buf[i]));
		}
		ma_decoder_uninit (&dec);
	}
	std::lock_guard<std::mutex> l (m);
	measures[path] = r;
	return r;
}

void Assets::SetAsync (bool on)
{
	if (!on) {
		// (what is queued: loaded now)
		std::unique_lock<std::mutex> l (m);
		while (!jobs.empty () || std::any_of (clips.begin (), clips.end (), [](auto &kv) { return kv.second.loading; })) {
			if (!worker.joinable () && !jobs.empty ()) {
				// (no worker: the queue's jobs done here)
				Job j = jobs.front ();
				jobs.pop_front ();
				Run (l, j);
				continue;
			}
			done.wait (l);
		}
	}
	async = on;
}

void Assets::Forget ()
{
	std::lock_guard<std::mutex> l (m);
	paths.clear ();
	for (auto it = clips.begin (); it != clips.end (); )
		it = (!it->second.loading && (!it->second.clip || it->second.clip.use_count () == 1)) ? clips.erase (it) : std::next (it);
}

void Assets::Shutdown ()
{
	{
		std::lock_guard<std::mutex> l (m);
		stopping = true;
		// (jobs not begun: never loaded)
		for (auto &j : jobs) if (!j.measure) { Entry &e = clips[j.key]; e.loading = false; e.failed = true; }
		jobs.clear ();
		cv.notify_all ();
		done.notify_all ();
	}
	if (worker.joinable ()) worker.join ();
	std::lock_guard<std::mutex> l (m);
	for (auto it = clips.begin (); it != clips.end (); )
		it = it->second.failed && !it->second.clip ? clips.erase (it) : std::next (it);
	stopping = false;
}

// ---- a clip as a sound --------------------------------------------------------

struct ClipSound::Ref {
	ma_audio_buffer_ref buf;
};

bool ClipSound::Init (ma_engine *engine, ClipRef c, ma_sound_group *group, unsigned channelsOut, bool loop)
{
	Uninit ();
	if (!engine || !c || !c->frames) return false;
	clip = std::move (c);
	ref = std::make_unique<Ref>();
	if (ma_audio_buffer_ref_init (ma_format_f32, clip->channels, clip->pcm.data (), clip->frames, &ref->buf) != MA_SUCCESS) {
		ref.reset (), clip.reset ();
		return false;
	}
	ref->buf.sampleRate = clip->rate;
	sound = std::make_unique<ma_sound>();
	ma_sound_config sc = ma_sound_config_init_2 (engine);
	sc.pDataSource = &ref->buf;
	sc.pInitialAttachment = group;
	sc.channelsOut = channelsOut;
	sc.flags = MA_SOUND_FLAG_NO_SPATIALIZATION | (group ? 0 : MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT);
	sc.isLooping = loop ? MA_TRUE : MA_FALSE;
	if (ma_sound_init_ex (engine, &sc, sound.get ()) != MA_SUCCESS) {
		ma_audio_buffer_ref_uninit (&ref->buf);
		sound.reset (), ref.reset (), clip.reset ();
		return false;
	}
	valid = true;
	return true;
}

ClipSound::ClipSound () = default;

ClipSound::~ClipSound ()
{
	Uninit ();
}

void ClipSound::Uninit ()
{
	if (valid) ma_sound_uninit (sound.get ());   // (out of the graph: the audio thread done with it)
	if (ref) ma_audio_buffer_ref_uninit (&ref->buf);
	sound.reset (), ref.reset (), clip.reset ();
	valid = false;
}

} // namespace audio

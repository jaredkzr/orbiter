// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The audio thread's rules, for every synthesizer and the path node:
//  - rendering never allocates (a mix of every source, binaural, events
//    fired, the music changing mood: no operator new while it renders)
//  - sources come and go while a real audio thread renders them (the null
//    backend's device thread): created, played and destroyed in a churn.
//    Built again with ThreadSanitizer (Audio.ChurnTSan) for the races.

#include <catch2/catch_test_macros.hpp>
#include "Audio/AudioEngine.h"
#include "Audio/PathNode.h"
#include "Audio/RocketSynth.h"
#include "Audio/RcsSynth.h"
#include "Audio/WindSynth.h"
#include "Audio/BoomSynth.h"
#include "Audio/ContactVoices.h"
#include "Audio/BlastSynth.h"
#include "Audio/FireSynth.h"
#include "Audio/SplashSynth.h"
#include "Audio/ThunderSynth.h"
#include "Audio/CreakSynth.h"
#include "Audio/AmbienceSynth.h"
#include "Audio/MusicSynth.h"
#include "Audio/Radio.h"
#include "Audio/Hrtf.h"
#include "Audio/Assets.h"
#include "Audio/ClipSynth.h"
#include "miniaudio.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <thread>
#include <vector>

using namespace audio;

// ---- counting allocations on a thread ---------------------------------------

namespace {
	thread_local bool g_watch = false;
	std::atomic<long> g_allocs{0};
}

#ifndef AUDIO_TSAN // (ThreadSanitizer has its own operator new)
void *operator new (size_t n)
{
	if (g_watch) g_allocs.fetch_add (1, std::memory_order_relaxed);
	if (void *p = std::malloc (n ? n : 1)) return p;
	throw std::bad_alloc ();
}
void *operator new[] (size_t n) { return operator new (n); }
void operator delete (void *p) noexcept { std::free (p); }
void operator delete[] (void *p) noexcept { std::free (p); }
void operator delete (void *p, size_t) noexcept { std::free (p); }
void operator delete[] (void *p, size_t) noexcept { std::free (p); }
#endif

namespace {

/// Every kind of source, each into a path node of its own on the outside bus
struct Zoo {
	struct Item { SourcePtr<Source> src; std::unique_ptr<PathNode> node; };
	std::vector<Item> items;
	RocketSynth *rocket = nullptr;
	BlastSynth *blast = nullptr;
	ThunderSynth *thunder = nullptr;
	CreakSynth *creak = nullptr;
	ContactVoices *contact = nullptr;
	std::shared_ptr<Clip> blow;   // (the contacts' take: a decaying noise burst, as a recording)
	SplashSynth *splash = nullptr;
	MusicSynth *music = nullptr;

	template <class T>
	T *Add (Engine &e, SourcePtr<T> s, bool binaural, float az)
	{
		if (!s) return nullptr;
		T *raw = s.get ();
		auto node = std::make_unique<PathNode>();
		if (!node->Init (e.Ma ())) return nullptr;
		ma_node_attach_output_bus (node->Node (), PathNode::AIR, e.Group (EXTERIOR), 0);
		ma_node_attach_output_bus (node->Node (), PathNode::HULL, e.Group (INTERIOR), 0);
		if (!s->Attach (node->Node ())) return nullptr;
		PathNode::Target t;
		t.gain = 0.5f, t.cutoff = 8000.0f, t.pan = std::sin (az);
		if (binaural) t.dir[0] = std::sin (az), t.dir[2] = std::cos (az);
		node->Set (PathNode::AIR, t);
		node->Set (PathNode::HULL, t);
		static_cast<Source*> (s.get ())->Start (); // (some synths hide Start with their own)
		items.push_back ({SourcePtr<Source> (s.release ()), std::move (node)});
		return raw;
	}

	void Build (Engine &e, uint32_t seed, bool binaural)
	{
		float az = 0.0f;
		auto next = [&az] () { return az += 0.7f; };
		rocket = Add (e, RocketSynth::Make (e, seed++), binaural, next ());
		if (rocket) { RocketSynth::Params p; p.level = 0.8f; p.whine = 0.15f; rocket->Set (p); }
		if (auto *r = Add (e, RcsSynth::Make (e, seed++), binaural, next ())) r->Set (1.0f, 200.0f);
		if (auto *w = Add (e, WindSynth::Make (e, seed++), binaural, next ())) { WindSynth::Params p; p.speed = 150.0f; p.buffet = 0.3f; w->Set (p); }
		Add (e, BoomSynth::Make (e, 0.2f, seed++), binaural, next ());
		if (!blow) {
			blow = std::make_shared<Clip> ();
			blow->channels = 1, blow->rate = e.SampleRate (), blow->frames = e.SampleRate () / 2;
			blow->pcm.resize (blow->frames);
			uint32_t r = 12345u;
			for (size_t i = 0; i < blow->pcm.size (); i++) {
				r = r * 1664525u + 1013904223u;
				blow->pcm[i] = ((r >> 8) * (1.0f / 8388608.0f) - 1.0f) * std::exp (-6.0f * i / blow->pcm.size ());
			}
		}
		contact = Add (e, ContactVoices::Make (e, ContactVoices::AIR, 15.0, seed++), binaural, next ());
		if (contact) {
			contact->SetBedTakes (ContactVoices::ROLL, blow.get (), 1.0f);
			contact->SetBed (ContactVoices::ROLL, 0.3f, 1.0f);
			contact->Fire (ContactVoices::Shot (ContactVoices::IMPACT, blow.get (), 0.5f, 1.0f, 8000.0f, 1.0f), 0.0f);
		}
		blast = Add (e, BlastSynth::Make (e, seed++), binaural, next ());
		if (blast) { blast->Set (500.0, 0.01); blast->Fire (); }
		if (auto *f = Add (e, FireSynth::Make (e, seed++), binaural, next ())) f->Set (FireSynth::PeakFor (1e8), FireSynth::DiameterFor (1e8), 1.0);
		splash = Add (e, SplashSynth::Make (e, seed++), binaural, next ());
		if (splash) { splash->Set (5.0f, 20.0f, 0.5f); splash->Entry (1.0f, 20.0f, 10.0f); }
		std::vector<float> tb ((size_t)e.SampleRate (), 0.0f);
		for (size_t i = 0; i < tb.size (); i++) tb[i] = 0.1f * std::sin (0.01f * i) * std::exp (-3.0f * i / tb.size ());
		thunder = Add (e, ThunderSynth::Make (e, std::move (tb)), binaural, next ());
		if (thunder) thunder->Fire ();
		creak = Add (e, CreakSynth::Make (e, seed++), binaural, next ());
		if (creak) creak->Strike (CreakSynth::GROAN, 1.0f);
		if (auto *a = Add (e, AmbienceSynth::Make (e, seed++), binaural, next ())) a->Set (0.05, 5.0, 0.02, 8.0);
		std::vector<float> voice ((size_t)(1.5 * e.SampleRate ()));
		for (size_t i = 0; i < voice.size (); i++) voice[i] = 0.2f * std::sin (0.05f * i) * std::sin (0.0007f * i);
		Add (e, RadioSynth::Make (e, std::move (voice), true, 25.0f, seed++), binaural, next ());
		// (the music: on its bus, as the game has it)
		auto m = MusicSynth::Make (e, seed++);
		if (m && m->Attach (e.Group (MUSIC))) {
			music = m.get ();
			music->SetMood (music::ASCENT);
			music->SetIntensity (0.7f);
			music->Start ();
			items.push_back ({SourcePtr<Source> (m.release ()), nullptr});
		}
	}

	/// Gone in the given order (as World::Remove: the source, then its node)
	void Clear (uint32_t shuffle)
	{
		for (size_t i = items.size (); i > 1; i--) {
			shuffle = shuffle * 1664525u + 1013904223u;
			std::swap (items[i - 1], items[(shuffle >> 8) % i]);
		}
		for (auto &it : items) { it.src.reset (); if (it.node) it.node->Uninit (); }
		items.clear ();
		rocket = nullptr, blast = nullptr, thunder = nullptr, creak = nullptr, contact = nullptr, splash = nullptr, music = nullptr;
	}
};

const Hrtf *LoadHrtf (Hrtf &h, unsigned rate)
{
	const char *f = "Sound/HRTF/kemar.hrtf";
	return std::filesystem::exists (f) && h.Load (f, rate) ? &h : nullptr;
}

} // namespace

TEST_CASE ("Rendering allocates nothing on the audio thread", "[audio][realtime]")
{
	Engine e;
	Engine::Options opt;
	opt.mode = Engine::OFFLINE;
	REQUIRE (e.Init (opt));
	Hrtf h;
	const Hrtf *hrtf = LoadHrtf (h, e.SampleRate ());
	PathNode::SetHrtf (hrtf);
	if (!hrtf) WARN ("no HRTFs (Sound/HRTF/kemar.hrtf): panning only");
	Zoo z;
	z.Build (e, 11, hrtf != nullptr);
	REQUIRE (z.items.size () >= 13);

	std::vector<float> buf (1024 * e.Channels ());
	long allocs = 0;
	// (45 s: the music's chords, bells and pulse many times, a mood change; events again)
	for (int b = 0; b < 45 * 48000 / 1024; b++) {
		if (b == 600 && z.music) z.music->SetMood (music::ORBIT);
		if (b % 200 == 0) {
			if (z.creak) z.creak->Strike (CreakSynth::CREAK, 0.8f);
			if (z.contact) z.contact->Fire (ContactVoices::Shot (ContactVoices::IMPACT, z.blow.get (), 0.5f, 1.0f, 8000.0f, 1.0f), 0.0f);
			if (z.splash) z.splash->Entry (0.8f, 20.0f, 8.0f);
		}
		g_allocs.store (0);
		g_watch = true;
		e.Render (buf.data (), 1024);
		g_watch = false;
		allocs += g_allocs.load ();
	}
	CHECK (allocs == 0);
	z.Clear (1);
	PathNode::SetHrtf (nullptr);
}

TEST_CASE ("Sources come and go while the audio thread renders", "[audio][realtime]")
{
	Engine e;
	Engine::Options opt;
	opt.mode = Engine::DEVICE;
	opt.nullDevice = true;
	if (!e.Init (opt)) { WARN ("no null device: skipped"); return; }
	Hrtf h;
	const Hrtf *hrtf = LoadHrtf (h, e.SampleRate ());
	PathNode::SetHrtf (hrtf);
	const uint64_t t0 = e.Time ();
	uint32_t rng = 12345;
	for (int round = 0; round < 60; round++) {
		Zoo z;
		z.Build (e, 100 + round, hrtf && (round & 1));
		// (a moment of rendering: 0 to 8 ms, some blocks or none)
		rng = rng * 1664525u + 1013904223u;
		std::this_thread::sleep_for (std::chrono::microseconds ((rng >> 8) % 8000));
		if (z.thunder) z.thunder->Fire ();
		z.Clear (rng);
	}
	// (the device thread was rendering all along)
	std::this_thread::sleep_for (std::chrono::milliseconds (50));
	CHECK (e.Time () > t0);
	e.Shutdown ();
	PathNode::SetHrtf (nullptr);
}

TEST_CASE ("Sound files are found, decoded once, in the background", "[audio][assets]")
{
	Assets &a = Assets::Get ();
	// (named Windows' way: any case, backslashes)
	const std::string p = a.Resolve ("sound\\CLICKS\\Ui_1.FLAC");
	REQUIRE (p == "Sound/Clicks/ui_1.flac");
	CHECK (a.Resolve ("Sound/Clicks/none.flac").empty ());
	a.Forget ();
	a.SetAsync (true);
	const int before = a.Decoded ();
	a.Prefetch (p, 48000, 0, true);
	bool failed = false;
	ClipRef c;
	for (int i = 0; i < 2000 && !(c = a.Peek (p, 48000, 0, failed)) && !failed; i++)
		std::this_thread::sleep_for (std::chrono::milliseconds (1));
	REQUIRE (c);
	CHECK (c->rate == 48000);
	CHECK (c->frames > 0);
	CHECK (std::isfinite (a.Measured (p).lufs));
	// (again: the same clip, not decoded again)
	CHECK (a.Load (p, 48000) == c);
	CHECK (a.Decoded () == before + 1);
	// (not a sound file: fails, nothing waits on it)
	a.Prefetch ("Sound/HRTF/kemar.hrtf", 48000);
	ClipRef bad;
	for (int i = 0; i < 2000 && !(bad = a.Peek ("Sound/HRTF/kemar.hrtf", 48000, 0, failed)) && !failed; i++)
		std::this_thread::sleep_for (std::chrono::milliseconds (1));
	CHECK (failed);
	a.Shutdown ();
	a.SetAsync (false);
}

TEST_CASE ("A pause holds the simulation's sounds where they are", "[audio][pause]")
{
	Engine e;
	Engine::Options opt;
	opt.mode = Engine::OFFLINE;
	REQUIRE (e.Init (opt));
	Assets::Get ().SetAsync (false);
	ClipRef clip = Assets::Get ().Load ("Sound/Voice/Warning Hull Temperature Critical.flac", e.SampleRate ());
	REQUIRE (clip);
	ClipSound s;
	REQUIRE (s.Init (e.Ma (), clip, e.Group (INTERIOR), 0, false));
	ma_sound_start (s.Sound ());
	std::vector<float> buf (480 * e.Channels ());
	auto cursor = [&] { float c = 0.0f; ma_sound_get_cursor_in_seconds (s.Sound (), &c); return (double)c; };
	auto play = [&] (double seconds) {
		double peak = 0.0;
		for (int b = 0; b < (int)(seconds * 100); b++) {
			e.Render (buf.data (), 480);
			e.Suspend ();
			for (float x : buf) peak = std::max (peak, (double)std::fabs (x));
		}
		return peak;
	};
	CHECK (play (0.5) > 0.0);
	CHECK (std::fabs (cursor () - 0.5) < 0.02);
	// paused: faded out (0.15 s), then held
	e.SetSimFade (0.0f, 0.15);
	play (0.3);
	CHECK (e.Suspended ());
	const double held = cursor ();
	CHECK (held < 0.5 + 0.3);
	CHECK (play (2.0) == 0.0);              // (silent)
	CHECK (cursor () == held);              // (where it was: not played on unheard)
	CHECK (!ma_sound_at_end (s.Sound ()));
	// going on: from there, faded back in
	e.SetSimFade (1.0f, 0.4);
	CHECK (!e.Suspended ());
	CHECK (play (0.5) > 0.0);
	CHECK (std::fabs (cursor () - (held + 0.5)) < 0.02);
	// (the interface's bus plays on in a pause: never held)
	e.SetSimFade (0.0f, 0.15);
	ClipSound ui;
	REQUIRE (ui.Init (e.Ma (), clip, e.Group (UI), 0, false));
	ma_sound_start (ui.Sound ());
	play (1.0);
	float c = 0.0f;
	ma_sound_get_cursor_in_seconds (ui.Sound (), &c);
	CHECK (c > 0.9f);
	s.Uninit (), ui.Uninit ();
}

namespace {
	/// A constant signal (1): a path's level read off its output
	struct OneSynth : SynthSource {
		static SourcePtr<OneSynth> Make (Engine &e) { SourcePtr<OneSynth> s (new OneSynth ()); if (!s->Init (e)) return nullptr; return s; }
	protected:
		void Render (float *out, unsigned n, float) override { for (unsigned i = 0; i < n; i++) out[i] = 1.0f; }
	};
}

TEST_CASE ("A path's level ramps over the simulation's step, not a block", "[audio][ramp]")
{
	Engine e;
	Engine::Options opt;
	opt.mode = Engine::OFFLINE;
	opt.ears = false;
	REQUIRE (e.Init (opt));
	PathNode node;
	REQUIRE (node.Init (e.Ma (), 0.0f));   // (no DC block: the constant through)
	ma_node_attach_output_bus (node.Node (), PathNode::AIR, e.Group (UI), 0);
	auto s = OneSynth::Make (e);
	REQUIRE (s);
	REQUIRE (s->Attach (node.Node ()));
	s->Start ();
	std::vector<float> buf (480 * e.Channels ());
	auto block = [&] { e.Render (buf.data (), 480); return buf[(480 - 1) * e.Channels ()] / 0.70710678f; }; // (left, at the block's end: pan 0)
	// a level reached over a step of 20 ms (960 frames): half way after a block, there after two
	PathNode::Target t;
	t.gain = 1.0f;
	node.Set (PathNode::AIR, t, 0.02);
	CHECK (std::fabs (block () - 0.5f) < 0.02f);
	CHECK (std::fabs (block () - 1.0f) < 0.02f);
	CHECK (std::fabs (block () - 1.0f) < 0.02f);   // (held)
	// no ramp: there within a block, as before
	t.gain = 0.25f;
	node.Set (PathNode::AIR, t);
	CHECK (std::fabs (block () - 0.25f) < 0.02f);
	// a new target half way: from where it is
	t.gain = 1.0f;
	node.Set (PathNode::AIR, t, 0.02);
	const float half = block ();
	t.gain = 0.0f;
	node.Set (PathNode::AIR, t, 0.01);
	CHECK (std::fabs (half - 0.625f) < 0.02f);
	CHECK (std::fabs (block () - 0.0f) < 0.02f);
	s.reset ();
	node.Uninit ();
}

TEST_CASE ("Two blows in one block: two", "[audio][events]")
{
	auto energy = [] (int strikes) {
		Engine e;
		Engine::Options opt;
		opt.mode = Engine::OFFLINE;
		opt.ears = false;
		e.Init (opt);
		PathNode node;
		node.Init (e.Ma (), 0.0f);
		ma_node_attach_output_bus (node.Node (), PathNode::AIR, e.Group (UI), 0);
		auto s = CreakSynth::Make (e, 5);
		s->Attach (node.Node ());
		static_cast<Source*> (s.get ())->Start ();
		PathNode::Target t;
		t.gain = 1.0f;
		node.Set (PathNode::AIR, t);
		for (int i = 0; i < strikes; i++) s->Strike (CreakSynth::GROAN, 1.0f);   // (before one block: both in it)
		std::vector<float> buf (480 * e.Channels ());
		double sum = 0.0;
		for (int b = 0; b < 200; b++) {
			e.Render (buf.data (), 480);
			for (float x : buf) sum += (double)x * x;
		}
		s.reset ();
		node.Uninit ();
		return sum;
	};
	const double one = energy (1), two = energy (2);
	CHECK (one > 0.0);
	CHECK (two > 1.2 * one);
}

TEST_CASE ("A recording on an emitter: its pitch glides, it seeks, loops, ends", "[audio][clip]")
{
	Engine e;
	Engine::Options opt;
	opt.mode = Engine::OFFLINE;
	opt.ears = false;
	REQUIRE (e.Init (opt));
	auto clip = std::make_shared<Clip>();
	clip->channels = 1, clip->rate = 48000, clip->frames = 48000;   // (a second of a 200 Hz tone)
	clip->pcm.resize (48000);
	for (size_t i = 0; i < 48000; i++) clip->pcm[i] = 0.5f * std::sin (6.2831853f * 200.0f * i / 48000.0f);
	PathNode node;
	REQUIRE (node.Init (e.Ma (), 0.0f));
	ma_node_attach_output_bus (node.Node (), PathNode::AIR, e.Group (UI), 0);
	PathNode::Target t;
	t.gain = 1.0f;
	node.Set (PathNode::AIR, t);
	auto s = ClipSynth::Make (e, clip, false);
	REQUIRE (s);
	REQUIRE (s->Attach (node.Node ()));
	static_cast<Source*> (s.get ())->Start ();
	std::vector<float> buf (480 * e.Channels ());
	auto block = [&] { e.Render (buf.data (), 480); return s->Cursor (); };
	for (int i = 0; i < 10; i++) block ();
	CHECK (std::fabs (s->Cursor () - 0.1) < 1e-3);
	// its pitch doubled: across one block (1 to 2: 1.5 on average), then at 2
	s->SetPitch (2.0f);
	const double c0 = s->Cursor (), c1 = block (), c2 = block ();
	CHECK (std::fabs ((c1 - c0) - 0.015) < 2e-4);
	CHECK (std::fabs ((c2 - c1) - 0.020) < 2e-4);
	// to half way; to its end (not looped): done
	s->Seek (0.5);
	CHECK (std::fabs (block () - 0.52) < 2e-3);
	for (int i = 0; i < 60 && !s->Done (); i++) block ();
	CHECK (s->Done ());
	// looped: on and on
	auto l = ClipSynth::Make (e, clip, true);
	REQUIRE (l->Attach (node.Node ()));
	static_cast<Source*> (l.get ())->Start ();
	for (int i = 0; i < 300; i++) e.Render (buf.data (), 480);
	CHECK (!l->Done ());
	CHECK (l->Cursor () < 1.0);
	s.reset (), l.reset ();
	node.Uninit ();
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The contacts' shared parts, without Orbiter:
//  - one-shots on an emitter (Emitter::Schedule): released when their sound
//    arrives - a travel time later through the air, at once through the
//    structure - in order, each once, timed within the step; offered again
//    when the source has no room; gone when time goes back
//  - an emitter heard only through its structure (StructureEmitter)
//  - recordings in takes and tiers (SampleBank): each in turn, never twice
//    running, a little off in rate and gain, repeatably; a layer not there
//    silent; strengths as tiers
//  - ContactVoices: one-shots played, the quietest's voice taken, the
//    body's modes by the vessel's size (not rung: off), nothing
//    allocated rendering, its time per block

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "Audio/AudioEngine.h"
#include "Audio/World.h"
#include "Audio/SampleBank.h"
#include "Audio/ContactVoices.h"
#include "Audio/Ears.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <new>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <vector>

using namespace audio;

// ---- counting allocations on a thread (as Audio.Churn) ---------------------------
namespace {
	thread_local bool g_watch = false;
	std::atomic<long> g_allocs{0};
}
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

using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

namespace {

int bodyA, planet;
const Body *A = reinterpret_cast<const Body*> (&bodyA);
const void *PLANET = &planet;

/// A scene of our own (as Audio.World's): bodies at given places, sea-level air
struct TestScene : Scene {
	double t = 0.0;
	std::map<const Body*, V3> at;
	bool PoseOf (const Body *b, Pose &p) const override
	{
		auto it = at.find (b);
		if (it == at.end ()) return false;
		p.pos = it->second;
		return true;
	}
	const void *StructureOf (const Body *b) const override { return b; }
	double SizeOf (const Body *) const override { return 5.0; }
	Air AirAt (const void *frame, const V3 &) const override { return frame ? Air::SeaLevel () : Air (); }
	Snapshot At (double time, const V3 &ear, double dt, bool inside = false, const void *of = nullptr)
	{
		t = time;
		Snapshot s;
		s.t = s.playT = time;
		s.dt = dt;
		s.listener = true;
		s.ear.pos = ear;
		s.frame = PLANET;
		s.air = Air::SeaLevel ();
		s.inside = inside, s.insideOf = of;
		return s;
	}
};

struct Fixture {
	Engine e;
	Fixture () { Engine::Options o; o.mode = Engine::OFFLINE; o.ears = false; e.Init (o); }
};

/// A source that writes down the one-shots it is given (and can refuse some)
class Recorder : public SynthSource {
public:
	struct Got { double at; int kind; float delay; };
	std::vector<Got> got;
	double now = 0.0;
	int refuse = 0;      // (refuse this many first)
	static SourcePtr<Recorder> Make (Engine &e)
	{
		SourcePtr<Recorder> r (new Recorder ());
		if (!r->Init (e)) return nullptr;
		return r;
	}
	bool Fire (const OneShot &s, float delay) override
	{
		if (refuse > 0) { refuse--; return false; }
		got.push_back ({now, s.kind, delay});
		return true;
	}
protected:
	void Render (float *out, unsigned n, float) override { for (unsigned i = 0; i < n; i++) out[i] = 0.0f; }
};

OneShot Kind (int k) { OneShot s; s.kind = k; return s; }

/// A mono 16-bit WAV file
void WriteWav (const std::string &path, const std::vector<float> &x)
{
	std::ofstream f (path, std::ios::binary);
	auto u32 = [&](uint32_t v) { f.write ((const char*)&v, 4); };
	auto u16 = [&](uint16_t v) { f.write ((const char*)&v, 2); };
	f.write ("RIFF", 4); u32 (36 + 2 * (uint32_t)x.size ()); f.write ("WAVE", 4);
	f.write ("fmt ", 4); u32 (16); u16 (1); u16 (1); u32 (48000); u32 (96000); u16 (2); u16 (16);
	f.write ("data", 4); u32 (2 * (uint32_t)x.size ());
	for (float v : x) { int16_t s = (int16_t)std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f); f.write ((const char*)&s, 2); }
}

/// A blow's recording: a decaying noise burst (peak about 1)
std::vector<float> Blow (unsigned seed, double len = 0.15, double tau = 0.03)
{
	dsp::Rng rng (seed);
	std::vector<float> x ((size_t)(len * 48000));
	for (size_t i = 0; i < x.size (); i++) x[i] = 0.9f * rng.White () * (float)std::exp (-(double)i / (tau * 48000));
	return x;
}

double Energy (const std::vector<float> &x) { double e = 0.0; for (float v : x) e += (double)v * v; return e / 48000.0; }

} // namespace

// ---- one-shots on an emitter --------------------------------------------------

TEST_CASE ("One-shots: released when their sound arrives, in order, each once", "[audio][contacts]")
{
	Fixture f;
	World w (f.e);
	TestScene sc;
	const double c = Air::SeaLevel ().c;
	sc.at[A] = V3 (2.0 * c, 0, 0);    // (2 s away)
	auto rec = Recorder::Make (f.e);
	Recorder *r = rec.get ();
	EmitterDesc d;
	d.body = A, d.level = 1.0f;
	EmitterId id = w.Add (d, std::move (rec));
	Emitter *em = w.Get (id);
	REQUIRE (em);
	const double dt = 0.02;
	em->Schedule (1.0, Kind (1));
	em->Schedule (1.0, Kind (2));     // (two at one moment: two)
	em->Schedule (1.505, Kind (3));
	em->Schedule (0.5, Kind (0));     // (scheduled late, out of order: in its place)
	r->refuse = 1;                    // (the first offer refused: offered again)
	for (int i = 0; i <= 300; i++) {
		const double t = i * dt;
		r->now = t;
		w.Update (sc.At (t, V3 (), dt), sc);
	}
	REQUIRE (r->got.size () == 4);
	CHECK (r->got[0].kind == 0);
	CHECK (r->got[1].kind == 1);
	CHECK (r->got[2].kind == 2);
	CHECK (r->got[3].kind == 3);
	// each when its sound arrived: 2 s after (the refused one a step later)
	CHECK_THAT (r->got[0].at, WithinAbs (2.5 + dt, dt * 1.01));
	CHECK_THAT (r->got[1].at, WithinAbs (3.0, dt * 1.01));
	CHECK_THAT (r->got[3].at, WithinAbs (3.505, dt * 1.01));
	// timed within the step: 1.505 heard between two updates - a quarter
	// step after the previous update's, played a quarter step in
	CHECK_THAT (r->got[3].delay, WithinAbs (0.25 * dt, 2e-4));
	for (auto &g : r->got) CHECK ((g.delay >= 0.0f && g.delay <= dt + 1e-6));
	CHECK (em->Pending () == 0);
}

TEST_CASE ("One-shots: at once through the structure; gone when time goes back", "[audio][contacts]")
{
	Fixture f;
	World w (f.e);
	TestScene sc;
	sc.at[A] = V3 (3, 0, 0);
	auto rec = Recorder::Make (f.e);
	Recorder *r = rec.get ();
	EmitterId id = w.Add (StructureEmitter (A, 0.2, "structure"), std::move (rec));
	Emitter *em = w.Get (id);
	REQUIRE (em);
	const double dt = 0.02;
	// in the cabin of its structure: at once (no travel through the air)
	em->Schedule (0.1, Kind (1));
	for (int i = 0; i <= 10; i++) { r->now = i * dt; w.Update (sc.At (i * dt, V3 (), dt, true, A), sc); }
	REQUIRE (r->got.size () == 1);
	CHECK_THAT (r->got[0].at, WithinAbs (0.1, 1e-9));
	// one in the future, then time goes back (a replay): never played
	em->Schedule (5.0, Kind (2));
	CHECK (em->Pending () == 1);
	r->now = 0.0;
	w.Update (sc.At (0.0, V3 (), dt, true, A), sc);
	CHECK (em->Pending () == 0);
	for (int i = 0; i < 400; i++) { r->now = i * dt; w.Update (sc.At (i * dt, V3 (), dt, true, A), sc); }
	CHECK (r->got.size () == 1);
}

TEST_CASE ("A structure's emitter: heard in its cabin, not outside", "[audio][contacts]")
{
	Fixture f;
	World w (f.e);
	TestScene sc;
	sc.at[A] = V3 (2, 0, 0);
	const double cabin = 0.5;   // [Pa] its full scale in the cabin
	EmitterId id = w.Add (StructureEmitter (A, cabin, "structure"), MakeToneSource (f.e, 100.0, 0.5));
	REQUIRE (w.Get (id));
	w.Update (sc.At (0.0, V3 (), 0.02, true, A), sc);
	CHECK_THAT (w.Get (id)->Heard ().hull, WithinRel ((float)(cabin / P_FULL), 1e-4));
	// outside, 20 m off: some 70 dB under
	sc.at[A] = V3 (20, 0, 0);
	w.Update (sc.At (0.02, V3 (), 0.02), sc);
	w.Update (sc.At (0.04, V3 (), 0.02), sc);
	CHECK (w.Get (id)->Heard ().hull == 0.0f);
	CHECK (w.Get (id)->Heard ().air < 3e-4f * (float)(cabin / P_FULL));
}

// ---- recordings in takes and tiers -------------------------------------------

namespace {
struct BankDir {
	std::filesystem::path dir;
	BankDir ()
	{
		dir = std::filesystem::temp_directory_path () / ("samplebank_" + std::to_string ((unsigned long)std::chrono::steady_clock::now ().time_since_epoch ().count ()));
		std::filesystem::create_directories (dir / "impact_gear");
		std::filesystem::create_directories (dir / "rattle");
		std::ofstream m (dir / "contacts.txt");
		m << "# a test bank\n";
		unsigned seed = 1;
		for (const char *tier : {"soft", "mid", "hard"})
			for (int i = 1; i <= 4; i++) {
				std::string f = std::string ("impact_gear/impact_gear_") + tier + "_" + std::to_string (i) + ".wav";
				auto x = Blow (seed++);
				WriteWav ((dir / f).string (), x);
				m << f << " impact_gear " << tier << " " << Energy (x) << "   # take\n";
			}
		for (int i = 1; i <= 2; i++) {
			std::string f = "rattle/rattle_" + std::to_string (i) + ".wav";
			auto x = Blow (100 + i, 0.5, 0.2);
			WriteWav ((dir / f).string (), x);
			m << f << " rattle - " << Energy (x) << "\n";
		}
		m << "impact_gear/missing.wav impact_gear hard 0.1\n";   // (not there: passed over)
	}
	~BankDir () { std::error_code ec; std::filesystem::remove_all (dir, ec); }
	std::string Manifest () const { return (dir / "contacts.txt").string (); }
};
}

TEST_CASE ("SampleBank: takes in turn, never twice running, a little off", "[audio][contacts]")
{
	Assets::Get ().SetAsync (false);
	BankDir bd;
	SampleBank b (7);
	REQUIRE (b.Load (bd.Manifest (), 48000));
	CHECK (b.Has ("impact_gear"));
	CHECK (b.Count ("impact_gear", SampleBank::HARD) == 4);
	CHECK (b.Count ("impact_gear") == 12);
	CHECK (b.Count ("rattle") == 2);
	std::string last;
	std::map<std::string, int> seen;
	for (int round = 0; round < 50; round++) {
		std::set<std::string> inRound;
		for (int i = 0; i < 4; i++) {
			SampleBank::Take k = b.Pick ("impact_gear", SampleBank::MID);
			REQUIRE (k);
			CHECK (k.file.find ("_mid_") != std::string::npos);
			CHECK (k.file != last);                 // (never twice running)
			inRound.insert (k.file);
			seen[k.file]++;
			last = k.file;
			CHECK ((k.rate >= 0.94f && k.rate <= 1.06f));
			CHECK ((k.gain >= 0.84f && k.gain <= 1.19f));   // (+-1.5 dB)
			CHECK (k.energy > 0.0);
			CHECK (k.clip->channels == 1);
		}
		CHECK (inRound.size () == 4);               // (each in turn)
	}
	CHECK (seen.size () == 4);
	// the energy as listed
	SampleBank::Take r = b.Pick ("rattle");
	REQUIRE (r);
	CHECK_THAT (r.energy, WithinRel (Energy (Blow (r.file.find ("_1.") != std::string::npos ? 101 : 102, 0.5, 0.2)), 1e-3));
	// repeatable: the same seed, the same takes and jitter
	SampleBank b1 (3), b2 (3);
	REQUIRE (b1.Load (bd.Manifest (), 48000));
	REQUIRE (b2.Load (bd.Manifest (), 48000));
	for (int i = 0; i < 20; i++) {
		SampleBank::Take x = b1.Pick ("impact_gear", SampleBank::SOFT), y = b2.Pick ("impact_gear", SampleBank::SOFT);
		CHECK (x.file == y.file);
		CHECK (x.rate == y.rate);
		CHECK (x.gain == y.gain);
	}
	// a layer not there: silent, never another's sound
	CHECK_FALSE (b.Pick ("strut"));
	CHECK_FALSE (b.Pick ("rattle", SampleBank::HARD));
	// none at all
	SampleBank none;
	CHECK_FALSE (none.Load ((bd.dir / "nothing.txt").string (), 48000));
	CHECK_FALSE (none.Pick ("impact_gear"));
}

TEST_CASE ("SampleBank: strengths as tiers, crossfading at their edges", "[audio][contacts]")
{
	auto b = SampleBank::ForStrength (0.0f);
	CHECK (b.tier == SampleBank::SOFT);
	CHECK (b.w == 0.0f);
	b = SampleBank::ForStrength (0.5f);       // (the middle of mid: mid alone)
	CHECK (b.tier == SampleBank::MID);
	CHECK_THAT (b.w, WithinAbs (0.0, 1e-6));
	b = SampleBank::ForStrength (0.6f);       // (towards hard)
	CHECK (b.tier == SampleBank::MID);
	CHECK (b.next == SampleBank::HARD);
	CHECK_THAT (b.w, WithinAbs (0.3, 1e-5));
	b = SampleBank::ForStrength (0.3f);       // (soft, near mid)
	CHECK (b.tier == SampleBank::SOFT);
	CHECK (b.next == SampleBank::MID);
	CHECK_THAT (b.w, WithinAbs (0.4, 1e-5));
	b = SampleBank::ForStrength (1.0f);
	CHECK (b.tier == SampleBank::HARD);
	CHECK (b.w == 0.0f);
	// (continuous across an edge: half and half)
	auto lo = SampleBank::ForStrength (0.6666f), hi = SampleBank::ForStrength (0.6668f);
	CHECK_THAT (lo.w, WithinAbs (0.5, 1e-3));
	CHECK_THAT (hi.w, WithinAbs (0.5, 1e-3));
	CHECK (lo.next == hi.tier);
}

// ---- the voices --------------------------------------------------------------

namespace {
std::shared_ptr<Clip> MakeClip (const std::vector<float> &x)
{
	auto c = std::make_shared<Clip>();
	c->channels = 1, c->rate = 48000, c->frames = x.size (), c->pcm = x;
	return c;
}
std::vector<float> Render (ContactVoices &v, double secs)
{
	std::vector<float> y ((size_t)(secs * 48000));
	for (size_t i = 0; i < y.size (); i += 480) v.RenderBlock (y.data () + i, (unsigned)std::min<size_t> (480, y.size () - i));
	return y;
}
}

TEST_CASE ("ContactVoices: one-shots played, the body by the vessel's size", "[audio][contacts]")
{
	Fixture f;
	CHECK_THAT (ContactVoices::BodyFreq (30.0), WithinRel (25.0, 0.01));
	CHECK_THAT (ContactVoices::BodyFreq (5.0), WithinRel (60.0, 0.01));
	auto v = ContactVoices::Make (f.e, ContactVoices::AIR, 18.0, 1);
	REQUIRE (v);
	CHECK_THAT (v->ModeFreq (0), WithinRel (ContactVoices::BodyFreq (18.0), 0.05));
	CHECK_THAT (v->ModeTau (0), WithinRel (0.6, 1e-5));
	CHECK_THAT (v->ModeTau (ContactVoices::MODES - 1), WithinRel (0.15, 1e-5));
	auto blow = MakeClip (Blow (5));
	// not ours: taken, nothing played
	OneShot other;
	other.kind = 99;
	CHECK (v->Fire (other, 0.0f));
	// a blow at gain 0.5, its recording as it is (no low-pass to speak of), no body
	REQUIRE (v->Fire (ContactVoices::Shot (ContactVoices::IMPACT, blow.get (), 0.5f, 1.0f, 20000.0f, 0.0f), 0.01f));
	auto y = Render (*v, 0.5);
	CHECK (v->Playing () == 0);
	// (10 ms late: silent till then; its energy a quarter of the recording's - a low-pass at 20 kHz, nearly all)
	double pre = 0.0;
	for (size_t i = 0; i < 470; i++) pre += std::fabs (y[i]);
	CHECK (pre == 0.0);
	CHECK_THAT (Energy (y), WithinRel (0.25 * Energy (Blow (5)), 0.15));
}

TEST_CASE ("ContactVoices: no body ringing; in the cabin the blow dulled, as loud", "[audio][contacts]")
{
	// (the body's modes are off: by ear a gong, not a hull - the recordings carry
	// their own body. Sent to it, a blow sounds as without)
	Fixture f;
	auto blow = MakeClip (Blow (9));
	auto run = [&](ContactVoices::Profile p, float send) {
		auto v = ContactVoices::Make (f.e, p, 15.0, 3);
		v->Fire (ContactVoices::Shot (ContactVoices::IMPACT, blow.get (), 1.0f, 1.0f, 20000.0f, send), 0.0f);
		return Render (*v, 1.0);
	};
	for (auto p : {ContactVoices::AIR, ContactVoices::STRUCTURE}) {
		auto dry = run (p, 0.0f), wet = run (p, 1.0f);
		double diff = 0.0;
		for (size_t i = 0; i < dry.size (); i++) diff += std::fabs (wet[i] - dry[i]);
		INFO ("profile " << p);
		CHECK (diff == 0.0);
	}
	// in the cabin: dulled (below 1.8 kHz), still most of the blow's energy (it is low)
	const double air = Energy (run (ContactVoices::AIR, 0.0f)), cab = Energy (run (ContactVoices::STRUCTURE, 0.0f));
	INFO ("the cabin's " << 10.0 * std::log10 (cab / air) << " dB against outside");
	CHECK ((cab < air && cab > 0.1 * air));
}

namespace {
/// A steady take (as a bed's): noise low-passed (smooth: a jump between
/// stretches would show), its RMS 1/sqrt(2)-ish, len s
std::vector<float> Steady (unsigned seed, double len = 2.5, float fc = 300.0f)
{
	dsp::Rng rng (seed);
	dsp::OnePole a, b;
	a.SetCutoff (fc, 48000.0f), b.SetCutoff (fc, 48000.0f);
	std::vector<float> x ((size_t)(len * 48000));
	for (auto &v : x) v = b.Lp (a.Lp (rng.White ()));
	double e = 0.0;
	for (float v : x) e += (double)v * v;
	const float k = (float)(0.3 / std::sqrt (e / x.size ()));   // (RMS 0.3)
	for (auto &v : x) v *= k;
	return x;
}
double Rms (const std::vector<float> &x, size_t a, size_t b)
{
	double e = 0.0;
	for (size_t i = a; i < b; i++) e += (double)x[i] * x[i];
	return std::sqrt (e / std::max<size_t> (1, b - a));
}
}

TEST_CASE ("ContactVoices: a chirp played only its spin-up long", "[audio][contacts]")
{
	Fixture f;
	auto v = ContactVoices::Make (f.e, ContactVoices::AIR, 15.0, 1);
	auto take = MakeClip (Steady (3, 1.0, 8000.0f));
	REQUIRE (v->Fire (ContactVoices::Shot (ContactVoices::CHIRP, take.get (), 1.0f, 1.0f, 20000.0f, 0.0f, 0.1f), 0.0f));
	auto y = Render (*v, 0.5);
	CHECK (v->Playing () == 0);
	// all of it within 0.1 s; its first 60 % at full, then faded
	CHECK (Rms (y, 4900, y.size ()) == 0.0);
	CHECK (Rms (y, 500, 2800) > 0.2);
	CHECK (Rms (y, 4300, 4800) < 0.5 * Rms (y, 500, 2800));
}

TEST_CASE ("ContactVoices: a bed at its level, grains crossfading unheard; silent at nothing", "[audio][contacts]")
{
	Fixture f;
	auto a = MakeClip (Steady (11)), b = MakeClip (Steady (12));
	const float ga = (float)(1.0 / std::sqrt (Energy (a->pcm) / 2.5)), gb = (float)(1.0 / std::sqrt (Energy (b->pcm) / 2.5));
	for (auto p : {ContactVoices::AIR, ContactVoices::STRUCTURE}) {
		INFO ("profile " << p);
		auto v = ContactVoices::Make (f.e, p, 15.0, 2);
		// no takes yet: silent whatever its level
		v->SetBed (ContactVoices::ROLL, 0.5f, 1.0f);
		auto y0 = Render (*v, 0.2);
		CHECK (Rms (y0, 0, y0.size ()) == 0.0);
		REQUIRE (v->SetBedTakes (ContactVoices::ROLL, a.get (), ga, b.get (), gb));
		auto y = Render (*v, 8.0);
		// eased up (about 0.1 s), then at its level (unit RMS x 0.5): 0.3 s windows within 2.5 dB of it,
		// over many grains and their crossfades (the cabin's: below 400 Hz - the takes are below 300)
		CHECK (Rms (y, 0, 2400) < 0.5 * 0.5);
		const double want = 0.5 * (p == ContactVoices::STRUCTURE ? 0.85 : 1.0);
		double lo = 1e9, hi = 0.0;
		for (size_t i = 48000; i + 14400 <= y.size (); i += 14400) {
			const double r = Rms (y, i, i + 14400);
			lo = std::min (lo, r), hi = std::max (hi, r);
		}
		INFO ("windows' RMS " << lo << " .. " << hi << " (want about " << want << ")");
		CHECK ((lo > want * 0.75 && hi < want * 1.33));
		// no clicks: no step between samples more than the takes' own (as smooth: a hard cut between
		// two stretches would step by about their RMS)
		float maxStep = 0.0f, ownStep = 0.0f;
		for (size_t i = 48001; i < y.size (); i++) maxStep = std::max (maxStep, std::fabs (y[i] - y[i - 1]));
		for (size_t i = 1; i < a->pcm.size (); i++) ownStep = std::max (ownStep, std::fabs (a->pcm[i] - a->pcm[i - 1]) * ga * 0.5f);
		INFO ("largest step " << maxStep << ", the take's own " << ownStep);
		CHECK (maxStep < 2.0f * ownStep);
		// to nothing: eased away (0.15 s; cut 60 dB down, about a second on), then silent
		v->SetBed (ContactVoices::ROLL, 0.0f, 1.0f);
		auto z = Render (*v, 2.0);
		CHECK (Rms (z, 0, 2400) > 0.1);
		CHECK (Rms (z, 72000, z.size ()) == 0.0);
		CHECK (v->BedLevel (ContactVoices::ROLL) == 0.0f);
	}
}

TEST_CASE ("ContactVoices: two beds, louder with a higher level; the rate followed", "[audio][contacts]")
{
	Fixture f;
	auto a = MakeClip (Steady (21, 2.5, 4000.0f));
	const float ga = (float)(1.0 / std::sqrt (Energy (a->pcm) / 2.5));
	auto v = ContactVoices::Make (f.e, ContactVoices::AIR, 15.0, 3);
	REQUIRE (v->SetBedTakes (ContactVoices::ROLL, a.get (), ga));
	REQUIRE (v->SetBedTakes (ContactVoices::BRAKE, a.get (), ga));
	v->SetBed (ContactVoices::ROLL, 0.1f, 0.8f);
	auto y1 = Render (*v, 2.0);
	v->SetBed (ContactVoices::BRAKE, 0.2f, 1.2f);
	auto y2 = Render (*v, 2.0);
	// the brake's on top: louder (uncorrelated: about sqrt(0.1^2 + 0.2^2))
	const double r1 = Rms (y1, 24000, y1.size ()), r2 = Rms (y2, 24000, y2.size ());
	INFO ("rolling " << r1 << ", with the brakes " << r2);
	CHECK_THAT (r1, WithinRel (0.1, 0.25));
	CHECK_THAT (r2, WithinRel (std::sqrt (0.05), 0.25));
}

TEST_CASE ("ContactVoices: the scrape's bed - brighter with its cutoff; in the cabin a grind", "[audio][contacts]")
{
	Fixture f;
	auto a = MakeClip (Steady (31, 2.5, 12000.0f));   // (a bright take)
	const float ga = (float)(1.0 / std::sqrt (Energy (a->pcm) / 2.5));
	// brightness: the share of its sample-to-sample change (high frequencies) against its level
	auto bright = [&](ContactVoices::Profile p, float fc) {
		auto v = ContactVoices::Make (f.e, p, 15.0, 4);
		v->SetBedTakes (ContactVoices::SCRAPE, a.get (), ga);
		v->SetBed (ContactVoices::SCRAPE, 0.3f, 1.0f, fc);
		auto y = Render (*v, 2.0);
		double d = 0.0, e = 0.0;
		for (size_t i = 24001; i < y.size (); i++) d += (double)(y[i] - y[i - 1]) * (y[i] - y[i - 1]), e += (double)y[i] * y[i];
		return std::sqrt (d / e);
	};
	const double dull = bright (ContactVoices::AIR, 800.0f), sharp = bright (ContactVoices::AIR, 12000.0f), cabin = bright (ContactVoices::STRUCTURE, 12000.0f);
	INFO ("brightness at 800 Hz " << dull << ", at 12 kHz " << sharp << ", in the cabin " << cabin);
	CHECK (sharp > 3.0 * dull);
	CHECK (cabin < 0.5 * sharp);   // (in the cabin below 600 Hz whatever its cutoff)
	// its level held (unit RMS x 0.3, the cutoff taking some of it)
	auto v = ContactVoices::Make (f.e, ContactVoices::AIR, 15.0, 5);
	v->SetBedTakes (ContactVoices::SCRAPE, a.get (), ga);
	v->SetBed (ContactVoices::SCRAPE, 0.3f, 1.0f, 20000.0f);
	auto y = Render (*v, 2.0);
	CHECK_THAT (Rms (y, 24000, y.size ()), WithinRel (0.3, 0.25));
	CHECK_THAT ((double)v->BedLevel (ContactVoices::SCRAPE), WithinRel (0.3, 0.05));
}

TEST_CASE ("ContactVoices: a tear, the clatter, the crash played as one-shots", "[audio][contacts]")
{
	Fixture f;
	auto v = ContactVoices::Make (f.e, ContactVoices::AIR, 15.0, 1);
	auto blow = MakeClip (Blow (7, 0.3, 0.1));
	REQUIRE (v->Fire (ContactVoices::Shot (ContactVoices::TEAR, blow.get (), 0.5f, 0.8f, 12000.0f, 0.0f), 0.0f));
	REQUIRE (v->Fire (ContactVoices::Shot (ContactVoices::CLATTER, blow.get (), 0.5f, 1.0f, 10000.0f, 0.0f), 0.2f));
	REQUIRE (v->Fire (ContactVoices::Shot (ContactVoices::CRASH, blow.get (), 0.5f, 1.0f, 16000.0f, 0.0f), 0.0f));
	auto y = Render (*v, 0.05);
	CHECK (v->Playing () == 3);
	CHECK (Rms (y, 0, y.size ()) > 0.0);
}

TEST_CASE ("A crash's one-shots on the planet: heard, the vessel's emitter gone meanwhile", "[audio][contacts]")
{
	// (VesselSounds::Detach: the crash on an emitter of its own on the planet -
	// the vessel's emitters removed as it is destroyed don't take it along)
	Fixture f;
	World w (f.e);
	TestScene sc;
	const double c = Air::SeaLevel ().c;
	int bodyP;
	const Body *P = reinterpret_cast<const Body*> (&bodyP);
	sc.at[A] = V3 (c, 0, 0), sc.at[P] = V3 (c, 0, 0);    // (1 s away, both)
	auto onPlanet = Recorder::Make (f.e), onVessel = Recorder::Make (f.e);
	Recorder *rp = onPlanet.get ();
	EmitterDesc d;
	d.body = A;
	EmitterId vid = w.Add (d, std::move (onVessel));
	d.body = P;
	EmitterId pid = w.Add (d, std::move (onPlanet));
	REQUIRE (w.Get (pid));
	w.Get (pid)->Schedule (0.5, Kind (ContactVoices::CRASH));
	w.Get (pid)->Schedule (0.8, Kind (ContactVoices::TEAR));
	const double dt = 0.02;
	for (int i = 0; i <= 150; i++) {
		const double t = i * dt;
		if (i == 30) w.Remove (vid);   // (the vessel destroyed at 0.6 s: its emitters go)
		rp->now = t;
		w.Update (sc.At (t, V3 (), dt), sc);
	}
	REQUIRE (rp->got.size () == 2);
	CHECK_THAT (rp->got[0].at, WithinAbs (1.5, dt * 1.01));
	CHECK_THAT (rp->got[1].at, WithinAbs (1.8, dt * 1.01));
}

TEST_CASE ("ContactVoices: the mechanisms in the cabin as they are, the blows dulled", "[audio][contacts]")
{
	Fixture f;
	auto blow = MakeClip (Blow (11, 0.2, 0.05));
	auto energy = [&](ContactVoices::Profile p, int kind) {
		auto v = ContactVoices::Make (f.e, p, 15.0, 2);
		REQUIRE (v->Fire (ContactVoices::Shot (kind, blow.get (), 1.0f, 1.0f, 20000.0f, 0.0f), 0.0f));
		return Energy (Render (*v, 0.5));
	};
	for (int k : {ContactVoices::LATCH, ContactVoices::HOOK, ContactVoices::RELEASE, ContactVoices::SEAL, ContactVoices::LOCK}) {
		INFO ("kind " << k);
		CHECK (ContactVoices::Clear (k));
		// (outside and in the cabin alike: not dulled)
		CHECK_THAT (energy (ContactVoices::STRUCTURE, k), WithinRel (energy (ContactVoices::AIR, k), 0.02));
	}
	CHECK_FALSE (ContactVoices::Clear (ContactVoices::IMPACT));
	CHECK_FALSE (ContactVoices::Clear (ContactVoices::THUD));
	// a blow in the cabin: dulled (the noise's high end gone)
	CHECK (energy (ContactVoices::STRUCTURE, ContactVoices::IMPACT) < 0.7 * energy (ContactVoices::AIR, ContactVoices::IMPACT));
}

TEST_CASE ("ContactVoices: two motors' beds at once, each its level; silent stopped", "[audio][contacts]")
{
	Fixture f;
	auto a = MakeClip (Steady (31, 2.5, 3000.0f));
	const float ga = (float)(1.0 / std::sqrt (Energy (a->pcm) / 2.5));
	auto v = ContactVoices::Make (f.e, ContactVoices::STRUCTURE, 15.0, 4);
	REQUIRE (v->SetBedTakes (ContactVoices::MOTOR_A, a.get (), ga));
	REQUIRE (v->SetBedTakes (ContactVoices::MOTOR_B, a.get (), ga));
	v->SetBed (ContactVoices::MOTOR_A, 0.05f, 0.95f);
	auto y1 = Render (*v, 1.5);
	CHECK_THAT (v->BedLevel (ContactVoices::MOTOR_A), WithinRel (0.05, 0.02));
	CHECK (v->BedLevel (ContactVoices::MOTOR_B) == 0.0f);
	v->SetBed (ContactVoices::MOTOR_B, 0.1f, 1.1f);
	auto y2 = Render (*v, 1.5);
	CHECK_THAT (v->BedLevel (ContactVoices::MOTOR_B), WithinRel (0.1, 0.02));
	CHECK (Rms (y2, 24000, y2.size ()) > Rms (y1, 24000, y1.size ()));
	// stopped: both out, silent
	v->SetBed (ContactVoices::MOTOR_A, 0.0f, 1.0f), v->SetBed (ContactVoices::MOTOR_B, 0.0f, 1.0f);
	auto y3 = Render (*v, 2.0);
	CHECK (v->BedLevel (ContactVoices::MOTOR_A) == 0.0f);
	CHECK (v->BedLevel (ContactVoices::MOTOR_B) == 0.0f);
	CHECK (Rms (y3, 48000 + 24000, y3.size ()) == 0.0);
}

TEST_CASE ("ContactVoices: sixteen at once, the quietest's voice taken", "[audio][contacts]")
{
	Fixture f;
	auto v = ContactVoices::Make (f.e, ContactVoices::AIR, 10.0, 1);
	auto longBlow = MakeClip (Blow (2, 1.0, 0.5));
	for (int i = 0; i < 20; i++)
		REQUIRE (v->Fire (ContactVoices::Shot (ContactVoices::IMPACT, longBlow.get (), 0.01f * (i + 1), 1.0f, 8000.0f, 0.5f), 0.0f));
	std::vector<float> y (480);
	v->RenderBlock (y.data (), 480);
	CHECK (v->Playing () == ContactVoices::VOICES);
}

TEST_CASE ("ContactVoices: its time per block (16 voices, the five beds)", "[audio][contacts][cpu]")
{
	Fixture f;
	auto v = ContactVoices::Make (f.e, ContactVoices::STRUCTURE, 20.0, 1);
	auto longBlow = MakeClip (Blow (4, 30.0, 10.0));
	auto bed = MakeClip (Steady (5));
	v->SetBedTakes (ContactVoices::ROLL, bed.get (), 1.0f, bed.get (), 1.0f);
	v->SetBedTakes (ContactVoices::BRAKE, bed.get (), 1.0f);
	v->SetBedTakes (ContactVoices::SCRAPE, bed.get (), 1.0f, bed.get (), 1.0f);
	v->SetBedTakes (ContactVoices::MOTOR_A, bed.get (), 1.0f), v->SetBedTakes (ContactVoices::MOTOR_B, bed.get (), 1.0f);
	v->SetBed (ContactVoices::ROLL, 0.1f, 1.1f), v->SetBed (ContactVoices::BRAKE, 0.1f, 0.9f), v->SetBed (ContactVoices::SCRAPE, 0.1f, 1.2f, 3000.0f);
	v->SetBed (ContactVoices::MOTOR_A, 0.05f, 0.95f), v->SetBed (ContactVoices::MOTOR_B, 0.05f, 1.05f);
	for (int i = 0; i < ContactVoices::VOICES; i++)
		v->Fire (ContactVoices::Shot (ContactVoices::IMPACT, longBlow.get (), 0.05f, 1.0f + 0.01f * i, 4000.0f, 0.5f), 0.0f);
	std::vector<float> y (480);
	v->RenderBlock (y.data (), 480);
	REQUIRE (v->Playing () == ContactVoices::VOICES);
	// nothing allocated rendering (one-shots arriving, voices taken)
	g_allocs = 0;
	for (int b = 0; b < 50; b++) {
		v->Fire (ContactVoices::Shot (ContactVoices::IMPACT, longBlow.get (), 0.05f, 1.0f, 4000.0f, 0.5f), 0.0f);
		g_watch = true;
		v->RenderBlock (y.data (), 480);
		g_watch = false;
	}
	CHECK (g_allocs.load () == 0);
	// the best of several runs of 200 blocks (others' load on the machine: not ours)
	double best = 1e9;
	for (int run = 0; run < 5; run++) {
		auto t0 = std::chrono::steady_clock::now ();
		for (int b = 0; b < 200; b++) v->RenderBlock (y.data (), 480);
		double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count () / 200.0;
		best = std::min (best, ms);
	}
	printf ("ContactVoices: %.4f ms per 480-frame block (16 voices, five beds)\n", best);
	CHECK (v->Playing () == ContactVoices::VOICES);
	CHECK (best < 0.15);
}

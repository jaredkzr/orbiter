// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The simulation's side of sound: the listener (the camera) and the
// sound sources (emitters) on bodies, and what of each is heard every
// time step:
//
//  - through the air: from where the source was when the sound left it
//    (Track: travel time, Doppler, silence ahead of a supersonic source's
//    shock cone, a boom where it sweeps over the listener), as loud as the
//    air at both ends lets it be (Medium), its high end absorbed on the
//    way. In the cockpit, muffled by the hull.
//  - through the hull: a listener in the cockpit of the vessel the source
//    is on (or a vessel docked to it) hears it through the structure, in
//    vacuum too.
//
// Positions are taken in the rotating frame of the planet whose air the
// listener is in (or near): the air moves with the planet.
// =======================================================================

#ifndef __AUDIOWORLD_H
#define __AUDIOWORLD_H

#include "AudioEngine.h"
#include "Medium.h"
#include "PathNode.h"
#include "Track.h"
#include "Source.h"
#include "Assets.h"
#include "Sim.h"
#include <array>
#include <cstring>
#include <type_traits>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Body;
class Planet;

namespace audio {

/// A source playing a miniaudio data source (generators: tests) or a file
SourcePtr<Source> MakeToneSource (Engine &e, double freq, double amp);
/// (the file: a resolved path, decoded through Assets - remembered)
SourcePtr<Source> MakeFileSource (Engine &e, const std::string &path, bool loop);
SourcePtr<Source> MakeClipSource (Engine &e, ClipRef clip, bool loop);

/// A body's identity for the sound: the same while the body lives; a body
/// forgotten (deleted: World::Forget) is never the same again, even if a new
/// one comes at its address. Compare these, not pointers
struct BodyRef {
	const Body *body = nullptr;
	uint64_t serial = 0;
	explicit operator bool () const { return serial != 0; }
	bool operator== (const BodyRef &o) const { return serial == o.serial; }
	bool operator!= (const BodyRef &o) const { return serial != o.serial; }
};

/// An emitter, as its owner holds it: checked by its generation - one
/// removed (or gone with its body) is null from then on, never another's
struct EmitterId {
	uint32_t slot = 0, gen = 0;
	explicit operator bool () const { return gen != 0; }
	bool operator== (const EmitterId &o) const { return slot == o.slot && gen == o.gen; }
	bool operator!= (const EmitterId &o) const { return !(*this == o); }
};

struct EmitterDesc {
	const Body *body = nullptr; ///< the body it's on
	V3 offset;                  ///< its position in the body's frame [m]
	float level = 1.0f;         ///< gain at refDist, in sea-level air
	float refDist = 10.0f;      ///< distance within which it doesn't get louder [m] (about its size)
	float hull = 0.0f;          ///< gain through the hull, for a listener in the vessel (0: none)
	V3 dir;                     ///< the way it sounds loudest, in the body's frame (none: all ways alike)
	float front = 1.0f;         ///< opposite that way: this much (a rocket's exhaust: about 0.1)
	bool boom = false;          ///< its shock cone booms (the body itself, once per vessel)
	float length = 10.0f;       ///< the body's length [m] (for its boom)
	bool delayed = false;       ///< a recording begun now: heard from when the sound of its start arrives
	                            ///< (its travel time; at once through the structure, or cinematic)
	std::string name;           ///< (for the readout)
};

/// An emitter heard only through its body's structure - in a cockpit of
/// it, or of a vessel docked to it, also in vacuum: outside, a tiny source
/// (refDist 0.1 m: nothing heard). Its source's full scale is `cabinP`
/// [Pa] in the cabin (an AIR and a STRUCTURE emitter per vessel: what is
/// heard outside, what its own structure carries in)
EmitterDesc StructureEmitter (const Body *body, double cabinP, const std::string &name);
/// (the gain through the hull it is given: its air level that much less)
constexpr float STRUCTURE_HULL = 50.0f;

/// A source's parameters as a few numbers, for its emitter's history (Push,
/// Recalled): its kind's own, as many as it has
template <int N> struct Floats {
	float v[N] = {};
	float &operator[] (int i) { return v[i]; }
	float operator[] (int i) const { return v[i]; }
};

class Emitter {
public:
	const EmitterDesc &Desc () const { return desc; }
	EmitterId Id () const { return id; }
	void SetLevel (float l) { desc.level = l; }
	void SetHull (float h) { desc.hull = h; }
	void SetOffset (const V3 &o) { desc.offset = o; }
	void SetDir (const V3 &d) { desc.dir = d; }
	/// Its source's own pitch (sound files)
	void SetPitch (float p) { if (source) source->SetPitch (p); }
	/// Its source has played to its end
	bool SourceDone () const { return source && source->Done (); }
	/// Held (paused) or going on; one not begun yet (delayed) begins only when let go
	void Hold (bool h) { held = h; if (begun && source) source->Pause (h); }
	/// Its source started (a delayed recording: once the sound of its start has arrived)
	bool Begun () const { return begun; }
	Source *GetSource () { return source.get (); }
	/// What is heard of it now (for the readout and tests)
	struct State {
		int images = 0;         // emission moments heard (0: not heard)
		double delay = 0.0;     // travel time [s]
		double dist = 0.0;      // from where it was heard [m]
		float doppler = 1.0f;
		float air = 0.0f;       // gains of the ways
		float hull = 0.0f;
		float cutoff = 20000.0f;
	};
	const State &Heard () const { return state; }

	/// What its source does, as it changes - its own parameters (a struct of
	/// its kind's: a throttle, a speed, blows counted...; plain data), stamped
	/// with the simulation time; Recalled: as heard now, when the sound now
	/// arriving left it (travel time ago; at once in the cockpit of its
	/// vessel, and while not heard). A minute of it kept (the farthest sound's
	/// travel), a value kept once while it holds (the newest at or before a
	/// time is the same: nothing lost)
	template <class T> void Push (double t, const T &p)
	{
		static_assert (std::is_trivially_copyable<T>::value, "an emitter's parameters: plain data");
		PushBytes (t, &p, sizeof (T));
	}
	template <class T> T Recalled () const
	{
		T p{};
		const unsigned char *b = RecalledBytes (sizeof (T));
		if (b) memcpy (&p, b, sizeof (T));
		return p;
	}
	/// A one-shot of its source's at simulation time tEmit [s] (a blow at
	/// that moment): released to the source (Source::Fire) when its sound
	/// arrives - a travel time later through the air, at once through the
	/// structure or cinematic - in order, each once, timed within the step
	/// it arrives in. False: too many waiting (MAX_PENDING)
	bool Schedule (double tEmit, const OneShot &s);
	/// (tests) one-shots waiting for their sound to arrive
	size_t Pending () const { return pending.size (); }
	static constexpr size_t MAX_PENDING = 256;

	/// The newest pushed (false: none)
	template <class T> bool Latest (T &p) const
	{
		if (histTimes.size () == histFirst || histSize != sizeof (T)) return false;
		memcpy (&p, histBytes.data () + (histTimes.size () - 1) * histSize, sizeof (T));
		return true;
	}
	/// (tests) the moments kept
	size_t HistoryLength () const { return histTimes.size () - histFirst; }

private:
	friend class World;
	EmitterId id;
	EmitterDesc desc;
	SourcePtr<Source> source;
	PathNode node;
	Track track;
	State state;
	bool culled = false;        // its source stopped: not heard (World::Cull)
	bool begun = true;          // its source started (delayed: once the sound of its start arrives)
	bool held = false;          // (paused: Hold)
	double created = -1.0;      // [s] (simulation time) when it was added
	double silentSince = -1.0;
	// its parameters' history: times, and as many records of histSize bytes (from histFirst on)
	std::vector<double> histTimes;
	std::vector<unsigned char> histBytes;
	size_t histFirst = 0, histSize = 0;
	double histNow = 0.0;    // (the last push's time: what is heard is recalled from it)
	void PushBytes (double t, const void *p, size_t n);
	const unsigned char *RecalledBytes (size_t n) const;
	void ClearHistory () { histTimes.clear (), histBytes.clear (), histFirst = 0; }
	double hearDelay = 0.0;     // the travel time the source is heard with
	struct Timed { double t; OneShot s; };
	std::deque<Timed> pending;  // (Schedule: by time)
	double heardAt = -1e300;    // the time last heard (the step's: one-shots before it are out)
	double releaseDelay = -1.0; // the travel time one-shots were last released with (< 0: none)
	void Release (double heard, double ramp);
	V3 lastP;                   // where it was last step (jumps)
	double lastT = 0.0;
	bool hasLast = false;
};

class World {
public:
	explicit World (Engine &engine);
	~World ();

	/// A new emitter with its source (playing); a null id if the engine is off
	EmitterId Add (const EmitterDesc &d, SourcePtr<Source> src);
	/// (a null id, or one gone: nothing)
	void Remove (EmitterId id);
	/// The emitter, if it is still there
	Emitter *Get (EmitterId id) const;
	/// Its source as what it is (a synthesizer's type), if still there
	template <class T> T *Synth (EmitterId id) const { Emitter *e = Get (id); return e ? static_cast<T*> (e->GetSource ()) : nullptr; }
	/// A body is deleted: its emitters go, its identity with them (the one
	/// place a body is forgotten: every handle and BodyRef to it dies here)
	void Forget (const Body *body);
	void Clear ();

	/// A body's identity (null: none); is it still there
	BodyRef Ref (const Body *b);
	/// (its identity if it has one yet - not given one: null)
	BodyRef Known (const Body *b) const;
	bool Alive (const BodyRef &r) const;
	/// The body, if still there
	const Body *Get (const BodyRef &r) const { return Alive (r) ? r.body : nullptr; }

	void SetRealism (Realism r) { realism = r; }
	Realism GetRealism () const { return realism; }

	/// Once per time step (simulation thread): the simulation as it is now
	/// (its time: the simulation's, or a replay's), the bodies asked of scene
	void Update (const Snapshot &snap, const Scene &scene);
	/// Time jumped (a replay began or ended): the sources' paths and
	/// histories start over
	void ResetPaths ();

	/// A sonic boom: a supersonic source's shock cone swept over the listener
	/// (dir: where it came from, camera frame; height: the source above the
	/// listener [m]; mach: its speed in the air there; sAir: its air)
	struct Boom { const Emitter *emitter; double t; V3 dir; double height, mach; Air sAir; };
	void OnBoom (std::function<void (const Boom&)> f) { onBoom = std::move (f); }

	/// Play a sound heard at once from a direction (camera frame): through
	/// the air (muffled by the hull in a cockpit), at a gain, for `length`
	/// seconds; then it goes
	void PlayShot (SourcePtr<Source> src, const V3 &dir, float gain, double length);

	const std::vector<std::unique_ptr<Emitter>> &Emitters () const { return emitters; }
	/// The voice cap: continuous sources (Source::Continuous) not heard
	/// for CULL_AFTER (their gain times their activity under CULL_LEVEL:
	/// about 10 dB SPL), or beyond the MAX_VOICES loudest, are stopped; they
	/// go on when they may be heard
	static constexpr int MAX_VOICES = 32;
	static constexpr float CULL_LEVEL = 1e-6f;
	static constexpr double CULL_AFTER = 0.5; // [s] (the clock of what is heard)
	/// Continuous sources playing now, stopped now
	int Voices () const { return voices; }
	int Culled () const { return culled; }
	/// The listener's air
	const Air &ListenerAir () const { return lAir; }
	bool ListenerInside () const { return inside; }

	/// The air at a global position near a planet (none beyond its
	/// atmosphere: OrbiterScene.cpp - Orbiter's, not the world's)
	static Air AirAt (const Planet *p, const double gpos[3]);

private:
	void UpdateEmitter (Emitter &e, double t);
	void Cull ();
	int voices = 0, culled = 0;

	struct Shot {
		SourcePtr<Source> src;
		std::unique_ptr<PathNode> node;
		double end;   // [s] (system time: shots play at warp too)
	};
	void UpdateShots ();

	Engine &engine;
	std::vector<std::unique_ptr<Emitter>> emitters;   // (in the order they came)
	std::vector<std::pair<uint32_t, Emitter*>> slots; // (by EmitterId::slot: its generation, the emitter)
	std::vector<uint32_t> freeSlots;
	uint32_t generation = 0;
	std::map<const Body*, uint64_t> serials;          // (the bodies' identities)
	uint64_t nextSerial = 0;
	void Erase (std::vector<std::unique_ptr<Emitter>>::iterator it);
	std::vector<Shot> shots;
	Realism realism = PHYSICAL;
	std::function<void (const Boom&)> onBoom;

	const Scene *scene = nullptr;    // (during Update)
	double now = 0.0;                // the clock of what is heard (Snapshot::playT)
	double ramp = 0.0;               // [s] the step: the paths' levels ramp over it
	// the listener
	const void *airBody = nullptr;   // the frame and air of this step
	V3 frameOrigin () const { return framePos; }
	V3 framePos;
	V3 L, Lprev, vL;
	double tPrev = -1.0;
	Air lAir;
	bool inside = false;             // in a cockpit
	bool listenerJump = false;       // the camera jumped this step (a cut)
	const void *insideOf = nullptr;  // the vessel (structure) the cockpit is in
	double camRot[9] = {1,0,0, 0,1,0, 0,0,1};
	double airRot[9] = {1,0,0, 0,1,0, 0,0,1};
};

} // namespace audio

#endif // !__AUDIOWORLD_H

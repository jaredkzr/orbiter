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
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Body;
class Planet;

namespace audio {

/// What makes the sound of an emitter: a sound file, a generator, a
/// synthesizer. It plays into the emitter's path node and follows the
/// Doppler factor (its playback rate).
class Source {
public:
	virtual ~Source () = default;
	/// Connect the output to a node's input 0 (mono)
	virtual bool Attach (void *node) = 0;
	virtual void Start () = 0;
	virtual void Stop () = 0;
	/// The Doppler factor (received / emitted frequency)
	virtual void SetDoppler (float f) = 0;
};

/// A source playing a miniaudio data source (generators: tests) or a file
std::unique_ptr<Source> MakeToneSource (Engine &e, double freq, double amp);
std::unique_ptr<Source> MakeFileSource (Engine &e, const std::string &path, bool loop);

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
	std::string name;           ///< (for the readout)
};

class Emitter {
public:
	const EmitterDesc &Desc () const { return desc; }
	void SetLevel (float l) { desc.level = l; }
	void SetHull (float h) { desc.hull = h; }
	void SetOffset (const V3 &o) { desc.offset = o; }
	void SetDir (const V3 &d) { desc.dir = d; }
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

	/// What its source does, as it changes (up to 16 numbers: a throttle, a
	/// speed...), stamped with the simulation time; Recalled: as heard now,
	/// when the sound now arriving left it (travel time ago; at once in the
	/// cockpit of its vessel, and while not heard)
	using Params = std::array<float, 16>;
	void Push (double t, const Params &p);
	Params Recalled () const;

private:
	friend class World;
	EmitterDesc desc;
	std::unique_ptr<Source> source;
	PathNode node;
	Track track;
	State state;
	bool gone = false;          // its body was deleted
	std::deque<std::pair<double, Params>> history;
	double hearDelay = 0.0;     // the travel time the source is heard with
	V3 lastP;                   // where it was last step (jumps)
	double lastT = 0.0;
	bool hasLast = false;
};

class World {
public:
	explicit World (Engine &engine);
	~World ();

	/// A new emitter with its source (playing), or null if the engine is off
	Emitter *Add (const EmitterDesc &d, std::unique_ptr<Source> src);
	void Remove (Emitter *e);
	/// A body is deleted: its emitters go
	void Forget (const Body *body);
	void Clear ();

	void SetRealism (Realism r) { realism = r; }
	Realism GetRealism () const { return realism; }

	/// Once per time step (simulation thread)
	void Update ();

	/// A sonic boom: a supersonic source's shock cone swept over the listener
	/// (dir: where it came from, camera frame; height: the source above the
	/// listener [m]; mach: its speed in the air there; sAir: its air)
	struct Boom { const Emitter *emitter; double t; V3 dir; double height, mach; Air sAir; };
	void OnBoom (std::function<void (const Boom&)> f) { onBoom = std::move (f); }

	/// Play a sound heard at once from a direction (camera frame): through
	/// the air (muffled by the hull in a cockpit), at a gain, for `length`
	/// seconds; then it goes
	void PlayShot (std::unique_ptr<Source> src, const V3 &dir, float gain, double length);

	const std::vector<std::unique_ptr<Emitter>> &Emitters () const { return emitters; }
	/// The listener's air
	const Air &ListenerAir () const { return lAir; }
	bool ListenerInside () const { return inside; }

	/// The air at a global position near a planet (none beyond its atmosphere)
	static Air AirAt (const Planet *p, const double gpos[3]);

private:
	void UpdateEmitter (Emitter &e, double t);

	struct Shot {
		std::unique_ptr<Source> src;
		std::unique_ptr<PathNode> node;
		double end;   // [s] (system time: shots play at warp too)
	};
	void UpdateShots ();

	Engine &engine;
	std::vector<std::unique_ptr<Emitter>> emitters;
	std::vector<Shot> shots;
	Realism realism = PHYSICAL;
	std::function<void (const Boom&)> onBoom;

	// the listener
	const Planet *airBody = nullptr; // the frame and air of this step
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

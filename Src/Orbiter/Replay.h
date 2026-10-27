// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Instant replay: the states of all vessels (position, velocity,
// attitude, thrust and animation levels) are recorded in memory for the
// last minutes of simulation. In replay mode the simulation is held and
// the scene shows any recorded moment: played at a chosen speed, paused
// or scrubbed on a timeline, with the camera free or following keyframes
// (cinematic camera), and can be exported as a frame sequence (and a
// video, if ffmpeg is available). Leaving the replay restores the live
// state exactly.
// =======================================================================

#ifndef __REPLAY_H
#define __REPLAY_H

#include "OrbiterAPI.h"
#include "Vecmat.h"
#include "Camera.h"
#include <deque>
#include <string>
#include <vector>

class Body;
class Vessel;

class Replay: public ImGuiDialog {
public:
	explicit Replay (size_t maxFrames);
	~Replay ();

	/// Record the vessels' states (after each simulation step)
	void Record ();
	/// A vessel is about to be deleted: forget it
	void Forget (const Vessel *v);
	/// A piece of debris is about to be deleted: forget it
	void Forget (const class Debris *d);
	/// The replay time shown [s simulation time]
	double Now () const { return t; }

	bool Active () const { return active_; }
	bool Exporting () const { return exporting; }
	void Toggle () { if (active_) Exit (); else Enter (); }
	bool Enter ();
	void Exit ();

	/// Each frame while active (before rendering): advance and show the replay time
	void Update ();

	// ImGuiDialog: the timeline and controls
	void Display () override;
	void OnDraw () override {}

	double Start () const { return frames.empty() ? 0.0 : frames.front().simt; }
	double End () const { return frames.empty() ? 0.0 : frames.back().simt; }
	double Time () const { return t; }
	void Seek (double simt);
	void SetPlaying (bool play) { playing = play; }
	void ShowNow () { if (active_) Show (t); } // (immediately, e.g. after Seek)
	void Export (double simt0, double simt1, int fps); // frames (and a video) of a time range
	const std::string &Status () const { return status; }
	void AddCameraKey () { if (active_) AddKey (); }
	void SetCinematic (bool on) { cinematic = on; if (on && active_) CameraKeys (); }

	struct CamKey {
		double t;
		Body *target;
		bool external;
		ExtCamMode mode;
		double dist, phi, theta, ap;
	};

private:
	struct VesselRec {
		Vessel *v;
		Vector pos, vel, omega; // global
		Quaternion q;
		std::vector<float> th;   // thruster levels
		std::vector<float> anim; // animation states
	};
	struct DebrisRec {           // (pieces in motion; those at rest stay where they are)
		class Debris *d;
		Vector pos, vel;
		Quaternion q;
	};
	struct Frame {
		double simt;
		std::vector<VesselRec> v;
		std::vector<DebrisRec> d;
	};
	struct LiveDebris {
		class Debris *d;
		StateVectors s;
		Vector cpos, cvel;
	};
	std::vector<LiveDebris> liveDebris;
	struct Live {                // the live state at the start of the replay
		Vessel *v;
		StateVectors s;
		Vector cpos, cvel;
		std::vector<double> th, thperm;
		std::vector<double> anim;
	};

	void Show (double simt);     // set the scene to a recorded time
	void SetCelestials ();       // celestial bodies at the current td
	void CameraKeys ();          // cinematic camera
	void AddKey ();
	void StartExport ();
	void FinishExport ();

	std::deque<Frame> frames;
	size_t maxFrames;
	bool active_ = false;
	double t = 0.0;              // replay time (simulation time)
	double speed = 1.0;
	bool playing = true;
	bool loop = false;
	double liveMJD = 0.0;
	std::vector<Live> live;
	bool liveRunning = false;

	std::vector<CamKey> keys;
	bool cinematic = false;
	double tIn = -1e100, tOut = 1e100; // export range

	bool exporting = false;
	int exportFps = 60;
	int exportFrame = 0;
	std::string exportDir;
	std::string status;
};

#endif // !__REPLAY_H

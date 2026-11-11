// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Crash-site fires: the propellant a crash spilled burning where it fell,
// after its fireball (VDebris::Fireball) - a fuel slick burning on the sea,
// a pool of it on the ground. Kept by the scene, not by a piece of debris:
// the pieces sink, are hidden by the replay or go out of range, the fire
// stays where the fuel is.
// - its flames: low, wide tongues over the spill (about half the fireball's
//   radius across), strongest for its time t_w, then dying to embers over
//   half of it
// - its smoke: a black stem rising from it (fuel burning on water smokes
//   black too), for its time and a smoulder of a minute after
// - its light: flickering, at the flames
// (ORBITER_VK_FIREBALL_LOG: logged as they start and go out)
// =======================================================================

#ifndef __CRASHFIRE_H
#define __CRASHFIRE_H

#include "Particles.h"
#include <vector>

class Scene;
class VkTexture;
struct LightSource;

class CrashFires {
public:
	/// a fire where a fireball of M [kg] of propellant formed (Mp [kg] of it
	/// burned in the ball, of radius R [m]) on body hp at global position
	/// gpos (its foot on the surface below), the fuel spread along the path
	/// (global direction dir, ground-relative speed speed [m/s]); burning
	/// for tw [s] from time t0 [s] (simulation time) on, starting as the
	/// ball lifts off it (start [s] after t0). onWater: a slick on the sea
	void Add (OBJHANDLE hp, const VECTOR3 &gpos, const VECTOR3 &dir, double speed, double M, double Mp, double R,
		double t0, double start, double tw, bool onWater);
	/// a frame: their flames and smoke (frozen while paused)
	void Update (Scene *scene);
	void CollectLights (Scene *scene, std::vector<LightSource> &lights);
	/// the hot air over them (the heat shimmer: PlumeRenderer::AddHeat)
	void CollectHeat (Scene *scene, class PlumeRenderer &pr);
	void DrawParticles (Scene *scene);
	/// the rings retired (their particles live on)
	void Release (ParticleSystem *sys);
	size_t Count () const { return fires.size(); }

private:
	struct Ring : ParticleRing { double accum = 0.0; VkTexture *tex = nullptr; bool emissive = false; };
	struct Fire {
		OBJHANDLE hp = nullptr;
		double lng = 0.0, lat = 0.0, rad = 0.0; // its centre (planet frame) [rad, rad, m]
		double dirE = 0.0, dirN = 0.0;          // the spill's long axis (east, north components)
		double half = 0.0, width = 0.0;         // [m] its half length along it, its half width
		double M = 0.0, Mp = 0.0, R = 0.0;
		double t0 = 0.0, start = 0.0, tw = 0.0;
		bool onWater = false;
		bool logged = false, loggedOut = false;
		double strength = 0.0;                  // its flames now (0..1)
		double smoke = 0.0;                     // its smoke now (0..1)
		Ring flames, stem, embers;
		uint32_t seed = 0;
	};
	std::vector<Fire> fires;
	/// its strength of flames and of smoke at age t [s] since t0
	static void Strength (const Fire &f, double t, double &flames, double &smoke);
	/// its centre, up direction, east and north (global frame) now
	static void Frame (const Fire &f, VECTOR3 &c, VECTOR3 &up, VECTOR3 &east, VECTOR3 &north);
};

#endif // !__CRASHFIRE_H

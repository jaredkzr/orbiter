// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Visual objects: render-side representations of Orbiter objects
// =======================================================================

#ifndef __VOBJECTS_H
#define __VOBJECTS_H

#include "VkMath.h"
#include "Scene.h"
#include "Clouds.h"
#include "Particles.h"
#include "WaterSpray.h"
#include <memory>
#include <vector>

class VkMesh;
#include "Contrail.h"
namespace vkscatter { class Gallery; class Field; }
namespace vkcity { class Field; }
class VESSEL;
class ShadowMap;
class Atmosphere;

class VObject {
public:
	VObject (OBJHANDLE hObj, Scene *scene);
	virtual ~VObject () {}

	OBJHANDLE Handle () const { return hObj; }
	virtual void Update ();
	virtual void Render (RenderPass pass) = 0;
	virtual void RenderSky () {}
	/// work before the frame's rendering scopes (data uploads)
	virtual void PrepareFrame () {}
	virtual void RenderShadow (ShadowMap &sm) {}
	virtual void CollectLights (std::vector<LightSource> &lights) {}
	/// engine plumes of this frame (before the prepass)
	virtual void CollectPlumes (class PlumeRenderer &pr) {}
	/// crash fireballs of this frame, marched as volumes (before the prepass)
	virtual void CollectFireballs (class FireballRenderer &fr) {}
	/// particles of the object's own (engine smoke), in the scene with the reactive mask
	virtual void DrawParticles () {}
	/// reentry plasma sheath (heated vessels), in the scene with the reactive mask
	virtual void DrawPlasma (class PlasmaRenderer &pr) {}
	/// nearest intersection of a ray (global frame, |d| = 1) with the
	/// object's meshes (and those of its surface bases) within maxDist
	virtual bool RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n) { return false; }
	/// what the last successful RayHit met: 0 terrain, 1 a surface base, 2 a vessel
	int LastHitKind () const { return lastHitKind; }
	virtual Atmosphere *GetAtmosphere () { return nullptr; }
	virtual int VisEvent (DWORD msg, DWORD_PTR context) { return 0; }
	virtual MESHHANDLE GetMesh (UINT idx) { return NULL; }

	double CamDist () const { return camDist; }
	/// remember the pose of this frame for the motion vectors of the next
	void StorePrev () { prevGpos = gpos, prevGrot = grot, havePrev = true; }
	double Size () const { return size; }
	/// small number identifying the object (ray traced light shadows)
	uint32_t Id () const { return id; }

protected:
	OBJHANDLE hObj;
	Scene *scene;
	uint32_t id;
	VECTOR3 gpos{};
	MATRIX3 grot{};
	double size = 1.0;
	double camDist = 0.0;
	VECTOR3 prevGpos{};
	MATRIX3 prevGrot{};
	bool havePrev = false;
	int lastHitKind = 2;
	/// set the prepass motion of this object (pose now and in the last frame)
	void SetMotion () const;
};

// -----------------------------------------------------------------------

class VVessel : public VObject {
public:
	VVessel (OBJHANDLE hObj, Scene *scene);
	~VVessel ();

	void Update () override;
	void Render (RenderPass pass) override;
	void RenderShadow (ShadowMap &sm) override;
	void CollectLights (std::vector<LightSource> &lights) override;
	void CollectPlumes (PlumeRenderer &pr) override;
	void DrawParticles () override;
	void DrawPlasma (PlasmaRenderer &pr) override;
	bool RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n) override;
	int VisEvent (DWORD msg, DWORD_PTR context) override;
	MESHHANDLE GetMesh (UINT idx) override;

private:
	struct MeshRec {
		std::unique_ptr<VkMesh> mesh;
		VECTOR3 ofs{};
		WORD visMode = MESHVIS_EXTERNAL;
		std::vector<Mat4> grpTransform; // animation transforms per group
		std::vector<Mat4> prevGrpTransform; // ... in the last frame (motion vectors)
		std::vector<uint8_t> grpVisible;
		std::unique_ptr<VkMesh> walls;  // along the cuts where parts broke off (Fracture.h)
		std::vector<uint32_t> wallGroup; // the group each of its parts is cut in
		std::vector<Mat4> wallPose;      // (the group transforms they were made for)
		std::vector<uint8_t> wallWatch;  // (groups at the gaps: made again when they move)
		std::vector<std::vector<BYTE>> hidden; // per group: oapiGetVesselHiddenTriangles (empty: none)
		// triangles cut along the torn surface between the parts (Fracture.h),
		// per group: those drawn by their pieces instead (1), and the pieces
		// on the vessel and broken off (the group's frame)
		std::vector<std::vector<uint8_t>> cutState;
		std::vector<std::vector<NTVERTEX>> cutKeep, cutLose;
	};
	void CutGroups ();
	void BuildWalls ();
	bool wallsDirty = false;
	double wallsBuilt = -1e30;
	std::vector<Mat4> wallTf, wallPrevTf; // (the walls' group transforms, while drawing)
	std::vector<uint8_t> wallVis;
	void WallTransforms (const MeshRec &rec);
	void LoadMeshes ();
	void InsertMesh (UINT idx);
	void UpdateAnimations ();
	void UpdateDamage ();
	DWORD damageVersion = 0; // (the parts broken off shown so far: oapiGetVesselDamageVersion)
	void UpdateTracks ();
	void RenderExhaust ();
	void SetHeating ();
	/// aerodynamic heating now (Sutton-Graves): false while too weak to glow
	bool Sheath (struct SheathState &s) const;
	/// the class keeps its add-on reentry streams (no sheath): -1 not known yet
	int reentryLegacy = -1;

	VESSEL *vessel;
	std::vector<MeshRec> meshes;
	/// distance along a spot light's direction to where it leaves the hull
	/// (emitters often sit inside it; shadow rays start there)
	std::map<const LightEmitter*, double> lightExit;
	double LightExit (const LightEmitter *le);
	// default (unanimated) positions of animated vertex lists, keyed by the list pointer
	std::map<VECTOR3*, std::vector<VECTOR3>> vtxListDefaults;
	float glassGlow[3] = { 0, 0, 0 }; // emission of transparent groups from interior lights
	std::vector<uint8_t> plumeExhaust; // exhausts drawn as plume volumes this frame
	/// engine smoke: a ring of particles per exhaust
	struct SmokeRing : ParticleRing { // (sys, first, capacity, head, lastEmit, life: ParticleRing)
		double accum = 0.0;
		double onTime = 0.0; // since the engine lit [s]
		bool havePrev = false;
		VECTOR3 prev{};
	};
	std::vector<SmokeRing> smoke;
	std::vector<SmokeRing> dust;   // dust (or spray) thrown up where a jet meets the ground
	uint8_t waterContact[3] = {}; // gear leg i stood in water last frame (UpdateTracks: a splash on touching down)
	/// the water (Water W9): splash, spray and wake from the core's water state
	WaterSpray water;
	void UpdateWaterSpray ();
	std::vector<SmokeRing> mist;   // fine spray hanging over water where a jet meets it (with dust[]: the spray sheet)
	std::vector<SmokeRing> trail;  // the long trail in the air (PlumeClass::trail): large puffs left along the path, carried by the wind
	std::vector<contrail::Emitter> contrails; // where the exhaust's water condenses (PlumeClass::contrail)
	int contrailBand = -1;                    // (the exhaust laying the persistent band this frame)
	/// the reentry plasma's wake: glowing gas streaming off the sheath's
	/// edge behind the vessel (near), and its afterglow along the path (far)
	struct WakeRing : ParticleRing { double accum = 0.0; bool havePrev = false; VECTOR3 prev{}; };
	WakeRing wake, afterglow;
	void EmitWake (double dt);
	/// the strongest of the vessel's legacy reentry streams (their amin/amax
	/// over the friction power; scales the wake), -1: none
	double LegacyReentry () const;
	void EmitTrail (DWORD idx, const struct PlumeClass &c, double fade, const VECTOR3 &src, const VECTOR3 &srcPrev, double startSize, double dt);
	void EmitDust (DWORD idx, int type, const struct PlumeDraw &pd, double dt);
	/// plumes as light sources this frame (global position, emission, side area)
	struct PlumeLight { VECTOR3 pos[3]; double radius[3]; float col[3]; double area; }; // (PlumeInfo: three points along the flame)
	std::vector<PlumeLight> plumeLights;
	/// distant flames drawn as points of light this frame (CollectPlumes, RenderExhaust)
	std::vector<PVtx> plumePoints;
	void EmitSmoke (DWORD idx, int type, const struct PlumeDraw &pd, const struct PlumeInfo &info, double dt);
	void AddPlumePoint (const struct PlumeInfo &info, int type);
};

// -----------------------------------------------------------------------

/// A piece broken off a vessel (oapiGetDebrisGroup): its triangles, with
/// the textures and materials of the vessel's mesh; those at the fracture
/// charred
class VDebris : public VObject {
public:
	VDebris (OBJHANDLE hObj, Scene *scene, DWORD serial);
	~VDebris ();
	DWORD Serial () const { return serial; }
	void Update () override;
	void Render (RenderPass pass) override;
	void RenderShadow (ShadowMap &sm) override;
	void CollectLights (std::vector<LightSource> &lights) override;
	void DrawParticles () override;
	void CollectFireballs (class FireballRenderer &fr) override;

private:
	DWORD serial;
	bool hidden = false;
	std::unique_ptr<VkMesh> mesh;
	DEBRISINFO info{};
	/// effects: the fireball when it broke off (propellant burning), the
	/// fire and smoke while it burns, sparks where it slides on the ground,
	/// dust where it hits it
	struct Ring : ParticleRing { // (sys, first, capacity, head, lastEmit, life: ParticleRing)
		double accum = 0.0;
		bool havePrev = false;
		VECTOR3 prev{};
		VkTexture *tex = nullptr;
		bool emissive = false;
	};
	Ring fire, smoke, sparks, dust;  // (sparks: also those of a blow)
	Ring frags;              // struck by another body: small bits of it
	WaterSpray water;        // in the water: splash, spray, wake (from its water state)
	// the fireball (Fireball): its flames, each cooling over its life (the
	// particles' temperature ramp), its soot cap rising from it; in vacuum
	// an expanding shell, its vapour (fbSmoke); its hot core (fbCore); the
	// flash as it ignites (fbFlashR)
	Ring fbFire, fbSmoke, fbCore, fbFlashR;
	/// the propellant of a break-up: its pieces' shares (one fireball of all
	/// of it; the visible time of their flames, t_w, by all of it)
	struct BreakUp { double fuel = 0.0; };
	std::shared_ptr<BreakUp> fbBreak;
	double fbT0 = -1e30, fbTf = 0.0;     // [s] when it formed; its flames' time t_f (0: none, vacuum)
	double fbR = 0.0, fbRNow = 0.0;      // [m] its radius grown, now
	double fbFuel = 0.0;       // [kg] its propellant (the pieces' of one break-up joined)
	bool fbWater = false;      // formed on the water (less of the fuel burns in it)
	bool fbFlash = false;      // its flash still to come (the step after it formed)
	int fbStage = 0;           // (ORBITER_VK_FIREBALL_LOG: its stages logged)
	VECTOR3 fbPos{}, fbVel{};  // its centre (global), velocity
	VECTOR3 fbDir{};           // the direction it was thrown along (relative to the air)
	double fbV0 = 0.0;         // [m/s] how fast (relative to the air), as it ignited
	VECTOR3 fbFlashPos{};      // where it ignited (the flash's light; moving with the air)
	double fbFuelW = 0.0;      // (the pieces' velocities weighted by their propellant: fbVel)
	// its shape now (the volume: FireballRenderer): drawn out along its path
	// (>= 1), lifting off (0..1), the vortex ring's turn [rad], the ground
	// below its centre [m] (along up: < 0 below), the local vertical
	double fbEl = 1.0, fbLift = 0.0, fbRoll = 0.0, fbGround = -1e9;
	double fbRaise = -1.0;     // its centre raised above where it formed (/ r: on the ground, a ball resting on it; -1: not known yet)
	VECTOR3 fbUp{ 0, 1, 0 };
	bool fbVolume = false;     // marched as a volume this step (its flame sprites left out)
	void Fireball (double dt, double rho, const VECTOR3 &up, const VECTOR3 &airVel, bool onWater);
	void VacuumFlash (double dt, double R, const VECTOR3 &vel);
	// a burning wreck at rest on the ground: a rising column of smoke, and
	// its long-lived drift with the wind (WreckSmoke)
	Ring column, drift;
	double burnEndT = -1e30;   // when its flames went out (it smoulders on)
	void WreckSmoke (double dt, double strength, const VECTOR3 &up, const VECTOR3 &groundVel, bool onWater);
	double hitSeen = -1e30;  // [s] the latest blow sprayed
	double sprayT = -1e30, sprayV = 0.0; // the latest spray: when, how hard [m/s]
	double hitFlashT = -1e30, hitFlashI = 0.0; // the flash of a fast blow
	bool burst = false;      // the fireball (once)
	double flashT = -1e30;   // [s] its flash
	double flashR = 0.0;     // [m] fireball radius
	double flashK = 3.0;     // [1/s] its light fading (quicker in vacuum)
	bool scorched = false;   // (a scorch mark made)
	bool burning = false;    // flames (burning, in air): visible for the break-up's t_w, not the core's whole burn
	bool sunk = false;       // under the water (no flames or smoke from it)
	bool quenched = false;   // its quench burst (a burning or hot piece going under)
	Ring quench;
	double flameF = 0.0;     // its flames now (0..1)
	double dustT = -1e30, dustImpact = 0.0; // the last puff of dust: when, its impact [m/s]
	void Effects ();
	void Spray (const VECTOR3 &pos, const VECTOR3 &vel, double v, double rho);
	/// (burst: the ring holds a couple of such bursts, not a stream over ringLife)
	bool Emit (Ring &r, ParticleSystem::Emission e, uint32_t n, double ringLife, bool burst = false);
	/// its groups cut along the torn surface between the vessel's parts
	/// (Fracture.h), where they are only partly in it: its pieces of their
	/// triangles, and the others' (its frame)
	struct PieceCut { bool on = false; std::vector<NTVERTEX> keep, lose; };
	void CutPiece (const VESSEL *src, const DEBRISINFO &info, std::vector<PieceCut> &cuts);
	double FireballRadius () const;
};

// -----------------------------------------------------------------------

class PlanetSurface;
class Atmosphere;

/// Surface base: generic structures supplied by the core as merged meshes
class VBase {
public:
	VBase (OBJHANDLE hBase, Scene *scene);
	~VBase ();
	OBJHANDLE Handle () const { return hBase; }
	void Render (RenderPass pass);
	void RenderShadow (ShadowMap &sm);
	bool RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n);
	/// floodlights of the base, at night
	void CollectLights (std::vector<LightSource> &lights);

private:
	/// lights of the base (base frame): runway edge lights and beacon arrays
	/// from the base's configuration, floodlights from Config/VK/Bases
	struct Light { VECTOR3 pos; float col[3]; float size; };
	struct Flood { VECTOR3 pos, dir; float col[3]; double range, umbra, penumbra; };
	std::vector<Light> lights;
	std::vector<Flood> floods;
	void ReadLights ();
	bool Night () const;
	OBJHANDLE hBase;
	Scene *scene;
	std::vector<std::unique_ptr<VkMesh>> ground;     // rendered before shadows (runways, pads)
	std::vector<std::unique_ptr<VkMesh>> structures; // rendered after shadows (buildings)
};

namespace vksurf { class TileProbe; }

class VPlanet : public VObject {
public:
	VPlanet (OBJHANDLE hObj, Scene *scene, bool isStar);
	~VPlanet ();
	void Update () override;
	void Render (RenderPass pass) override;
	void RenderSky () override;
	void RenderShadow (ShadowMap &sm) override;
	void PrepareFrame () override;
	bool RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n) override;
	void CollectLights (std::vector<LightSource> &lights) override;
	Atmosphere *GetAtmosphere () override { return atmosphere.get(); }
	/// tiled cloud layer (or nullptr) and its altitude [m]
	PlanetSurface *CloudLayer () const { return clouds.get(); }
	/// cloud decks of a gas giant (Config/VK/GasGiant), or null
	const VolumetricClouds::Decks *Decks () const { return hasDecks ? &decks : nullptr; }
	/// its rings, when the camera is within its decks (drawn before their
	/// composite: beyond them)
	void RenderRingBehindDecks ();
	double CloudAlt () const;
	/// reader of the ground's water mask (made on first use), or null: the body has none
	vksurf::TileProbe *MaskProbe ();
	/// reader of the ground's surface colour tiles (made on first use), or null: the body has none
	vksurf::TileProbe *SurfProbe ();

private:
	std::unique_ptr<vksurf::TileProbe> maskProbe;
	bool maskProbeInit = false;
	std::unique_ptr<vksurf::TileProbe> surfProbe;
	bool surfProbeInit = false;
	float colour[4];
	bool isStar;
	std::unique_ptr<Atmosphere> atmosphere;
	std::unique_ptr<PlanetSurface> surface; // tiled surface, if available
	std::unique_ptr<PlanetSurface> clouds;  // tiled cloud layer, if available
	std::vector<std::unique_ptr<VBase>> bases; // bases near the camera
	std::unique_ptr<vkscatter::Gallery> gallery; // preview of the ground scatter assets (ORBITER_VK_SCATTER_GALLERY)
	std::unique_ptr<vkscatter::Field> field;     // the ground scatter around the camera
	bool fieldInit = false;
	std::unique_ptr<vkcity::Field> cities;       // the procedural cities around the camera
	bool citiesInit = false;
	void UpdateGallery ();
	class VkTexture *globalTex = nullptr;   // whole-sphere map (bodies without tiles)
	class VkTexture *groundTex[2] = {};     // ... its western and eastern hemispheres (atmosphere, cloud decks)
	VolumetricClouds::Decks decks;
	bool hasDecks = false;
	bool DecksHide () const;                // the decks are drawn and the camera is below their clouds' top
	bool RingsBehindDecks () const;         // the camera in this planet's decks: its rings beyond them
	class VkTexture *ringTex = nullptr;
	class VkTexture *cloudTex = nullptr;    // legacy cloud layer
	double cloudAlt = 0.0;
	vkc::Buffer ringVtx, ringIdx;
	uint32_t ringNIdx = 0;
	float ringPrm[4] = {}, ringLegacy[4] = {}; // (Scene::DrawRing)
	void LoadLegacyTextures ();
	void BuildRing (double irad, double orad);
	void RenderRing ();
	void UpdateBases ();
	void RenderGlow ();
	double terrainTop = -1.0;               // [m] above size no terrain reaches (RayHit's quick reject; -1: not yet known)
};

#endif // !__VOBJECTS_H

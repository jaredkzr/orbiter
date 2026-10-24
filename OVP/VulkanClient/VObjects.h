// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Visual objects: render-side representations of Orbiter objects
// =======================================================================

#ifndef __VOBJECTS_H
#define __VOBJECTS_H

#include "VkMath.h"
#include "Scene.h"
#include <memory>
#include <vector>

class VkMesh;
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
	/// particles of the object's own (engine smoke), in the scene with the reactive mask
	virtual void DrawParticles () {}
	/// nearest intersection of a ray (global frame, |d| = 1) with the
	/// object's meshes (and those of its surface bases) within maxDist
	virtual bool RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n) { return false; }
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
	bool RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n) override;
	int VisEvent (DWORD msg, DWORD_PTR context) override;
	MESHHANDLE GetMesh (UINT idx) override;

private:
	struct MeshRec {
		std::unique_ptr<VkMesh> mesh;
		VECTOR3 ofs{};
		WORD visMode = MESHVIS_EXTERNAL;
		std::vector<Mat4> grpTransform; // animation transforms per group
		std::vector<uint8_t> grpVisible;
	};
	void LoadMeshes ();
	void InsertMesh (UINT idx);
	void UpdateAnimations ();
	void RenderExhaust ();
	void SetHeating ();

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
	struct SmokeRing {
		class ParticleSystem *sys = nullptr;
		uint32_t first = 0, capacity = 0, head = 0;
		double accum = 0.0, lastEmit = -1e30, life = 0.0;
		bool havePrev = false;
		VECTOR3 prev{};
	};
	std::vector<SmokeRing> smoke;
	void EmitSmoke (DWORD idx, int type, const struct PlumeDraw &pd, const struct PlumeInfo &info, double dt);
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

private:
	OBJHANDLE hBase;
	Scene *scene;
	std::vector<std::unique_ptr<VkMesh>> ground;     // rendered before shadows (runways, pads)
	std::vector<std::unique_ptr<VkMesh>> structures; // rendered after shadows (buildings)
};

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
	Atmosphere *GetAtmosphere () override { return atmosphere.get(); }
	/// tiled cloud layer (or nullptr) and its altitude [m]
	PlanetSurface *CloudLayer () const { return clouds.get(); }
	double CloudAlt () const;

private:
	float colour[4];
	bool isStar;
	std::unique_ptr<Atmosphere> atmosphere;
	std::unique_ptr<PlanetSurface> surface; // tiled surface, if available
	std::unique_ptr<PlanetSurface> clouds;  // tiled cloud layer, if available
	std::vector<std::unique_ptr<VBase>> bases; // bases near the camera
	class VkTexture *globalTex = nullptr;   // whole-sphere map (bodies without tiles)
	class VkTexture *ringTex = nullptr;
	class VkTexture *cloudTex = nullptr;    // legacy cloud layer
	double cloudAlt = 0.0;
	vkc::Buffer ringVtx, ringIdx;
	uint32_t ringNIdx = 0;
	void LoadLegacyTextures ();
	void BuildRing (double irad, double orad);
	void RenderRing ();
	void UpdateBases ();
	void RenderGlow ();
};

#endif // !__VOBJECTS_H

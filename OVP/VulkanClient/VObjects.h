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

class VObject {
public:
	VObject (OBJHANDLE hObj, Scene *scene);
	virtual ~VObject () {}

	OBJHANDLE Handle () const { return hObj; }
	virtual void Update ();
	virtual void Render (RenderPass pass) = 0;
	virtual int VisEvent (DWORD msg, DWORD_PTR context) { return 0; }
	virtual MESHHANDLE GetMesh (UINT idx) { return NULL; }

	double CamDist () const { return camDist; }
	double Size () const { return size; }

protected:
	OBJHANDLE hObj;
	Scene *scene;
	VECTOR3 gpos{};
	MATRIX3 grot{};
	double size = 1.0;
	double camDist = 0.0;
};

// -----------------------------------------------------------------------

class VVessel : public VObject {
public:
	VVessel (OBJHANDLE hObj, Scene *scene);
	~VVessel ();

	void Update () override;
	void Render (RenderPass pass) override;
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

	VESSEL *vessel;
	std::vector<MeshRec> meshes;
	// default (unanimated) positions of animated vertex lists, keyed by the list pointer
	std::map<VECTOR3*, std::vector<VECTOR3>> vtxListDefaults;
};

// -----------------------------------------------------------------------

class PlanetSurface;

class VPlanet : public VObject {
public:
	VPlanet (OBJHANDLE hObj, Scene *scene, bool isStar);
	~VPlanet ();
	void Update () override;
	void Render (RenderPass pass) override;

private:
	float colour[4];
	bool isStar;
	std::unique_ptr<PlanetSurface> surface; // tiled surface, if available
};

#endif // !__VOBJECTS_H

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "VObjects.h"
#include "WaterFx.h"
#include "CrashFire.h"
#include "Fireball.h"
#include "GfxSettings.h"
#include "ProcSurface.h"
#include "SurfaceQuery.h"
#include "GroundTracks.h"
#include "ScatterAssets.h"
#include "ScatterField.h"
#include "CityField.h"
#include "VkMesh.h"
#include "VulkanClient.h"
#include "Particles.h"
#include "PlanetSurface.h"
#include "Atmosphere.h"
#include "Shadows.h"
#include "LegacyTex.h"
#include "TileArchive.h"
#include "Fracture.h"
#include "Contrail.h"
#include "ParticleStream.h"
#include "Plasma.h"
#include <map>
#include <random>
#include "VesselAPI.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <set>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <sstream>

// ======================================================================
// VObject

VObject::VObject (OBJHANDLE h, Scene *s)
	: hObj(h), scene(s)
{
	static uint32_t nextId = 1;
	id = nextId++ & 0xffffff; // (exact as a float)
	size = oapiGetSize (hObj);
}

void VObject::Update ()
{
	scene->ObservedPos (hObj, gpos); // (light-time: where its light left it)
	oapiGetRotationMatrix (hObj, &grot);
	camDist = length (gpos - scene->CameraPos());
}

void VObject::SetMotion () const
{
	scene->SetObjectMotion (scene->ModelView (gpos, grot),
		havePrev ? scene->PrevModelView (prevGpos, prevGrot) : scene->PrevModelView (gpos, grot));
}

// ======================================================================
// VVessel

VVessel::VVessel (OBJHANDLE h, Scene *s)
	: VObject(h, s)
{
	vessel = oapiGetVesselInterface (hObj);
	LoadMeshes ();
}

VVessel::~VVessel ()
{
	// (the smoke lives on after the vessel's visual is gone: out of range,
	// or the vessel deleted; its rings are freed when it has faded)
	ParticleSystem *sys = scene->Particles ();
	for (auto *rings : { &smoke, &dust, &mist, &trail })
		for (auto &r : *rings)
			if (sys && r.sys == sys) sys->Release (r);
	for (auto &c : contrails) c.Release (sys);
	water.Release (sys);
	for (ParticleRing *r : { (ParticleRing*)&wake, (ParticleRing*)&afterglow })
		if (sys && r->sys == sys) sys->Release (*r);
	// restore animated vertex lists to their default state
	for (auto &it : vtxListDefaults)
		std::copy (it.second.begin(), it.second.end(), it.first);
}

void VVessel::LoadMeshes ()
{
	meshes.clear();
	UINT n = vessel->GetMeshCount();
	meshes.resize (n);
	for (UINT i = 0; i < n; i++)
		InsertMesh (i);
}

void VVessel::InsertMesh (UINT idx)
{
	if (idx >= meshes.size()) meshes.resize (idx + 1);
	MeshRec &rec = meshes[idx];
	rec = MeshRec();
	MESHHANDLE hMesh = vessel->GetMeshTemplate (idx);
	if (!hMesh) {
		const char *name = vessel->GetMeshName (idx);
		if (name) hMesh = oapiLoadMeshGlobal (name);
	}
	if (!hMesh) return;
	VulkanClient *gc = scene->Client();
	rec.mesh = std::make_unique<VkMesh> (gc->Ctx(), gc->TexMgr(), hMesh);
	const char *mname = vessel->GetMeshName (idx);
	std::string meshName = mname ? std::string (mname) : gc->MeshName (hMesh);
	if (!meshName.empty()) rec.mesh->ApplyMaterialConfig (gc->Materials(), meshName);
	vessel->GetMeshOffset (idx, rec.ofs);
	rec.visMode = vessel->GetMeshVisibilityMode (idx);
	rec.grpTransform.assign (rec.mesh->nGroup(), Mat4::Identity());
	rec.grpVisible.assign (rec.mesh->nGroup(), 1);
	damageVersion = 0; // (a new mesh: the parts broken off hidden again)
}

MESHHANDLE VVessel::GetMesh (UINT idx)
{
	// device mesh handle (see VESSEL::GetDevMesh)
	return (idx < meshes.size() && meshes[idx].mesh ? (MESHHANDLE)meshes[idx].mesh.get() : NULL);
}

int VVessel::VisEvent (DWORD msg, DWORD_PTR context)
{
	switch (msg) {
	case EVENT_VESSEL_INSMESH:
		InsertMesh ((UINT)context);
		return 1;
	case EVENT_VESSEL_DELMESH:
		if ((UINT)context == (UINT)-1) meshes.clear();
		else if ((UINT)context < meshes.size()) meshes[(UINT)context] = MeshRec();
		return 1;
	case EVENT_VESSEL_MESHVISMODE:
		if ((UINT)context < meshes.size())
			meshes[(UINT)context].visMode = vessel->GetMeshVisibilityMode ((UINT)context);
		return 1;
	case EVENT_VESSEL_MESHOFS:
		if ((UINT)context < meshes.size())
			vessel->GetMeshOffset ((UINT)context, meshes[(UINT)context].ofs);
		return 1;
	case EVENT_VESSEL_RESETANIM:
	case EVENT_VESSEL_CLEARANIM:
	case EVENT_VESSEL_DELANIM:
	case EVENT_VESSEL_NEWANIM:
		return 1; // animations are evaluated from their current state every frame
	}
	return 0;
}

void VVessel::Update ()
{
	VObject::Update ();
	UpdateAnimations ();
	UpdateDamage ();
	UpdateTracks ();
	UpdateWaterSpray ();
}

void VVessel::UpdateDamage ()
{
	// the triangles of parts broken off are not drawn
	DWORD v = oapiGetVesselDamageVersion (hObj);
	if (v != damageVersion) {
		damageVersion = v;
		for (size_t m = 0; m < meshes.size(); m++) {
			MeshRec &rec = meshes[m];
			if (!rec.mesh) continue;
			rec.hidden.resize (rec.mesh->nGroup());
			for (uint32_t g = 0; g < rec.mesh->nGroup(); g++) {
				std::vector<BYTE> &mask = rec.hidden[g];
				DWORD ntri = rec.mesh->GetGroup (g).nIndex / 3;
				mask.assign (ntri, 0);
				DWORD n = v ? oapiGetVesselHiddenTriangles (hObj, (UINT)m, g, mask.data(), ntri) : 0;
				if (!n) mask.clear ();
				rec.mesh->SetHiddenTriangles (g, n ? mask.data() : nullptr);
			}
			rec.cutState.clear (), rec.cutKeep.clear (), rec.cutLose.clear ();
		}
		if (v && fracture::CutTriangles ()) CutGroups ();
		wallsDirty = true;
	} else if (v && !wallsDirty) {
		// the walls are made for the vessel as posed: again when it moves
		// (a door opening away from the skin next to it)
		// (only groups at the gaps: not every control surface in flight)
		for (const MeshRec &rec : meshes)
			if (rec.wallPose.size() == rec.grpTransform.size()) {
				for (size_t g = 0; g < rec.wallPose.size() && !wallsDirty; g++)
					if (g < rec.wallWatch.size() && rec.wallWatch[g])
						for (int i = 0; i < 16; i++)
							if (fabs (rec.wallPose[g].m[i] - rec.grpTransform[g].m[i]) > 1e-4) { wallsDirty = true; break; }
			} else if (rec.mesh) wallsDirty = true;
	}
	double t = oapiGetSysTime ();
	if (wallsDirty && t - wallsBuilt >= 0.2) { // (not every frame while it moves)
		BuildWalls ();
		wallsDirty = false, wallsBuilt = t;
	}
}

namespace {
	// the inside of the skin is drawn within about this times the vessel's
	// size of the gaps where parts broke off
	constexpr double INSIDE_REACH = 0.15;

	/// A part with the texture and material of a group of a mesh
	VkMesh::Part GroupPart (const VkMesh &mesh, uint32_t g)
	{
		const VkMesh::Group &grp = mesh.GetGroup (g);
		VkMesh::Part p;
		p.tex = grp.tex;
		p.mat = mesh.Material (grp.material);
		p.pbr = mesh.PBR (grp.material);
		p.alphaTest = grp.alphaTest;
		return p;
	}

	/// Groups whose triangles are cut along the torn surface: drawn, opaque
	/// (the same on the vessel and on the pieces: from the mesh template)
	bool Cuttable (MESHHANDLE hm, DWORD g)
	{
		MESHGROUP *mg = oapiMeshGroup (hm, g);
		if (!mg || (mg->UsrFlag & (VkMesh::GRP_HIDDEN | VkMesh::GRP_ADDITIVE))) return false;
		if (mg->MtrlIdx != (DWORD)-1)
			if (MATERIAL *m = oapiMeshMaterial (hm, mg->MtrlIdx))
				if (m->diffuse.a < 0.999f) return false;
		return true;
	}
}

void VVessel::CutGroups ()
{
	// the parts not shown
	DWORD np = vessel->GetPartCount ();
	std::vector<uint8_t> gone (np, 0);
	bool any = false;
	for (DWORD k = 0; k < np; k++) {
		PARTINFO pi;
		if (vessel->GetPartInfo (k, &pi) && (pi.flags & PART_HIDDEN)) gone[k] = 1, any = true;
	}
	if (!any) return;
	fracture::Field field (vessel, size, (uint32_t)std::hash<std::string>() (vessel->GetName ()));
	auto side = [&] (const VECTOR3 &p) { int k = field.PartAt (p); return k < 0 || k >= (int)np || !gone[k]; };
	// the groups with triangles shown and not shown: each triangle cut
	// where the surface passes through it (the vessel's frame, unposed:
	// as the structure was made)
	for (size_t m = 0; m < meshes.size(); m++) {
		MeshRec &rec = meshes[m];
		if (!rec.mesh || !(rec.visMode & MESHVIS_EXTERNAL)) continue;
		MESHHANDLE hm = rec.mesh->Template ();
		if (!hm) continue;
		uint32_t ng = std::min (rec.mesh->nGroup(), (uint32_t)rec.hidden.size());
		rec.cutState.assign (ng, {}), rec.cutKeep.assign (ng, {}), rec.cutLose.assign (ng, {});
		for (uint32_t g = 0; g < ng; g++) {
			std::vector<BYTE> &mask = rec.hidden[g];
			if (mask.empty() || !Cuttable (hm, g)) continue;
			bool shown = std::any_of (mask.begin(), mask.end(), [] (BYTE b) { return b != 1; });
			MESHGROUP *mg = oapiMeshGroup (hm, g);
			if (!shown || !mg || mg->nIdx / 3 != mask.size()) continue;
			std::vector<uint8_t> &state = rec.cutState[g];
			state.assign (mask.size(), 0);
			for (size_t k = 0; k < mask.size(); k++) {
				NTVERTEX v[3];
				bool ok = true;
				for (int j = 0; j < 3 && ok; j++) {
					WORD i = mg->Idx[k * 3 + j];
					if (i >= mg->nVtx) ok = false;
					else v[j] = mg->Vtx[i];
				}
				if (!ok) continue;
				int r = fracture::Cut (v, rec.ofs, side, field.Resolution(), rec.cutKeep[g], rec.cutLose[g]);
				bool hid = mask[k] == 1;
				if ((r == 0 && !hid) || (r == 1 && hid)) continue; // (as it was)
				if (r != 2) (r ? rec.cutLose[g] : rec.cutKeep[g]).insert ((r ? rec.cutLose[g] : rec.cutKeep[g]).end(), v, v + 3);
				state[k] = 1, mask[k] = 1;
			}
			rec.mesh->SetHiddenTriangles (g, mask.data());
		}
	}
}

void VVessel::BuildWalls ()
{
	std::vector<fracture::Tri> tris; // (the skin, vessel frame, as posed)
	std::vector<std::pair<size_t, uint32_t>> srcs; // (mesh and group of each source)
	std::vector<std::vector<NTVERTEX>> inside;     // (the inside of the skin, by source)
	struct InsideOf { size_t tri; const NTVERTEX *a, *b, *c; };
	std::vector<InsideOf> insideOf;                 // (kept triangles that may show their inside)
	bool cut = false;
	for (size_t m = 0; m < meshes.size(); m++) {
		MeshRec &rec = meshes[m];
		rec.walls.reset ();
		rec.wallGroup.clear ();
		rec.wallPose = rec.grpTransform;
		rec.wallWatch.assign (rec.grpTransform.size(), 0);
		for (size_t g = 0; g < rec.hidden.size() && g < rec.wallWatch.size(); g++)
			if (!rec.hidden[g].empty()) rec.wallWatch[g] = 1;
		if (!rec.mesh || !damageVersion || !(rec.visMode & MESHVIS_EXTERNAL)) continue;
		MESHHANDLE hm = rec.mesh->Template ();
		if (!hm) continue;
		for (uint32_t g = 0; g < rec.mesh->nGroup() && g < rec.hidden.size(); g++) {
			const std::vector<BYTE> &mask = rec.hidden[g];
			MESHGROUP *mg = oapiMeshGroup (hm, g);
			DWORD ntri = rec.mesh->GetGroup (g).nIndex / 3;
			if (!mg || mg->nIdx / 3 != ntri) continue;
			cut |= !mask.empty();
			uint32_t src = (uint32_t)srcs.size();
			srcs.push_back ({ m, g });
			const VkMesh::Group &vg = rec.mesh->GetGroup (g);
			inside.emplace_back ();
			bool solid = !vg.transparent && !(vg.usrFlag & (VkMesh::GRP_HIDDEN | VkMesh::GRP_ADDITIVE));
			const Mat4 &M = g < rec.grpTransform.size() ? rec.grpTransform[g] : Mat4::Identity();
			const std::vector<uint8_t> *state = g < rec.cutState.size() && !rec.cutState[g].empty() ? &rec.cutState[g] : nullptr;
			for (DWORD k = 0; k < ntri; k++) {
				if (state && (*state)[k]) continue; // (drawn by its pieces)
				fracture::Tri t;
				VECTOR3 nm{};
				bool ok = true;
				for (int j = 0; j < 3; j++) {
					WORD i = mg->Idx[k * 3 + j];
					if (i >= mg->nVtx) { ok = false; break; }
					const NTVERTEX &vx = mg->Vtx[i];
					VECTOR3 p = _V(vx.x, vx.y, vx.z);
					t.p[j] = M.TransformPoint (p) + rec.ofs;
					nm += M.TransformPoint (p + _V(vx.nx, vx.ny, vx.nz)) - M.TransformPoint (p);
				}
				if (!ok || length (nm) < 1e-9) continue;
				t.n = unit (nm);
				t.src = src;
				t.kept = mask.empty() || mask[k] != 1;
				tris.push_back (t);
				if (t.kept && solid) insideOf.push_back ({ tris.size() - 1, &mg->Vtx[mg->Idx[k * 3]], &mg->Vtx[mg->Idx[k * 3 + 1]], &mg->Vtx[mg->Idx[k * 3 + 2]] });
			}
			// the pieces of the triangles cut
			if (state)
				for (int kept = 0; kept < 2; kept++) {
					const std::vector<NTVERTEX> &pc = kept ? rec.cutKeep[g] : rec.cutLose[g];
					for (size_t k = 0; k + 2 < pc.size(); k += 3) {
						fracture::Tri t;
						for (int j = 0; j < 3; j++) t.p[j] = M.TransformPoint (_V(pc[k + j].x, pc[k + j].y, pc[k + j].z)) + rec.ofs;
						VECTOR3 nm = crossp (t.p[1] - t.p[0], t.p[2] - t.p[0]);
						if (length (nm) < 1e-12) continue;
						VECTOR3 vn = _V(pc[k].nx + pc[k + 1].nx + pc[k + 2].nx, pc[k].ny + pc[k + 1].ny + pc[k + 2].ny, pc[k].nz + pc[k + 1].nz + pc[k + 2].nz);
						t.n = unit (nm);
						if (dotp (t.n, M.TransformPoint (vn) - M.TransformPoint (_V(0, 0, 0))) < 0.0) t.n = -t.n;
						t.src = src;
						t.kept = kept != 0;
						tris.push_back (t);
						if (t.kept && solid) insideOf.push_back ({ tris.size() - 1, &pc[k], &pc[k + 1], &pc[k + 2] });
					}
				}
		}
	}
	if (!cut) return;
	// the inside of the skin near the gaps (only there: elsewhere the vessel
	// looks as it did, e.g. through its windows)
	{
		double cell = INSIDE_REACH * size;
		auto key = [cell] (const VECTOR3 &p) {
			return std::make_tuple ((int64_t)std::floor (p.x / cell), (int64_t)std::floor (p.y / cell), (int64_t)std::floor (p.z / cell));
		};
		std::set<std::tuple<int64_t,int64_t,int64_t>> near;
		for (const fracture::Tri &t : tris)
			if (!t.kept) {
				auto [x, y, z] = key ((t.p[0] + t.p[1] + t.p[2]) / 3.0);
				for (int i = -1; i <= 1; i++) for (int j = -1; j <= 1; j++) for (int k = -1; k <= 1; k++)
					near.insert ({ x + i, y + j, z + k });
			}
		double ofs = 0.15 * fracture::WallDepth (size);
		for (const InsideOf &io : insideOf) {
			const fracture::Tri &t = tris[io.tri];
			if (near.count (key ((t.p[0] + t.p[1] + t.p[2]) / 3.0)))
				fracture::AddInside (*io.a, *io.b, *io.c, ofs, inside[t.src]);
		}
	}
	// walls along the cuts, each moving with the group it is cut in
	std::vector<std::vector<NTVERTEX>> walls;
	fracture::Walls (tris, fracture::WallDepth (size), 1e-3, (uint32_t)std::hash<std::string>() (vessel->GetName ()), walls);
	std::vector<std::vector<VkMesh::Part>> parts (meshes.size());
	for (size_t s = 0; s < walls.size(); s++) {
		if (walls[s].empty()) continue;
		auto [m, g] = srcs[s];
		// back to the frame of the group, unposed
		const MeshRec &rec = meshes[m];
		Mat4 Mi = g < rec.grpTransform.size() ? rec.grpTransform[g].AffineInverse () : Mat4::Identity();
		VECTOR3 o = Mi.TransformPoint (_V(0, 0, 0));
		for (NTVERTEX &vx : walls[s]) {
			VECTOR3 p = Mi.TransformPoint (_V(vx.x, vx.y, vx.z) - rec.ofs);
			VECTOR3 n = unit (Mi.TransformPoint (_V(vx.nx, vx.ny, vx.nz)) - o);
			vx.x = (float)p.x, vx.y = (float)p.y, vx.z = (float)p.z;
			vx.nx = (float)n.x, vx.ny = (float)n.y, vx.nz = (float)n.z;
		}
		fracture::AddParts (walls[s], parts[m]);
		meshes[m].wallGroup.resize (parts[m].size(), g);
	}
	for (size_t s = 0; s < inside.size(); s++) {
		auto [m, g] = srcs[s];
		fracture::AddParts (inside[s], parts[m], true);
		meshes[m].wallGroup.resize (parts[m].size(), g);
		// the pieces of its triangles cut, on the vessel (its texture and material)
		const MeshRec &rec = meshes[m];
		if (g < rec.cutKeep.size() && !rec.cutKeep[g].empty()) {
			fracture::AddParts (rec.cutKeep[g], parts[m], GroupPart (*rec.mesh, g));
			meshes[m].wallGroup.resize (parts[m].size(), g);
		}
	}
	for (MeshRec &rec : meshes)
		for (uint32_t g : rec.wallGroup)
			if (g < rec.wallWatch.size()) rec.wallWatch[g] = 1;
	VulkanClient *gc = scene->Client();
	for (size_t m = 0; m < meshes.size(); m++)
		if (!parts[m].empty())
			meshes[m].walls = std::make_unique<VkMesh> (gc->Ctx(), gc->TexMgr(), parts[m]);
}

void VVessel::UpdateTracks ()
{
	// wheels and gear on bare ground near the camera leave tracks
	GroundTracks *gt = scene->Tracks ();
	if (!gt) return;
	DWORD n = std::min (vessel->GetTouchdownPointCount (), (DWORD)3); // (the gear: the first three)
	OBJHANDLE hp = vessel->GetSurfaceRef ();
	PlumeRenderer *pr = scene->Plumes ();
	bool near = camDist < 3000.0 && hp && vessel->GroundContact ();
	for (DWORD i = 0; i < n; i++) {
		TOUCHDOWNVTX tv;
		if (!near || !vessel->GetTouchdownPoint (tv, i)) { gt->NoContact (hObj, (int)i); waterContact[i] = 0; continue; }
		VECTOR3 g, pp;
		vessel->Local2Global (tv.pos, g);
		oapiGetGlobalPos (hp, &pp);
		VECTOR3 up = unit (g - pp);
		// the ground below the point: bare terrain only (not bases or vessels)
		double t = 0.0;
		VECTOR3 vn = scene->ViewDir (up);
		int kind = 0;
		if (pr && pr->rayCast) {
			if (!pr->rayCast (id, scene->ViewPos (g + up), scene->ViewDir (-up), 1.5, t, vn, &kind) || kind != 0) {
				gt->NoContact (hObj, (int)i);
				waterContact[i] = 0;
				continue;
			}
		} else t = 1.0;
		if (vksurf::QueryGlobal (scene, hp, g).IsWater ()) { // (no tracks on water: foam and ripples round the leg)
			gt->NoContact (hObj, (int)i);
			if (WaterFx *wf = scene->Water ()) {
				VECTOR3 hv;
				vessel->GetGroundspeedVector (FRAME_HORIZON, hv);
				double R0 = std::clamp (0.03 * size, 0.4, 1.5);
				if (!waterContact[i] && -hv.y > 0.3) wf->Impulse (hp, g, 2.0 * R0, std::clamp (-hv.y / 3.0, 0.2, 1.0)); // (touching down)
				wf->Feed (hp, ((uint64_t)id << 16) | 0x100 | i, g, R0, std::clamp (0.3 + vessel->GetGroundspeed () / 10.0, 0.3, 1.0), 0.0, WaterFx::Kind::Contact);
			}
			waterContact[i] = 1;
			continue;
		}
		waterContact[i] = 0;
		// (track size from the vessel's size and mass: about 0.15 m wide and
		// 2 cm deep for the delta-glider)
		double hw = std::clamp (0.012 * size, 0.05, 0.4);
		double depth = std::clamp (0.01 * std::cbrt (vessel->GetMass () / 1000.0), 0.008, 0.08);
		gt->Contact (hObj, (int)i, hp, g + up * (1.0 - t), scene->GlobalDir (vn), hw, depth);
	}
}

// ----------------------------------------------------------------------
// Animations
//
// The mesh is stored in the animation's default state. Each component maps
// the animation state onto a progress 0..1 over [state0,state1]; its
// transformation is evaluated for the difference between the current and
// the default progress. Child components inherit the transformation of
// their parents.

namespace {

	double Progress (const ANIMATIONCOMP *c, double state)
	{
		if (c->state1 == c->state0) return state >= c->state0 ? 1.0 : 0.0;
		return std::clamp ((state - c->state0) / (c->state1 - c->state0), 0.0, 1.0);
	}

	Mat4 LocalTransform (const ANIMATIONCOMP *c, double state, double defstate)
	{
		double p = Progress (c, state), p0 = Progress (c, defstate);
		double dp = p - p0;
		MGROUP_TRANSFORM *tr = c->trans;
		if (!tr || dp == 0.0) return Mat4::Identity();
		switch (tr->Type()) {
		case MGROUP_TRANSFORM::ROTATE: {
			auto *rot = static_cast<MGROUP_ROTATE*>(tr);
			double len = length (rot->axis);
			if (len == 0.0) return Mat4::Identity();
			return Mat4::RotationAbout (rot->ref, rot->axis / len, rot->angle * dp);
		}
		case MGROUP_TRANSFORM::TRANSLATE: {
			auto *trn = static_cast<MGROUP_TRANSLATE*>(tr);
			return Mat4::Translation (trn->shift * dp);
		}
		case MGROUP_TRANSFORM::SCALE: {
			auto *scl = static_cast<MGROUP_SCALE*>(tr);
			auto f = [](double s, double prog) { return 1.0 + (s - 1.0) * prog; };
			auto ratio = [&](double s) { double d = f(s, p0); return d != 0.0 ? f(s, p) / d : 1.0; };
			return Mat4::Translation (scl->ref) *
				Mat4::Scale (ratio (scl->scale.x), ratio (scl->scale.y), ratio (scl->scale.z)) *
				Mat4::Translation (-scl->ref);
		}
		default:
			return Mat4::Identity();
		}
	}

} // namespace

void VVessel::UpdateAnimations ()
{
	for (auto &rec : meshes) {
		rec.prevGrpTransform = rec.grpTransform;
		std::fill (rec.grpTransform.begin(), rec.grpTransform.end(), Mat4::Identity());
	}

	ANIMATION *anim = nullptr;
	UINT nanim = vessel->GetAnimPtr (&anim);
	if (!nanim || !anim) return;

	// owning animation of each component (needed for the state of parent components)
	std::unordered_map<const ANIMATIONCOMP*, const ANIMATION*> owner;
	for (UINT a = 0; a < nanim; a++)
		for (UINT c = 0; c < anim[a].ncomp; c++)
			owner[anim[a].comp[c]] = &anim[a];

	std::unordered_map<const ANIMATIONCOMP*, Mat4> compMatrix;
	std::function<Mat4(const ANIMATIONCOMP*)> CompMatrix = [&](const ANIMATIONCOMP *c) -> Mat4 {
		auto it = compMatrix.find (c);
		if (it != compMatrix.end()) return it->second;
		const ANIMATION *a = owner.count (c) ? owner[c] : nullptr;
		Mat4 M = a ? LocalTransform (c, a->state, a->defstate) : Mat4::Identity();
		if (c->parent) M = CompMatrix (c->parent) * M;
		compMatrix[c] = M;
		return M;
	};

	for (UINT a = 0; a < nanim; a++) {
		for (UINT ci = 0; ci < anim[a].ncomp; ci++) {
			const ANIMATIONCOMP *c = anim[a].comp[ci];
			MGROUP_TRANSFORM *tr = c->trans;
			if (!tr || !tr->grp || !tr->ngrp) continue;
			Mat4 M = CompMatrix (c);
			if (tr->mesh == LOCALVERTEXLIST) {
				VECTOR3 *list = (VECTOR3*)tr->grp;
				auto it = vtxListDefaults.find (list);
				if (it == vtxListDefaults.end())
					it = vtxListDefaults.emplace (list, std::vector<VECTOR3> (list, list + tr->ngrp)).first;
				for (UINT k = 0; k < tr->ngrp && k < it->second.size(); k++)
					list[k] = M.TransformPoint (it->second[k]);
			} else if (tr->mesh < meshes.size() && meshes[tr->mesh].mesh) {
				MeshRec &rec = meshes[tr->mesh];
				for (UINT k = 0; k < tr->ngrp; k++) {
					UINT g = tr->grp[k];
					if (g < rec.grpTransform.size())
						rec.grpTransform[g] = M * rec.grpTransform[g];
				}
			}
		}
	}
}

void VVessel::Render (RenderPass pass)
{
	bool internal = oapiCameraInternal() && oapiCameraTarget() == hObj;
	bool vc = internal && oapiCockpitMode() == COCKPIT_VIRTUAL;
	Mat4 mv = scene->ModelView (gpos, grot);
	scene->Sunlight (gpos);
	scene->SetObjectGlassGlow (glassGlow);
	if (pass == RenderPass::Depth) SetMotion ();
	SetHeating ();

	// virtual cockpit displays: MFD and HUD surfaces replace the textures of the registered groups
	std::vector<std::vector<GroupTexOverride>> ovr;
	if (vc) {
		ovr.resize (meshes.size());
		VulkanClient *gc = scene->Client();
		for (int i = 0; i < MAXMFD; i++) {
			const VCMFDSPEC *spec = nullptr;
			SURFHANDLE surf = gc->GetVCMFDSurface (i, &spec);
			if (surf && spec && spec->nmesh < meshes.size())
				ovr[spec->nmesh].push_back ({ spec->ngroup, (VkTexture*)surf, false });
		}
		const VCHUDSPEC *hs = nullptr;
		SURFHANDLE hud = gc->GetVCHUDSurface (&hs);
		if (hud && hs && hs->nmesh < meshes.size())
			ovr[hs->nmesh].push_back ({ hs->ngroup, (VkTexture*)hud, true });
	}

	for (size_t m = 0; m < meshes.size(); m++) {
		MeshRec &rec = meshes[m];
		if (!rec.mesh) continue;
		bool visible;
		if (!internal)
			visible = (rec.visMode & MESHVIS_EXTERNAL) != 0;
		else
			visible = (rec.visMode & MESHVIS_COCKPIT) || (vc && (rec.visMode & MESHVIS_VC)) || (rec.visMode & MESHVIS_EXTPASS);
		if (!visible) continue;
		Mat4 meshMv = mv * Mat4::Translation (rec.ofs);
		scene->DrawMesh (rec.mesh.get(), meshMv, pass, &rec.grpTransform, &rec.grpVisible,
			(m < ovr.size() && !ovr[m].empty()) ? &ovr[m] : nullptr,
			rec.prevGrpTransform.size() == rec.grpTransform.size() ? &rec.prevGrpTransform : nullptr);
		if (rec.walls) {
			WallTransforms (rec);
			scene->DrawMesh (rec.walls.get(), meshMv, pass, &wallTf, &wallVis, nullptr, &wallPrevTf);
		}
	}
	static const float noGlow[3] = { 0, 0, 0 };
	scene->SetObjectGlassGlow (noGlow);
	scene->SetObjectHeat (_V(0, 0, 0), 0.0f);
	if (pass == RenderPass::Transparent) RenderExhaust ();
}

bool VVessel::Sheath (SheathState &s) const
{
	// stagnation-point heat flux (Sutton-Graves, 1 m nose radius) [W/m^2]
	VESSEL *v = oapiGetVesselInterface (hObj);
	VECTOR3 as;
	v->GetAirspeedVector (FRAME_LOCAL, as);
	double spd = length (as), rho = v->GetAtmDensity ();
	s.flux = 1.74e-4 * sqrt (rho) * spd * spd * spd;
	const double qMin = 5e4, qMax = 8e5; // glow onset (a faint one on a 2 km/s suborbital reentry), full glow
	s.level = (float)std::clamp ((s.flux - qMin) / (qMax - qMin), 0.0, 1.0);
	if (s.level <= 0.0f || spd <= 0.0) return false;
	s.dirGlobal = mul (grot, as / spd);
	s.dirView = scene->ViewDir (s.dirGlobal);
	return true;
}

void VVessel::SetHeating ()
{
	SheathState s;
	if (!Sheath (s)) {
		scene->SetObjectHeat (_V(0, 0, 0), 0.0f);
		return;
	}
	scene->SetObjectHeat (s.dirView, s.level);
}

void VVessel::DrawPlasma (PlasmaRenderer &pr)
{
	// (the shell around the hull seen from outside; from the cockpit the
	// hull is not drawn to hide it)
	if (oapiCameraInternal () && oapiCameraTarget () == hObj) return;
	if (reentryLegacy < 0) {
		PlumeRenderer *plumes = scene->Plumes ();
		reentryLegacy = plumes && plumes->config.ReentryLegacy (vessel) ? 1 : 0;
	}
	if (reentryLegacy) return;
	SheathState s;
	if (!Sheath (s)) return;
	Mat4 mv = scene->ModelView (gpos, grot);
	for (size_t m = 0; m < meshes.size(); m++) {
		MeshRec &rec = meshes[m];
		if (!rec.mesh || !(rec.visMode & MESHVIS_EXTERNAL)) continue;
		pr.DrawMesh (rec.mesh.get(), mv * Mat4::Translation (rec.ofs), &rec.grpTransform, &rec.grpVisible, s, size);
	}
}

double VVessel::LegacyReentry () const
{
	double s = -1.0;
	for (const VkParticleStream *ps : scene->Client ()->ParticleStreams ())
		if (ps->GetKind () == VkParticleStream::Kind::Reentry && ps->Source () == hObj) s = std::max (s, ps->Strength ());
	return s;
}

// The plasma's wake: the shock layer's glowing gas streams off the edge of
// the sheath and closes behind the vessel in a luminous tail some tens of
// vessel lengths long, cooling from the sheath's colour through orange to a
// dull red (ParticleSystem::Emission::ramp); each particle drawn as a streak
// along its motion through the air. Far behind, an afterglow along the path:
// the ionised air recombining for a few seconds, a faint streak kilometres
// long that shows a reentry from afar at night.
void VVessel::EmitWake (double dt)
{
	ParticleSystem *sys = scene->Particles ();
	if (reentryLegacy < 0) {
		PlumeRenderer *plumes = scene->Plumes ();
		reentryLegacy = plumes && plumes->config.ReentryLegacy (vessel) ? 1 : 0;
	}
	SheathState sh;
	if (!sys || !sys->enabled || dt <= 0.0 || reentryLegacy == 1 || !Sheath (sh)) return;
	// (an add-on's reentry streams decide when it starts: their amin/amax
	// over the friction power; none: the heating alone)
	double leg = LegacyReentry ();
	double k = sh.level * (leg < 0.0 ? 1.0 : std::clamp (leg / 0.15, 0.0, 1.0));
	if (k < 0.02) return;
	VECTOR3 vel, air;
	vessel->GetGlobalVel (vel);
	if (!vessel->GetAirspeedVector (FRAME_GLOBAL, air)) return;
	double V = length (air);
	if (V < 300.0) return;
	VECTOR3 f = air / V;          // motion through the air (global)
	VECTOR3 airVel = vel - air;   // the air's own velocity
	// the vessel as an ellipsoid (semi-axes from its cross sections: an
	// ellipsoid's along x is pi b c, ...), its outline across the flow
	VECTOR3 cs;
	vessel->GetCrossSections (cs);
	cs.x = std::max (cs.x, 0.01), cs.y = std::max (cs.y, 0.01), cs.z = std::max (cs.z, 0.01);
	double ax[3] = { sqrt (cs.y * cs.z / (PI * cs.x)), sqrt (cs.x * cs.z / (PI * cs.y)), sqrt (cs.x * cs.y / (PI * cs.z)) };
	for (double &a : ax) a = std::clamp (a, 0.05 * size, size);
	double len = 2.0 * std::max ({ ax[0], ax[1], ax[2] }); // [m] its length
	VECTOR3 fl = tmul (grot, f);  // (local)
	VECTOR3 u1 = unit (crossp (fl, std::fabs (fl.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0))), u2 = crossp (fl, u1);
	// (the gas leaving the sheath has cooled a little: its colour a step
	// down the sheath's, orange-pink at peak heating)
	float col[3];
	SheathState::Colour (0.7f * sh.level, col);
	col[1] *= 0.94f, col[2] = std::min (1.0f, 1.08f * col[2] + 0.03f); // (a touch pinker: dim orange edges read as brown smoke)
	VkTexture *tex = scene->FireballTexture (5); // (a smooth glow: puffs with structure read as smoke)

	// near: from the outline (pushed a little downstream) and the tail's
	// core; the gas entrained at a fraction of the vessel's speed (a streak
	// of its motion through the air) and left behind, the wake some 15
	// vessel lengths long
	const int NRIM = 12;
	const double ENTRAIN = 0.2;
	static std::minstd_rand rngWake (777);
	double life = std::clamp (15.0 * len / ((1.0 - ENTRAIN) * V), 0.04, 1.0);
	double psize = 0.28 * len;
	// (a streak is drawn centred on its particle: emitted half its length
	// downstream, so that it does not reach ahead of the hull)
	double streak = 0.015;                                       // [s]
	double half = streak * ENTRAIN * V;                          // [m]
	double spacing = std::max (0.2 * psize, 0.2 * half); // (overlapping a lot: one luminous body, not beads)
	double rate = std::min ((1.0 - ENTRAIN) * V / spacing, 1500.0) * (NRIM + 2); // [1/s] all sources
	if (sys->Reserve (wake, rate, life, ParticleSystem::Class::PlasmaWake, 0.0, tex, true)) {
		wake.accum += rate * dt;
		uint32_t n = (uint32_t)wake.accum;
		wake.accum -= n;
		// (a share each for the rim's sources, the core the rest: about two)
		uint32_t per = std::max (n / (NRIM + 2), 1u);
		int first = (int)(rngWake () % NRIM);
		for (int j = 0; j <= NRIM && n; j++) {
			int i = j == NRIM ? NRIM : (first + j) % NRIM;
			uint32_t m = i == NRIM ? n : std::min (n, per);
			n -= m;
			VECTOR3 lp;
			if (i < NRIM) {
				double a = (i + 0.5) * 2.0 * PI / NRIM;
				VECTOR3 u = u1 * cos (a) + u2 * sin (a);
				double sup = sqrt ((u.x * ax[0]) * (u.x * ax[0]) + (u.y * ax[1]) * (u.y * ax[1]) + (u.z * ax[2]) * (u.z * ax[2]));
				lp = u * (0.9 * sup);
			} else lp = _V(0, 0, 0);
			VECTOR3 src = gpos + mul (grot, lp) - f * (0.25 * len + half);
			ParticleSystem::Emission e{};
			e.p1 = src, e.p0 = src - vel * dt;
			e.vel = airVel + air * ENTRAIN, e.dir = f;
			e.v0 = 0.0, e.spread = 0.006 * V;
			e.lifetime = life, e.dt = dt;
			e.size = psize * (i == NRIM ? 1.4 : 1.0), e.growth = 0.4 * len / life;
			e.alpha = 0.06 * k * k; // (luminous, not a searchlight: brighter, it took the auto exposure down - the night sky went black)
			e.slowdown = 2.0;
			e.emissive = true, e.tinted = true, e.ramp = true;
			for (int c = 0; c < 3; c++) e.albedo[c] = col[c];
			e.cls = ParticleSystem::Class::PlasmaWake;
			e.streak = (float)streak;
			sys->Emit (wake, e, m);
		}
	}
	// far: the afterglow, at rest in the air, large enough to hold a pixel
	// or two from tens of kilometres
	if (k > 0.2) {
		double gsize = std::max (2.0 * len, 30.0);
		double glife = 3.0 * std::min (1.0, k * 1.5);
		double grate = std::min (V / (0.5 * gsize), 600.0);
		if (sys->Reserve (afterglow, grate, glife, ParticleSystem::Class::PlasmaWake, 0.0, scene->FireballTexture (3), true)) {
			afterglow.accum += grate * dt;
			uint32_t n = (uint32_t)afterglow.accum;
			afterglow.accum -= n;
			if (n) {
				VECTOR3 src = gpos - f * (2.0 * len);
				ParticleSystem::Emission e{};
				e.p1 = src, e.p0 = src - vel * dt;
				e.vel = airVel, e.dir = f;
				e.v0 = 0.0, e.spread = 3.0;
				e.lifetime = glife, e.dt = dt;
				e.size = gsize, e.growth = 0.3 * gsize / glife;
				e.alpha = 0.08 * (k - 0.2) / 0.8;
				e.slowdown = 0.5;
				e.emissive = true, e.tinted = true, e.ramp = true;
				// (the recombining air: the sheath's colour towards violet)
				float hc[3];
				SheathState::Colour (sh.level, hc);
				e.albedo[0] = 0.85f * hc[0], e.albedo[1] = 0.55f * hc[1], e.albedo[2] = std::min (1.0f, 0.4f + 0.6f * hc[2]);
				e.cls = ParticleSystem::Class::PlasmaWake;
				e.streak = -1.0f;
				sys->Emit (afterglow, e, n);
			}
		}
	}
}

// ======================================================================
// VDebris

void VDebris::CutPiece (const VESSEL *src, const DEBRISINFO &info, std::vector<PieceCut> &cuts)
{
	// the offset of each group's mesh in the vessel (its frame: where the
	// structure's parts are)
	std::vector<DEBRISGROUP> dgs (info.ngroup);
	std::vector<VECTOR3> vofs (info.ngroup);
	std::vector<uint8_t> found (info.ngroup, 0);
	for (DWORD i = 0; i < info.ngroup; i++) {
		if (!oapiGetDebrisGroup (hObj, i, &dgs[i]) || !dgs[i].hMesh) continue;
		for (UINT m = 0; m < src->GetMeshCount (); m++) {
			MESHHANDLE hm = src->GetMeshTemplate (m);
			if (!hm)
				if (const char *name = src->GetMeshName (m)) hm = oapiLoadMeshGlobal (name);
			if (hm == dgs[i].hMesh) { src->GetMeshOffset (m, vofs[i]); found[i] = 1; break; }
		}
	}
	// its parts (not known for pieces from scenarios: those lost where its
	// triangles are)
	DWORD np = src->GetPartCount ();
	unsigned long long mine = info.parts;
	if (!mine)
		for (DWORD i = 0; i < info.ngroup; i++) {
			if (!found[i]) continue;
			const DEBRISGROUP &dg = dgs[i];
			for (DWORD k = 0; k + 2 < dg.nvtx; k += 3) {
				VECTOR3 c = (_V(dg.vtx[k].x, dg.vtx[k].y, dg.vtx[k].z) + _V(dg.vtx[k + 1].x, dg.vtx[k + 1].y, dg.vtx[k + 1].z) +
					_V(dg.vtx[k + 2].x, dg.vtx[k + 2].y, dg.vtx[k + 2].z)) / 3.0 - dg.ofs + vofs[i];
				int p = src->GetPartAt (c);
				PARTINFO pi;
				if (p >= 0 && p < 64 && src->GetPartInfo ((DWORD)p, &pi) && (pi.flags & PART_LOST)) mine |= 1ull << p;
			}
		}
	if (!mine) return;
	fracture::Field field (src, oapiGetSize (src->GetHandle ()), (uint32_t)std::hash<std::string>() (std::string (info.source)));
	auto side = [&] (const VECTOR3 &p) { int k = field.PartAt (p); return k >= 0 && k < 64 && k < (int)np && ((mine >> k) & 1); };
	// the groups only partly in it: all the triangles of the mesh's group
	// cut where the surface passes through them
	for (DWORD i = 0; i < info.ngroup; i++) {
		const DEBRISGROUP &dg = dgs[i];
		if (!found[i] || !Cuttable (dg.hMesh, dg.group)) continue;
		MESHGROUP *mg = oapiMeshGroup (dg.hMesh, dg.group);
		if (!mg || mg->nIdx / 3 <= dg.nvtx / 3) continue;
		PieceCut &c = cuts[i];
		c.on = true;
		for (DWORD k = 0; k + 2 < mg->nIdx; k += 3) {
			NTVERTEX v[3];
			bool ok = true;
			for (int j = 0; j < 3 && ok; j++) {
				WORD x = mg->Idx[k + j];
				if (x >= mg->nVtx) ok = false;
				else v[j] = mg->Vtx[x];
			}
			if (!ok) continue;
			int r = fracture::Cut (v, vofs[i], side, field.Resolution(), c.keep, c.lose);
			if (r != 2) (r ? c.lose : c.keep).insert ((r ? c.lose : c.keep).end(), v, v + 3);
		}
		// (in the piece's frame)
		for (auto *pc : { &c.keep, &c.lose })
			for (NTVERTEX &x : *pc) x.x += (float)dg.ofs.x, x.y += (float)dg.ofs.y, x.z += (float)dg.ofs.z;
	}
}

VDebris::VDebris (OBJHANDLE h, Scene *s, DWORD sn)
	: VObject (h, s), serial (sn)
{
	DEBRISINFO info;
	if (!oapiGetDebrisInfo (hObj, &info)) return;
	OBJHANDLE hSrc = oapiGetVesselByName (info.source);
	std::vector<PieceCut> cuts (info.ngroup);
	if (hSrc && fracture::CutTriangles ()) CutPiece (oapiGetVesselInterface (hSrc), info, cuts);
	std::vector<VkMesh::Part> parts;
	for (DWORD i = 0; i < info.ngroup; i++) {
		DEBRISGROUP dg;
		if (!oapiGetDebrisGroup (hObj, i, &dg) || dg.nvtx < 3) continue;
		// the texture and material of the vessel's mesh group
		VkMesh::Part base;
		if (dg.hMesh) {
			if (MESHGROUP *mg = oapiMeshGroup (dg.hMesh, dg.group)) {
				if (mg->MtrlIdx != (DWORD)-1) // (none: the default material)
					if (MATERIAL *m = oapiMeshMaterial (dg.hMesh, mg->MtrlIdx)) base.mat = *m;
				if (mg->TexIdx != (DWORD)-1)
					base.tex = (VkTexture*)oapiGetTextureHandle (dg.hMesh, mg->TexIdx + 1);
			}
		}
		base.pbr = PBRParams::FromMaterial (base.mat);
		// the triangles at the fracture: charred (dark, rough)
		VkMesh::Part charred = base;
		for (float *c : { &charred.mat.diffuse.r, &charred.mat.diffuse.g, &charred.mat.diffuse.b,
		                  &charred.mat.ambient.r, &charred.mat.ambient.g, &charred.mat.ambient.b }) *c *= 0.18f;
		charred.mat.specular = { 0.02f, 0.02f, 0.02f, 1.0f };
		charred.pbr = PBRParams::FromMaterial (charred.mat);
		charred.pbr.roughness = 0.95f;
		charred.pbr.metalness = 0.0f;
		if (cuts[i].on) { // (its pieces of the triangles cut)
			fracture::AddParts (cuts[i].keep, parts, base);
			continue;
		}
		int cur[2] = { -1, -1 }; // (the parts being filled: plain, charred)
		for (DWORD k = 0; k + 2 < dg.nvtx; k += 3) {
			int c = dg.edge && dg.edge[k / 3] > 0.5f ? 1 : 0;
			// (16-bit indices: a new part when one is full)
			if (cur[c] < 0 || parts[cur[c]].vtx.size() + 3 > 65535) {
				parts.push_back (c ? charred : base);
				cur[c] = (int)parts.size() - 1;
			}
			VkMesh::Part &p = parts[cur[c]];
			for (int j = 0; j < 3; j++) {
				p.idx.push_back ((uint16_t)p.vtx.size());
				p.vtx.push_back (dg.vtx[k + j]);
			}
		}
	}
	// walls along the cut edges (as deep as on the vessel it broke off)
	std::vector<fracture::Tri> tris;
	for (DWORD i = 0; i < info.ngroup; i++) {
		if (cuts[i].on) {
			for (int kept = 0; kept < 2; kept++) {
				const std::vector<NTVERTEX> &pc = kept ? cuts[i].keep : cuts[i].lose;
				for (size_t k = 0; k + 2 < pc.size(); k += 3) {
					fracture::Tri t;
					for (int j = 0; j < 3; j++) t.p[j] = _V(pc[k + j].x, pc[k + j].y, pc[k + j].z);
					VECTOR3 nm = crossp (t.p[1] - t.p[0], t.p[2] - t.p[0]);
					if (length (nm) < 1e-12) continue;
					t.n = unit (nm);
					if (dotp (t.n, _V(pc[k].nx + pc[k + 1].nx + pc[k + 2].nx, pc[k].ny + pc[k + 1].ny + pc[k + 2].ny, pc[k].nz + pc[k + 1].nz + pc[k + 2].nz)) < 0.0) t.n = -t.n;
					t.kept = kept != 0;
					tris.push_back (t);
				}
			}
			continue;
		}
		DEBRISGROUP dg;
		if (!oapiGetDebrisGroup (hObj, i, &dg) || !dg.edge) continue;
		for (DWORD k = 0; k + 2 < dg.nvtx; k += 3) {
			fracture::Tri t;
			VECTOR3 n{};
			for (int j = 0; j < 3; j++) {
				const NTVERTEX &v = dg.vtx[k + j];
				t.p[j] = _V(v.x, v.y, v.z);
				n += _V(v.nx, v.ny, v.nz);
			}
			if (length (n) < 1e-9) continue;
			t.n = unit (n);
			t.open = dg.edge[k / 3] > 0.5f;
			tris.push_back (t);
		}
	}
	double depth = fracture::WallDepth (hSrc ? oapiGetSize (hSrc) : info.size);
	std::vector<NTVERTEX> inside; // (of the skin, but not of its windows)
	for (DWORD i = 0; i < info.ngroup; i++) {
		DEBRISGROUP dg;
		if (!oapiGetDebrisGroup (hObj, i, &dg)) continue;
		if (dg.hMesh)
			if (MESHGROUP *mg = oapiMeshGroup (dg.hMesh, dg.group))
				if (mg->MtrlIdx != (DWORD)-1)
					if (MATERIAL *m = oapiMeshMaterial (dg.hMesh, mg->MtrlIdx))
						if (m->diffuse.a < 0.999f) continue;
		const NTVERTEX *vtx = cuts[i].on ? cuts[i].keep.data() : dg.vtx;
		size_t nvtx = cuts[i].on ? cuts[i].keep.size() : dg.nvtx;
		for (size_t k = 0; k + 2 < nvtx; k += 3)
			fracture::AddInside (vtx[k], vtx[k + 1], vtx[k + 2], 0.15 * depth, inside);
	}
	std::vector<std::vector<NTVERTEX>> walls;
	fracture::Walls (tris, depth, 1e-3, serial, walls);
	for (auto &w : walls) fracture::AddParts (w, parts);
	fracture::AddParts (inside, parts, true);
	if (parts.empty()) return;
	VulkanClient *gc = scene->Client();
	mesh = std::make_unique<VkMesh> (gc->Ctx(), gc->TexMgr(), parts);
}

namespace {
	// the fireballs alive (their owners): a ball forming inside one that
	// formed a little earlier joins it (PLAN.md 2.6: spheres overlapping by
	// more than half merge)
	std::vector<VDebris*> &LiveFireballs () { static std::vector<VDebris*> v; return v; }
}

VDebris::~VDebris ()
{
	auto &live = LiveFireballs ();
	live.erase (std::remove (live.begin (), live.end (), this), live.end ());
	ParticleSystem *sys = scene->Particles ();
	for (Ring *r : { &fire, &smoke, &sparks, &dust, &frags, &fbFire, &fbSmoke, &fbCore, &fbFlashR, &fbSkirt, &fbSteam, &column, &drift, &quench, &embers, &vent })
		if (r->sys && r->sys == sys) sys->Release (*r); // (retired with r->tex while its particles live; rings idle for long were released already)
	water.Release (sys);
}

void VDebris::Update ()
{
	VObject::Update ();
	hidden = !oapiGetDebrisInfo (hObj, &info) || (info.flags & DEBRIS_HIDDEN);
	if (!hidden) Effects ();
}

namespace {
	// the fireball of M [kg] of propellant released by a break-up in air
	// (out/fx-explosion/PLAN.md section 5): of the liquid sprayed by a crash
	// only a part mixes with the air and burns in the ball (f_p: 0.35; on
	// the water 0.25, the rest goes into the sea), the rest pools and burns
	// where it fell (CrashFires)
	struct FireballModel {
		double fp, Mp;  // the share burning in the ball, its mass [kg]
		double R;       // [m] its radius (hydrocarbon fireballs: diameter about 5.8 m per cube root of kg)
		double tf;      // [s] its flames' time: bright at 0.25, lifting off at 0.35, burned out by 1.0, gone by 1.3 of it
		double tw;      // [s] the flames of the wreck and of the spill (then smouldering)
		double soot;    // [s] its soot cap visible, rising
		double blast;   // 0: a deflagration (no pressure ring, no condensation cloud - PLAN.md section 3); > 0 reserved for a detonation
	};
	FireballModel FireModel (double M, bool onWater)
	{
		FireballModel m;
		M = std::max (M, 0.0);
		m.fp = onWater ? 0.25 : 0.35;
		m.Mp = m.fp * M;
		m.R = 2.9 * cbrt (m.Mp);
		// (between the BLEVE's M^(1/3) and footage: half a second for a few
		// kilograms, a couple of seconds for tonnes)
		m.tf = std::clamp (0.45 * pow (std::max (m.Mp, 1e-3), 0.2), 0.4, 6.0);
		m.tw = std::clamp (2.0 + 0.4 * cbrt (M), 2.0, 15.0);
		m.soot = 8.0 + 3.0 * cbrt (m.Mp);
		m.blast = 0.0;
		return m;
	}

	double Smooth01 (double x) { x = std::clamp (x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }

	// the air at a point, for an explosion's effects (out/fx-explosion/
	// VACUUM_PLAN.md 3.1): three things that one density threshold (1e-4
	// kg/m^3: a sea-level fireball at 60 km, a vacuum flash at 70, a burning
	// wreck and its smoke on Mars) decided, each continuous and from what it
	// depends on
	struct Air {
		double rho = 0.0, p = 0.0, O2pp = 0.0; // [kg/m^3], [Pa], [Pa]
		/// the drag on its particles, the buoyancy and suspension of its
		/// smoke, by density: 1 at sea level (as engine smoke's,
		/// ParticleSystem::ExhaustDrag), 0.13 on Mars or at 30 km, none in vacuum
		double Drag () const { return std::min (std::sqrt (std::max (rho, 0.0) / 1.225), 3.0); }
		/// what may burn on (0..1) - a fuel spill, burning wreckage and its
		/// smoke, embers - needs the air's oxygen (the burst carries its own
		/// oxidiser): full above 8 kPa of it, none below 1.5 kPa (Earth above
		/// about 18 km; Mars, Venus, Titan)
		double Burn () const { return Smooth01 ((O2pp - 1.5e3) / 6.5e3); }
		/// M [kg] of propellant released here held together as a fireball (1)
		/// or expanding freely (0): where its gas would stop at this pressure
		/// (the sea-level ball's radius times (p0 / p)^(1/3)) against where a
		/// free cloud of it thins out, its optical depth 0.05 (droplets and
		/// ice, about 1 m^2/kg). 7 t: 1 at sea level, at 30 km and on Mars,
		/// 0.67 at 50 km, 0.26 at 60 km, 0 at 70 km
		double Confine (double M) const
		{
			if (p <= 0.0 || M <= 0.0) return 0.0;
			double Req = FireModel (M, false).R * cbrt (101325.0 / std::max (p, 1e-3));
			double Rvis = std::sqrt (KAPPA * M / (PI * 0.05));
			return 1.0 - Smooth01 ((Req / Rvis - 0.5) / 2.5);
		}
		static constexpr double KAPPA = 1.0; // [m^2/kg] the free cloud's extinction per mass (droplets of a millimetre or so; at 2 a DG's cloud hung as a grey veil for over a second)
	};

	// the free cloud of M [kg] of propellant out of the air (VACUUM_PLAN.md
	// 3.3; FireballRenderer, fireball_grid.comp): self-similar - a Gaussian
	// of scale s = s0 + v t, every parcel on at the speed it got; its density
	// falls as 1 / s^3, its column through the centre as 1 / s^2: it thins
	// out by itself, gone at a column depth of 0.06
	struct FreeCloud {
		double s0, v, M;
		double S (double t) const { return s0 + v * std::max (t, 0.0); }
		double Sigma (double t) const { double s = S (t); return Air::KAPPA * M / (5.5683280 * s * s * s); } // (pi^1.5)
		double SEnd () const { return std::sqrt (Air::KAPPA * M / (PI * 0.06)); }
		double TEnd () const { return std::max (SEnd () - s0, 0.0) / std::max (v, 1.0); }
	};
	/// a burst's free cloud: R the radius its propellant's ball would have (2.9 M^1/3), v its speed
	FreeCloud CloudOf (double M, double R, double v) { return FreeCloud{ 0.12 * R, v, M }; }
	/// the air of body hp at global position gpos (none: an airless body, or above its atmosphere)
	Air AirAt (OBJHANDLE hp, const VECTOR3 &gpos)
	{
		Air a;
		const ATMCONST *ac = hp ? oapiGetPlanetAtmConstants (hp) : nullptr;
		if (!ac) return a;
		VECTOR3 bp;
		oapiGetGlobalPos (hp, &bp);
		ATMPARAM atm{};
		oapiGetPlanetAtmParams (hp, length (gpos - bp), &atm); // (by radius)
		a.rho = atm.rho, a.p = atm.p;
		a.O2pp = ac->p0 > 0.0 ? ac->O2pp * atm.p / ac->p0 : 0.0; // (well mixed)
		return a;
	}
}

double VDebris::FireballRadius () const
{
	return FireModel (info.fuel, false).R;
}

bool VDebris::Emit (Ring &r, ParticleSystem::Emission e, uint32_t n, double ringLife, bool burst)
{
	ParticleSystem *sys = scene->Particles ();
	if (!sys || !sys->enabled || !n) return false;
	// the ring for this emission (twice it) and the rate of emissions that
	// recur (n since the last one); grown, shrunk or chained with them,
	// released when idle (ParticleSystem::Reserve)
	double simt = oapiGetSimTime ();
	double gap = r.sys == sys && r.lastEmit > -1e29 ? simt - r.lastEmit : 1e30;
	double rate = burst || e.dt <= 0.0 ? 0.0 : n / std::max (std::max (e.dt, gap), 1e-3);
	// (a burst, or the first of a stream: room for it twice)
	if (!sys->Reserve (r, rate, std::max (e.lifetime, ringLife), ParticleSystem::Resolve (e), rate > 0.0 ? 0.0 : 2.0 * n, r.tex, r.emissive))
		return false;
	// (particle_emit.comp: a particle emitted at fraction f of the step at
	// mix (p0, p1, f) moves on with its velocity for the rest of the step:
	// a source at rest in the global frame moves with the velocity given,
	// e.g. the planet's orbital velocity, from p1 - vel dt to p1)
	if (e.p0.x == e.p1.x && e.p0.y == e.p1.y && e.p0.z == e.p1.z) e.p0 = e.p1 - e.vel * e.dt;
	sys->Emit (r, e, n);
	return true;
}

namespace {
	// velocity of the ground (and the still air) of body hp under global
	// position p (global frame). (Not crossp (omega, r): in Orbiter's
	// left-handed frame that turns the wrong way - the debris' "air" moved
	// at twice the ground's speed, 816 m/s off at the Cape)
	VECTOR3 GroundVelocity (OBJHANDLE hp, const VECTOR3 &p)
	{
		double lng, lat, rad;
		oapiGlobalToEqu (hp, p, &lng, &lat, &rad);
		return oapiGetGroundVector (hp, lng, lat, 3);
	}

	// the fireball's flames slow down in the air [1/s] (they spread about
	// its centre at its size times this)
	constexpr double FIREBALL_SLOWDOWN = 3.0;
	// its centre: braked gently (the fuel travels on with the wreck for the
	// first second) [1/s]
	constexpr double FIREBALL_BRAKE = 1.0;
	// a burning piece's flames trail this many times its size behind it
	constexpr double FLAME_LENGTH = 4.0;
}

void VDebris::Effects ()
{
	ParticleSystem *sys = scene->Particles ();
	// (frozen while paused, as the rest of the simulation: the step keeps its
	// last value then, and the particles would pile up without ageing)
	double dt = oapiGetPause () ? 0.0 : oapiGetSimStep ();
	if (!sys || !sys->enabled || dt <= 0.0) return;
	double simt = oapiGetSimTime ();
	double D = sys->density, sz = std::pow (D, -1.0 / 3.0);
	VECTOR3 vel;
	oapiGetGlobalVel (hObj, &vel);
	OBJHANDLE hp = oapiGetGbodyByName ((char*)"Earth");
	{   // (the planet below: its atmosphere and up direction)
		double dmin = 1e100;
		for (DWORD i = 0; i < oapiGetGbodyCount(); i++) {
			OBJHANDLE b = oapiGetGbodyByIndex (i);
			VECTOR3 bp;
			oapiGetGlobalPos (b, &bp);
			double d = length (gpos - bp) - oapiGetSize (b);
			if (d < dmin) dmin = d, hp = b;
		}
	}
	VECTOR3 pp;
	oapiGetGlobalPos (hp, &pp);
	VECTOR3 up = unit (gpos - pp);
	double alt = length (gpos - pp) - oapiGetSize (hp);
	ATMPARAM atm{};
	const ATMCONST *ac = oapiGetPlanetAtmConstants (hp);
	if (ac) oapiGetPlanetAtmParams (hp, length (gpos - pp), &atm); // (by radius)
	double rho = ac ? atm.rho : 0.0;
	double drag = ParticleSystem::ExhaustDrag (rho);
	// (the air here as the explosion's effects need it: drag, what burns on,
	// whether a fireball holds together - VACUUM_PLAN.md 3.1)
	Air here = AirAt (hp, gpos);
	double airDrag = here.Drag (), burnOn = here.Burn ();
	if (fire.tex == nullptr) fire.tex = scene->FireballTexture (4), fire.emissive = true; // (ragged flames, coloured by their temperature ramp)
	if (sparks.tex == nullptr) sparks.tex = scene->SparkTexture (), sparks.emissive = true;
	if (smoke.tex == nullptr) smoke.tex = sys->SmokeTexture ();
	if (dust.tex == nullptr) dust.tex = sys->SmokeTexture ();

	// (down on a sea: water, not ground - a splash and steam, no scorch)
	bool onWater = false;
	if (alt < 3.0 * size + 20.0) onWater = vksurf::QueryGlobal (scene, hp, gpos).IsWater ();

	// (born at the sea's surface - a vessel broken up by its impact on the
	// water: the splash is its water state's, below, as each piece hits)

	// the fireball: propellant released as it broke off (if seen then)
	if (!burst && info.fuel > 1.0 && simt - info.created < 0.5) {
		burst = true;
		// (the pieces of one break-up release their propellant together, in
		// one place: one fireball of all of it - the first piece's - not one
		// each. Nine overlapping balls of the shares read as a lumpy pile,
		// and their particles were thinned into fewer, larger ones. Its
		// velocity: theirs, weighted by their propellant - the fuel travels
		// with the pieces, not with the first one)
		static std::vector<std::pair<VDebris*, VECTOR3>> formed; // (where each formed: a step ago)
		static double formedT = -1e30;
		if (simt != formedT) formed.clear (), formedT = simt;
		VECTOR3 p0 = gpos - vel * dt;
		VDebris *own = nullptr;
		// (ORBITER_VK_FIREBALL_NOJOIN=1, testing: a fireball per piece - several at once)
		static const bool noJoin = getenv ("ORBITER_VK_FIREBALL_NOJOIN") && atoi (getenv ("ORBITER_VK_FIREBALL_NOJOIN"));
		// (in vacuum the pieces of one break-up join too: one shell, one
		// flash - five flashes lit the station out white)
		bool later = false; // (joining a ball that formed a step or more ago)
		if (!noJoin)
			for (auto &o : formed) {
				const DEBRISINFO &oi = o.first->info;
				bool same = !strcmp (oi.source, info.source) && std::fabs (oi.created - info.created) < 0.05; // (one break-up)
				if (same || (o.first->fbTf > 0.0 && length (o.second - p0) < std::max (o.first->fbR, FireModel (info.fuel, onWater).R))) { own = o.first; break; }
			}
		// (overlap merging, PLAN.md 2.6: a ball forming within one still
		// growing - under 0.35 t_f old - whose spheres would overlap by more
		// than half (centres closer than half the radii's sum) joins it: its
		// radius grown is eased to the sum's over 0.2 s, not jumped - phase 6)
		// (held together as a fireball, or expanding freely: by the air's
		// pressure against the propellant's mass - Air::Confine; until the
		// two are blended (VACUUM_PLAN V5), the one that dominates)
		bool confined = here.Confine (info.fuel) >= 0.5;
		if (!own && !noJoin && confined) {
			double Rn = FireModel (info.fuel, onWater).R;
			static const bool fbLog3 = getenv ("ORBITER_VK_FIREBALL_LOG") != nullptr;
			for (VDebris *o : LiveFireballs ()) {
				if (o == this || o->fbTf <= 0.0 || simt - o->fbT0 > 1.3 * o->fbTf) continue;
				double d = length (o->FireballPosNow (simt) - gpos); // (both now: p0 is a step ago, a kilometre off in the global frame)
				if (fbLog3) oapiWriteLogV ("VulkanClient: fireball %s forms %.0f m from %s's ball (R %.0f m, %.2f s old): %s", info.name, d, o->info.name, o->fbR, simt - o->fbT0,
					simt - o->fbT0 > 0.35 * o->fbTf ? "too old to join" : d < 0.5 * (o->fbR + Rn) ? "joins it" : "apart");
				if (simt - o->fbT0 > 0.35 * o->fbTf) continue;
				if (d < 0.5 * (o->fbR + Rn)) { own = o, later = true; break; }
			}
		}
		if (own) {
			own->fbFuel += info.fuel;
			own->fbBreak->fuel += info.fuel;
			fbBreak = own->fbBreak;
			FireballModel m = FireModel (own->fbFuel, own->fbWater);
			if (own->fbTf > 0.0) {
				own->fbRGoal = m.R, own->fbTf = m.tf;
				if (!later) own->fbR = own->flashR = m.R;
			} else own->fbR = own->fbRGoal = own->flashR = 2.9 * cbrt (own->fbFuel), own->fbVacVs = std::clamp (3.0 * own->flashR, 20.0, 300.0);
			own->fbVel = (own->fbVel * own->fbFuelW + vel * info.fuel) / (own->fbFuelW + info.fuel);
			own->fbFuelW += info.fuel;
			static const bool fbLog2 = getenv ("ORBITER_VK_FIREBALL_LOG") != nullptr;
			if (later && fbLog2) oapiWriteLogV ("VulkanClient: fireball %s joins %s's ball at +%.2f s (overlap): R %.1f -> %.1f m", info.name, own->info.name, simt - own->fbT0, own->fbR, m.R);
		} else {
			fbFuel = info.fuel, fbWater = onWater;
			fbBreak = std::make_shared<BreakUp> ();
			fbBreak->fuel = info.fuel;
			FireballModel m = FireModel (info.fuel, onWater);
			// (in vacuum all of it flashes: nothing holds it together)
			double R = confined ? m.R : 2.9 * cbrt (info.fuel);
			flashT = info.created, flashR = R, flashK = confined ? 3.0 : 10.0;
			// (fbPos: where it was a step ago - Fireball moves it on to now; the
			// global velocities carry Earth's orbit, a kilometre a step)
			fbT0 = simt, fbR = fbRGoal = R, fbPos = p0, fbPosT = simt - dt, fbVel = vel, fbFuelW = info.fuel, fbFlash = true, fbStage = 0;
			fbRise = fbRisen = 0.0;
			fbG = GGRAV * oapiGetMass (hp) / (oapiGetSize (hp) * oapiGetSize (hp)); // (Mars: 3.7 - its rise as sqrt (g R))
			LiveFireballs ().push_back (this);
			fbFlashPos = p0;
			fbTf = confined ? m.tf : 0.0;
			if (!confined) {
				// (its shell marched as a volume when large enough on
				// screen - CollectFireballs; else sprites)
				fbVacVs = std::clamp (3.0 * R, 20.0, 300.0);
				fbPos = gpos; // (now, not a step ago: the shell moves on from here with fbVel - CollectFireballs)
				fbUp = up, fbPlanet = hp;
				// on an airless body's ground (VACUUM_PLAN.md 3.5): its cloud
				// cut by the ground and turned along it (fireball_grid.comp), the
				// regolith thrown out on ballistic arcs, a blast halo left on it
				{
					double lng, lat, rad;
					oapiGlobalToEqu (hp, gpos, &lng, &lat, &rad);
					vksurf::Point pt = vksurf::Query (scene, hp, lng, lat);
					double h = rad - oapiGetSize (hp) - (pt.IsWater () ? 0.0 : pt.elev);
					double sEnd = CloudOf (fbFuel, R, fbVacVs).SEnd ();
					fbGround = h < 1.2 * sEnd ? -h : -1e9;
					static const bool fbLog4 = getenv ("ORBITER_VK_FIREBALL_LOG") != nullptr;
					if (fbLog4) oapiWriteLogV ("VulkanClient: vacuum burst %.1f m above the ground (the cloud's end %.0f m): %s", h, sEnd,
						h < 0.6 * sEnd && airDrag < 0.01 && !pt.IsWater () ? "on it - sheet, regolith, halo" : h < 1.2 * sEnd ? "near it - sheet" : "clear of it");
					if (fbGround > -1e8 && h < 0.6 * sEnd && airDrag < 0.01 && !pt.IsWater ()) {
						VECTOR3 foot = gpos - up * h;
						if (scene->Tracks ()) scene->Tracks ()->BlastHalo (hp, foot, up, 1.2 * sEnd);
						if (!dust.tex) dust.tex = sys->SmokeTexture ();
						// (the regolith: grains in a fast, thin sheet - Particles:
						// dust thrown in vacuum, drawn as streaks - low over the
						// ground, falling back under the body's gravity)
						VECTOR3 ex = crossp (up, fabs (up.x) < 0.9 ? _V(1, 0, 0) : _V(0, 1, 0));
						ex = unit (ex);
						VECTOR3 ey = crossp (up, ex);
						const int K = 24;
						for (int k = 0; k < K; k++) {
							double a = (k + 0.5) * 2.0 * PI / K, el = (12.0 + 18.0 * ((k * 7) % 5) / 4.0) * RAD;
							VECTOR3 rd = ex * cos (a) + ey * sin (a);
							ParticleSystem::Emission e{};
							e.p0 = e.p1 = foot + up * 0.3, e.vel = GroundVelocity (hp, foot), e.dir = -unit (rd * cos (el) + up * sin (el)), e.dt = dt;
							e.v0 = 0.6 * fbVacVs, e.spread = 0.35 * fbVacVs;
							e.lifetime = 8.0;
							e.size = 0.5, e.growth = 0.0;
							e.alpha = 0.7, e.slowdown = 0.0;
							e.tinted = true, e.cold = true, e.ballistic = true;
							vksurf::DustColourGlobal (scene, hp, foot, e.albedo);
							e.cls = ParticleSystem::Class::Dust;
							Emit (dust, e, (uint32_t)std::clamp (cbrt (fbFuel) * 2.0 * D, 6.0 * D, 60.0 * D), 8.0, true);
						}
					}
				}
				double Rs = 1.3 * CloudOf (fbFuel, R, fbVacVs).SEnd (); // (about its size seen, half-way)
				FireballRenderer *fr = scene->Fireballs ();
				fbVac = fr && fr->WantsVolume (id, Rs, length (gpos - scene->CameraPos ())); // (now, not p0: a step ago, a kilometre off)
				if (!fbVac) VacuumFlash (dt, R, vel);
				static const bool fbLog = getenv ("ORBITER_VK_FIREBALL_LOG") != nullptr;
				if (fbLog) oapiWriteLogV ("VulkanClient: stage vacuum shell: R %.1f m, %.0f m/s, %s", R, fbVacVs, fbVac ? "volume" : "sprites");
			}
			formed.push_back ({ this, p0 });
		}
		static const bool fbLog = getenv ("ORBITER_VK_FIREBALL_LOG") != nullptr;
		if (fbLog) oapiWriteLogV ("VulkanClient: fireball %s fuel %.0f kg (rho %.2g, p %.3g Pa, O2 %.3g Pa: drag %.3f, burn %.2f, confine %.2f; water %d, v %.1f m/s)%s", info.name, info.fuel, rho, here.p, here.O2pp,
			airDrag, burnOn, here.Confine (info.fuel), (int)onWater, length (vel - GroundVelocity (hp, gpos)), own ? " joins the first" : "");
	}
	// (the volume's soot ball rises on to 1.8 t_f: FireballRenderer)
	if (fbTf > 0.0 && simt - fbT0 < (fbVolume || fbVolFade > 0.0 ? 1.85 : 1.35) * fbTf) {
		VECTOR3 air = GroundVelocity (hp, fbPos); // (the air there: turning with the planet)
		bool ignite = fbFlash;
		fbPlanet = hp;
		Fireball (dt, airDrag, up, air, onWater);
		{   // (the ground or the sea below its centre: the volume spreads on it)
			double lng, lat, rad;
			oapiGlobalToEqu (hp, fbPos, &lng, &lat, &rad);
			vksurf::Point pt = vksurf::Query (scene, hp, lng, lat);
			double h = rad - oapiGetSize (hp) - (pt.IsWater () ? 0.0 : pt.elev);
			fbGround = h < 4.0 * fbR ? -h : -1e9;
			if (fbRaise < 0.0) fbRaise = std::clamp (0.7 - h / std::max (fbR, 1.0), 0.0, 0.7);
			static const bool fbLog = getenv ("ORBITER_VK_FIREBALL_LOG") != nullptr;
			double ta = simt - fbT0;
			if (fbLog && floor (ta / 0.5) != floor ((ta - dt) / 0.5))
				oapiWriteLogV ("VulkanClient: fireball at +%.2f s: r %.1f m, centre %.1f m above the surface, rising %.1f m/s", ta, fbRNow, h, dotp (fbVel - air, up));
		}
		// as it ignites (its pieces joined): the fuel spilled where it fell
		// burns on as it lifts off (a fireball formed near the ground or the
		// sea; not one in mid-air)
		if (ignite && !fbFlash && fbTf > 0.0 && scene->Fires () && burnOn > 0.02) { // (it needs the air's oxygen: none on Mars) // (fbTf 0: it joined an older ball at its flash - that one's spill)
			double lng, lat, rad;
			oapiGlobalToEqu (hp, fbPos, &lng, &lat, &rad);
			vksurf::Point pt = vksurf::Query (scene, hp, lng, lat);
			double h = rad - oapiGetSize (hp) - (pt.IsWater () ? 0.0 : pt.elev);
			if (h < std::max (0.5 * fbR, 15.0)) {
				FireballModel m = FireModel (fbFuel, fbWater);
				scene->Fires ()->Add (hp, fbPos - up * h, fbDir, fbV0, fbFuel, m.Mp, fbR, fbT0, 0.5 * fbTf, m.tw, pt.IsWater ());
				// (on the sea: a ring and foam at its foot, by its size - PLAN.md section 3)
				if (pt.IsWater () && scene->Water ())
					scene->Water ()->Impulse (hp, fbPos - up * h, 0.6 * fbR, std::clamp (cbrt (m.Mp) / 12.0, 0.3, 1.0));
			}
		}
	}
	// struck by another body (when it broke off, or since): small bits of
	// it and sparks thrown off (a spray for each blow; a grinding contact
	// sprays again only now and then, or when harder)
	if (frags.tex == nullptr) frags.tex = scene->FragmentTexture ();
	if (info.hitT > hitSeen && info.hit > 12.0 && simt - info.hitT < 0.5) {
		hitSeen = info.hitT;
		if (simt - sprayT > 0.3 || info.hit > 1.5 * sprayV) {
			sprayT = simt, sprayV = info.hit;
			VECTOR3 hp;
			oapiLocalToGlobal (hObj, &info.hitpos, &hp); // (where it is now)
			Spray (hp, vel, info.hit, airDrag);
		}
	}

	// on the ground: a scorch mark where it burned or crashed
	if (!scorched && onWater) scorched = true; // (none on water)
	if (!scorched && alt < 3.0 * size + 2.0 && (info.fuel > 1.0 || info.impact > 8.0) && scene->Tracks ()) {
		scorched = true;
		double R = info.fuel > 1.0 ? std::max (0.8 * FireballRadius (), 2.0 * size) : 1.2 * size;
		scene->Tracks ()->Scorch (hp, gpos - up * (alt + 0.0), up, R, 0.04 * R);
	}

	// under the water (a wreck floods and sinks): nothing burns or smokes
	// there - a burning or hot piece going under: a burst of steam
	WATERSTATE ws;
	bool haveWs = onWater && oapiGetDebrisWaterState (hObj, &ws);
	bool under = onWater && ((haveWs && ws.volume > 0.0 && ws.submerged >= 0.95 * ws.volume) || alt < -(size + 1.0));
	if (under && !sunk && !quenched && (burning || info.temp > 700.0) && rho > 1e-4) {
		quenched = true;
		if (!quench.tex) quench.tex = sys->SmokeTexture ();
		static const bool fbLog = getenv ("ORBITER_VK_FIREBALL_LOG") != nullptr;
		if (fbLog) oapiWriteLogV ("VulkanClient: stage %s under at +%.2f s (%s): quench", info.name, simt - info.created, burning ? "burning" : "hot");
		double s = std::max (size, 1.0);
		ParticleSystem::Emission e{};
		VECTOR3 gv = GroundVelocity (hp, gpos);
		e.p0 = e.p1 = gpos - up * alt, e.vel = gv, e.dir = -up, e.dt = dt; // (at the surface above it)
		e.v0 = 3.0, e.spread = 2.0;
		e.lifetime = 2.0;
		e.size = 0.6 * s * sz, e.growth = 0.8 * s * sz;
		e.alpha = 0.45, e.slowdown = 1.5;
		e.tinted = true, e.cold = false;
		e.albedo[0] = e.albedo[1] = e.albedo[2] = 0.85f; // (steam)
		e.cls = ParticleSystem::Class::Steam;
		Emit (quench, e, (uint32_t)std::clamp (8.0 * s * D, 8.0 * D, 120.0 * D), 2.0, true);
	}
	sunk = under;

	// burning: fire and black smoke trailing from it (by the second, and
	// by the distance it moves through the air: an unbroken trail when fast).
	// Its flames seen for the break-up's t_w (the core's burn, longer, keeps
	// it hot), dying down over the last two seconds of it; then it smoulders
	VECTOR3 groundVel = GroundVelocity (hp, gpos); // (the surface below; the air turns with it)
	VECTOR3 airVel = vel - groundVel;
	double airSpd = rho > 1e-6 ? length (airVel) : 0.0;
	// (a part burned off in reentry, no propellant: the core's burn as it is)
	double tw = fbBreak || info.fuel > 1.0 ? FireModel (fbBreak ? fbBreak->fuel : info.fuel, false).tw : 1e30;
	double flameLeft = std::min (info.burn, tw - (simt - info.created));
	// (flames need the air's oxygen: in vacuum, or in air without it (Mars),
	// the propellant flashes in the burst and is gone; in thin air they are
	// weaker - Air::Burn)
	burning = flameLeft > 0.0 && burnOn > 0.02 && !sunk;
	flameF = burning ? std::min (1.0, flameLeft / 2.0) * burnOn : 0.0;
	bool atRest = alt < 2.0 * size + 3.0 && length (vel - groundVel) < 8.0;
	// (the flames stream aft of a flying piece, up from one at rest: the
	// hull glows on that side - Render)
	flameDir = airSpd > 1.0 ? airVel / airSpd : -up;
	if (burning) {
		double f = flameF;
		double s = std::max (0.6 * size, 0.5);
		// (a burning piece's flames at a density of 4 at most - beyond it,
		// larger particles: a midair break-up's two dozen falling pieces
		// at density 8 reserved more of the pool than it has)
		double Ds = std::min (D, 4.0), szs = std::pow (Ds, -1.0 / 3.0);
		if (!fire.havePrev) fire.prev = gpos - vel * dt;
		// (tongues streaming off it: three times the flames of a flicker)
		fire.accum += std::min (3.0 * (45.0 + airSpd / (0.35 * s)), 600.0) * Ds * f * dt;
		uint32_t n = (uint32_t)fire.accum;
		fire.accum -= n;
		if (n) {
			ParticleSystem::Emission e{};
			e.p0 = fire.prev, e.p1 = gpos, e.vel = vel;
			e.dir = flameDir; // (thrown aft, off its windward side; up when at rest)
			e.v0 = 2.0, e.spread = 1.5;
			// (the flames trail a few times its size behind it, however fast)
			e.lifetime = std::clamp (FLAME_LENGTH * s / std::max (airSpd, 1.0), 0.15, 0.7), e.dt = dt;
			e.size = s * szs, e.growth = 0.8 * s * szs;
			e.alpha = 0.8 * f, e.slowdown = std::max (drag, 0.5);
			e.emissive = true, e.ramp = true, e.erode = true;
			// tongues (phase 5): each flame drawn as a streak along its motion
			// through the air - aft of a flying piece, by its speed, up to
			// about three times its size; up from one at rest (gpuparticle.vert)
			e.streak = (float)std::clamp ((1.0 + 1.0 * fmod (simt * 7.3, 1.0)) * s / std::max (airSpd, 2.0), 0.03, 0.4);
			e.cls = ParticleSystem::Class::Fire;
			Emit (fire, e, n, 0.7);
		}
		fire.prev = gpos, fire.havePrev = true;
	} else fire.havePrev = false;
	// embers (phase 5): glowing bits carried up by the hot air as its flames
	// die down (the last two seconds of them) and for half of t_w after,
	// fewer and fewer
	{
		double ember = 0.0;
		if (burning) ember = 0.25 + 0.75 * (1.0 - flameF);
		else if (burnEndT > -1e29 && !sunk && burnOn > 0.02) {
			double tail = 0.5 * std::min (tw, 30.0);
			ember = std::max (0.0, 1.0 - (simt - burnEndT) / tail);
		}
		if (ember > 0.01) {
			double s = std::max (0.6 * size, 0.5);
			embers.accum += 18.0 * D * ember * std::min (1.0, 0.5 + 0.5 * s) * dt;
			uint32_t n = (uint32_t)embers.accum;
			embers.accum -= n;
			if (n) {
				if (!embers.tex) embers.tex = scene->SparkTexture (), embers.emissive = true;
				ParticleSystem::Emission e{};
				e.p0 = gpos - vel * dt, e.p1 = gpos, e.vel = vel, e.dir = -up, e.dt = dt;
				e.v0 = 2.5, e.spread = 1.5 + 0.5 * s;
				e.lifetime = 1.2 + 0.8 * ember;
				e.size = 0.07 * sz, e.growth = 0.0;
				e.alpha = 1.0, e.slowdown = std::max (drag, 0.6);
				e.emissive = true, e.ramp = true, e.spherical = true, e.speck = true; // (a pixel at least: seen from afar)
				e.tinted = true, e.albedo[0] = 1.0f, e.albedo[1] = 0.55f, e.albedo[2] = 0.2f;
				e.streak = 0.04f;
				e.cls = ParticleSystem::Class::Spark;
				Emit (embers, e, n, 2.0);
			}
		}
	}
	// out of the air, or in air without oxygen: what is left of its
	// propellant vents from where its tanks or lines broke, a white jet
	// freezing at once to droplets and ice - not burning (VACUUM_PLAN V3,
	// the user's choice): for 2 to 5 s by its propellant, from a side of it
	// fixed to the piece (turning with its tumble), each puff coasting on at
	// the jet's speed plus the piece's, thinning out as it expands; dragged
	// in thin air. Where it can burn, it burns instead (its flames above)
	if (info.fuel > 5.0 && burnOn <= 0.02 && !sunk) {
		double tv = std::clamp (1.5 + 0.35 * cbrt (info.fuel), 2.0, 5.0), age = simt - info.created;
		if (age >= 0.0 && age < tv) {
			double k = 1.0 - age / tv; // (its pressure falling)
			double s = std::max (0.5 * size, 0.5);
			vent.accum += 120.0 * D * k * dt;
			uint32_t n = (uint32_t)vent.accum;
			vent.accum -= n;
			if (n) {
				if (!vent.tex) vent.tex = sys->SmokeTexture ();
				// (its side: a direction of its own frame, by its serial)
				uint32_t h = info.serial * 2654435761u;
				double z = ((h & 0xffff) / 65535.0) * 2.0 - 1.0, ph = ((h >> 16) / 65535.0) * 2.0 * PI, sr = sqrt (std::max (1.0 - z * z, 0.0));
				VECTOR3 dl = _V(sr * cos (ph), sr * sin (ph), z);
				VECTOR3 dg = mul (grot, dl); // (its frame to the global axes)
				double vj = 25.0 + 35.0 * k; // [m/s] the jet
				ParticleSystem::Emission e{};
				VECTOR3 at = gpos + dg * (0.4 * size);
				e.p0 = at - vel * dt, e.p1 = at, e.vel = vel, e.dir = -dg, e.dt = dt;
				e.v0 = vj, e.spread = 0.18 * vj;
				e.lifetime = 1.0;
				// (expanding as it goes: a cone - the puffs large and faint, many
				// overlapping; small and denser, the jet read as a spray of specks)
				e.size = std::max (1.6 * s, 1.5) * sz, e.growth = 0.7 * vj * sz;
				e.alpha = 0.2 * (0.4 + 0.6 * k), e.slowdown = 2.0 * std::min (airDrag, 1.0);
				e.tinted = true, e.cold = true;
				e.albedo[0] = 0.92f, e.albedo[1] = 0.93f, e.albedo[2] = 0.95f;
				e.cls = ParticleSystem::Class::Spray;
				Emit (vent, e, n, 1.0);
			}
		}
	}
	// its smoke: black while it burns (fuel burning on the water smokes
	// black too: steam only from its splash and its quench); a hot piece
	// flying on, its flames out: a thin grey trail, fading as it cools
	// (at rest - afloat, on the ground - its column takes its smoke:
	// WreckSmoke; the trail's puffs drawn as sprites there were black discs)
	double hot = !burning && !sunk && !atRest && burnOn > 0.02 ? std::clamp ((info.temp - 700.0) / 500.0, 0.0, 1.0) : 0.0;
	if ((burning && !atRest) || hot > 0.0) {
		// (by the distance it moves through the air, D puffs every 0.4 of
		// its size - overlapping many times, an unbroken trail however fast
		// it falls; 20 D a second at least while it is slow. Not by a rate
		// capped per second: falling pieces left puffs metres apart, beads -
		// f6 midair. Phase 5: the spacing is the same whatever is left of
		// its flames (the count was scaled by it: beads as they died down);
		// the puffs larger and fainter, so that the band reads as a band,
		// not as a row of discs)
		double s = std::max (0.6 * size, 0.5);
		if (!smoke.havePrev) smoke.prev = gpos - vel * dt;
		double puff = 2.5 * s * sz;
		smoke.accum += (airSpd * dt / (0.4 * s) + 12.0 * dt) * D;
		uint32_t m = (uint32_t)std::min (smoke.accum, 400.0);
		smoke.accum -= m;
		if (m) {
			ParticleSystem::Emission e{};
			e.p0 = smoke.prev, e.p1 = gpos, e.vel = vel, e.dir = -up;
			// (large, overlapping, little spread: a band widening as it
			// ages; small eroded puffs scattered into dots - f6 midair)
			e.v0 = 0.5, e.spread = 0.4;
			e.lifetime = burning ? 8.0 : 4.0, e.dt = dt;
			e.size = burning ? puff : 0.6 * puff, e.growth = 1.5 * s * sz;
			e.alpha = burning ? 0.35 * std::max (flameF, 0.3) : 0.12 * hot, e.slowdown = std::max (drag, 0.3);
			e.tinted = true;
			// (a burning piece's trail into the smoke volume (phase 5): as
			// sprites it was a band of round discs, polka dots, however many
			// overlapped (the sum of round things); with D puffs per 0.4 of
			// its size the volume draws an unbroken band at 300 m/s too (the
			// beads of f6 midair were from a rate capped per second). On the
			// water a slow piece's smoke joins the wreck's column and the
			// spill's stem there. A hot piece's faint grey trail stays sprites)
			e.volumetric = burning;
			e.albedo[0] = e.albedo[1] = e.albedo[2] = burning ? 0.08f : 0.35f; // (soot; a hot piece's: grey)
			// (a trail of smoke: keeps its coverage as it ages, thins by
			// opacity - the band erosion; eroded as puffs it broke up)
			e.cls = ParticleSystem::Class::WreckSmoke;
			Emit (smoke, e, m, 8.0);
		}
		smoke.prev = gpos, smoke.havePrev = true;
	} else smoke.havePrev = false;
	// a burning wreck at rest on the ground (or afloat): its smoke rises in
	// a column and drifts off with the wind, for as long as it burns and
	// as it smoulders on for a minute or so (not once it sank)
	if (burning) burnEndT = simt;
	double colStr = burning ? std::clamp (flameF, 0.5, 1.0) : (burnEndT > -1e29 ? exp (-(simt - burnEndT) / 25.0) : 0.0);
	if (atRest && !sunk && colStr > 0.08 && burnOn > 0.02) WreckSmoke (dt, colStr, up, groundVel, onWater);

	// sliding on the ground: sparks (and a gouge); hitting it: dust
	VECTOR3 contact = gpos - up * std::min (alt, 0.7 * size);
	if (info.scrape > 3.0) {
		double v = info.scrape;
		sparks.accum += std::min (40.0 * v, 600.0) * D * dt;
		uint32_t n = (uint32_t)sparks.accum;
		sparks.accum -= n;
		if (n) {
			ParticleSystem::Emission e{};
			e.p0 = e.p1 = contact, e.vel = vel, e.dir = -up;
			e.v0 = 0.25 * v, e.spread = 0.35 * v;
			e.lifetime = 0.5, e.dt = dt;
			e.size = 0.06 * sz, e.growth = 0.0;
			e.alpha = 1.0, e.slowdown = 0.5;
			e.emissive = true, e.ballistic = true;
			e.cls = ParticleSystem::Class::Spark;
			Emit (sparks, e, n, 0.5);
		}
		if (GroundTracks *gt = scene->Tracks ()) {
			if (onWater) gt->NoContact (hObj, 0); // (no gouge on water)
			else gt->Contact (hObj, 0, hp, contact, up, 0.25 * size, 0.08);
		}
	} else if (GroundTracks *gt = scene->Tracks ()) gt->NoContact (hObj, 0);
	// (the dust thrown up keeps a little of its speed over the ground: with
	// all of it, it would streak along its path)
	VECTOR3 dustVel = groundVel + (vel - groundVel) * 0.25;
	// (a puff for each blow: one bouncing along at speed hits the ground
	// step after step, a puff each step would be a solid wall of dust)
	bool blow = info.impact > 1.5 && (simt - dustT > 0.3 || info.impact > 1.5 * dustImpact);
	// in the water: its splash, spray and wake, as the water's physics says
	// (a blow on the ground under the water, the seabed: none)
	if (!(info.flags & DEBRIS_ATREST)) {
		if (haveWs || oapiGetDebrisWaterState (hObj, &ws)) water.Update (scene, hp, ws, gpos, vel, size, camDist, 0x8000000000ull | info.serial);
	}
	if (blow && onWater) dustT = simt, dustImpact = info.impact;
	else if (blow && airDrag > 0.01) { // (in air: dust that hangs in it; in vacuum the blow's grains fly on ballistically - Particles: dust thrown in vacuum)
		dustT = simt, dustImpact = info.impact;
		uint32_t n = (uint32_t)std::min (info.impact * 6.0 * D, 400.0);
		ParticleSystem::Emission e{};
		e.p0 = e.p1 = contact, e.vel = dustVel, e.dir = -up;
		// (thrown out at a few metres per second, however hard the blow:
		// the ground gives way, it is not hit like a ball)
		e.v0 = std::min (0.3 * info.impact, 6.0), e.spread = std::min (0.6 * info.impact, 12.0);
		e.lifetime = 6.0, e.dt = dt;
		e.size = 0.8 * size * sz, e.growth = 0.5 * size * sz;
		e.alpha = 0.4, e.slowdown = 1.5 * std::min (airDrag, 1.0);
		e.tinted = true, e.cold = true, e.ballistic = false, e.planar = true, e.volumetric = true;
		vksurf::DustColourGlobal (scene, hp, contact, e.albedo);
		Emit (dust, e, n, 6.0, true); // (one-shot bursts: the ring holds a couple, not a rate)
	} else if (info.scrape > 5.0 && airDrag > 0.01) {
		// sliding fast: a thin trail of dust kicked up behind it
		if (!dust.havePrev) dust.prev = contact - vel * dt;
		dust.accum += std::min (0.4 * info.scrape, 20.0) * D * dt;
		uint32_t n = (uint32_t)dust.accum;
		dust.accum -= n;
		if (n) {
			ParticleSystem::Emission e{};
			e.p0 = dust.prev, e.p1 = contact, e.vel = dustVel, e.dir = -up;
			e.v0 = 0.05 * info.scrape, e.spread = 0.1 * info.scrape;
			e.lifetime = 2.5, e.dt = dt;
			e.size = 0.25 * size * sz, e.growth = 0.3 * size * sz;
			e.alpha = 0.12, e.slowdown = 2.0 * std::min (airDrag, 1.0);
			e.tinted = true, e.cold = true, e.planar = true;
			vksurf::DustColourGlobal (scene, hp, contact, e.albedo);
			Emit (dust, e, n, 6.0);
		}
		dust.prev = contact, dust.havePrev = true;
	}
	if (info.scrape <= 5.0 || blow) dust.havePrev = false;
}

namespace {
	// a puff of the long-lived tier (carried by the wind, fading in over 25 s:
	// particle_common.glsl): the trail encoding in one place (the record's
	// trail flag; before it, slowdown < 0 marked it)
	void LongLived (ParticleSystem::Emission &e, double slowdown)
	{
		e.slowdown = std::abs (slowdown);
		e.trail = true;
	}
}

void VDebris::Fireball (double dt, double airDrag, const VECTOR3 &up, const VECTOR3 &air, bool onWater)
{
	// the fire of the propellant released as it broke off, in air (the
	// stages by its flames' time t_f, FireModel - about 2 s for 7 t):
	// - a white flash as it ignites (its first step, the pieces joined)
	// - grown to its size by 0.25 t_f, drawn out along its path at first
	//   (the fuel thrown on with the wreck), braked gently, its brightest
	// - lifting off from 0.35 t_f (buoyant, some 0.5 sqrt(g R)); its soot
	//   cap from 0.4 t_f, rising on for most of a minute
	// - burned out by t_f: its last flames gone by 1.3 t_f (the spill burns
	//   on where it fell: CrashFires)
	// The flames: billows of mixed sizes out to its rim, cooling to orange
	// and dark red as they age and break up into tongues (the flames'
	// textures by temperature: Scene::FireballTexture); its core: small,
	// bright, short-lived flames near the centre while it burns hard
	// (marched as a volume, FireballRenderer, when large enough on screen:
	// its flames and flash are the volume's, not sprites; the soot cap
	// grows inside it as it burns out and takes over from it)
	ParticleSystem *sys = scene->Particles ();
	double D = sys->density, sz = std::pow (D, -1.0 / 3.0);
	double t = oapiGetSimTime () - fbT0, tf = std::max (fbTf, 1e-3), u = t / tf;
	double R = fbR;
	static const bool fbLog = getenv ("ORBITER_VK_FIREBALL_LOG") != nullptr;
	FireballRenderer *fr = scene->Fireballs ();
	// (its distance from where it is now: fbPos is a step behind here, and
	// the global frame moves 30 km/s with the Earth - a kilometre a step
	// at 1x, ten at 10x: at 10x warp the ball went to sprites - phase 6)
	bool vol = fr && fr->WantsVolume (id, R, length (FireballPosNow (oapiGetSimTime ()) - scene->CameraPos ()));
	if (vol != fbVolume && fbLog) oapiWriteLogV ("VulkanClient: stage fireball at +%.2f s: %s", t, vol ? "volume" : "sprites");
	fbVolume = vol;
	// (the switch cross-faded over 0.35 s: the volume's weight, the sprites
	// at the rest - the sprites build up over their lifetimes, about as
	// long; at its birth no fade - phase 6)
	if (fbVolFade < 0.0) fbVolFade = vol ? 1.0 : 0.0;
	else fbVolFade = std::clamp (fbVolFade + (vol ? 1.0 : -1.0) * dt / 0.35, 0.0, 1.0);
	double spriteW = 1.0 - fbVolFade; // (the sprites' share)
	// (a ball that joined it after it formed: its radius grown eased up)
	if (fbRGoal > fbR) fbR += (fbRGoal - fbR) * (1.0 - exp (-dt / 0.2)), R = fbR;
	fbUp = up;
	// (the flash on its second step - the pieces' shares joined it by then)
	bool flash = fbFlash && t > 0.0;
	if (flash) {
		// overlap merging (PLAN.md 2.6), the whole ball now known: one
		// forming inside an older ball still growing (under 0.35 t_f old),
		// the centres closer than half the sum of the radii, gives it its
		// propellant and is no more (no flash of its own; the older ball's
		// R grown eased up to the sum's) - phase 6
		static const bool noJoin = getenv ("ORBITER_VK_FIREBALL_NOJOIN") && atoi (getenv ("ORBITER_VK_FIREBALL_NOJOIN"));
		double simt = oapiGetSimTime ();
		for (VDebris *o : noJoin ? std::vector<VDebris*> () : LiveFireballs ()) {
			if (o == this || o->fbTf <= 0.0 || o->fbT0 >= fbT0 || simt - o->fbT0 > 0.35 * o->fbTf) continue;
			double d = length (o->FireballPosNow (simt) - FireballPosNow (simt));
			if (d >= 0.5 * (o->fbRGoal + fbR)) continue;
			o->fbFuel += fbFuel;
			if (o->fbBreak && fbBreak && o->fbBreak != fbBreak) o->fbBreak->fuel += fbBreak->fuel;
			FireballModel m = FireModel (o->fbFuel, o->fbWater);
			o->fbRGoal = m.R, o->fbTf = m.tf;
			o->fbVel = (o->fbVel * o->fbFuelW + fbVel * fbFuelW) / (o->fbFuelW + fbFuelW);
			o->fbFuelW += fbFuelW;
			if (fbLog) oapiWriteLogV ("VulkanClient: fireball %s (M %.0f kg) %.0f m from %s's ball (R %.1f m, %.2f s old) joins it (overlap): R -> %.1f m", info.name, fbFuel, d, o->info.name, o->fbRGoal, simt - o->fbT0, m.R);
			fbTf = 0.0, fbFlash = false, flashR = 0.0, fbR = fbRNow = 0.0;
			auto &live = LiveFireballs ();
			live.erase (std::remove (live.begin (), live.end (), this), live.end ());
			return;
		}
		fbFlash = false;
		VECTOR3 rel = fbVel - air;
		fbV0 = length (rel);
		fbDir = fbV0 > 1.0 ? rel / fbV0 : up;
		if (fbLog) {
			FireballModel m = FireModel (fbFuel, fbWater);
			oapiWriteLogV ("VulkanClient: fireball model %s: M %.0f kg, f_p %.2f, M_p %.0f kg: R %.1f m, t_f %.2f s, t_w %.1f s, soot %.0f s; v %.1f m/s (pieces' mean)",
				info.name, fbFuel, m.fp, m.Mp, m.R, m.tf, m.tw, m.soot, fbV0);
			oapiWriteLogV ("VulkanClient: stage fireball at +%.2f s: flash", t);
		}
	}
	if (fbLog) { // (its stages as they pass)
		static const double at[] = { 0.25, 0.35, 0.4, 1.0, 1.3 };
		static const char *what[] = { "full size", "lift-off", "soot cap", "burn-out (no new flames)", "flames gone" };
		while (fbStage < 5 && u >= at[fbStage]) { oapiWriteLogV ("VulkanClient: stage fireball at +%.2f s (u %.2f): %s", t, u, what[fbStage]); fbStage++; }
	}
	// its centre: the air's velocity, its momentum and its buoyant rise.
	// Its momentum (the pieces' thrown on) braked by the air, smoothly; its
	// rise growing from the first (the hot gas buoyant at once, held at
	// first by its spreading on the surface) to some 0.7 sqrt(g R) about
	// its burn-out, slowing after as it draws air in and cools - the cap
	// it hands over to rises on as it did (FireballRenderer::Rise). (Its
	// rise was relaxed to a fixed speed from 0.35 t_f: the momentum braked
	// to a stop first, the ball hung there, then was pushed up - a stall
	// between its burst and its lift-off)
	// (by the air's drag, Air::Drag: 1 at sea level; in thin air (Mars, high
	// up) it carries on and spreads further; its buoyancy (rise) wherever
	// the air holds it together, a little air enough)
	double brake = FIREBALL_BRAKE * airDrag;
	double slow = FIREBALL_SLOWDOWN * airDrag; // (its flames')
	double buoyant = std::min (airDrag / 0.05, 1.0);
	double lift = FireballRenderer::Lift (u);
	{
		VECTOR3 mom = (fbVel - air - up * fbRise) * exp (-brake * dt);
		fbRise = FireballRenderer::Rise (u, R, fbG) * buoyant;
		fbVel = air + mom + up * fbRise;
	}
	fbPos += fbVel * dt;
	fbRisen += fbRise * dt;
	fbPosT = oapiGetSimTime ();
	fbFlashPos += air * dt;
	if (flash) {
		// its foot: the surface below it (known from its first step), where
		// its flash is, its skirt pushed out, the sea boils; and the thud of
		// it: the camera near it jolted when the sound arrives (Scene::Jolt).
		// (After the move: fbPos was a step old, a kilometre off in the
		// global frame, which moves with the planet)
		fbFootKnown = fbGround > -1e8;
		fbFoot = fbFootKnown ? fbPos + up * fbGround : fbFlashPos;
		FireballModel m = FireModel (fbFuel, fbWater);
		if (fbPlanet) scene->Jolt (fbPlanet, fbFoot, cbrt (m.Mp), R);
	} else fbFoot += air * dt;
	// its size (FireballRenderer::Radius): its burst slowed by the air, and
	// growing on as it rises (drawing air in) - the envelope never shrinks
	double r = FireballRenderer::Radius (u, R, fbRisen);
	fbRNow = r;
	// drawn out along its path while the fuel is thrown on with the wreck:
	// up to (1 + 0.3 v / 20 m/s) times as long; its length along the path
	// never contracts (fbAlong): it grows across to it as it burns
	// (emitted along a line through its centre: ext, half the extra length)
	double el = std::min (0.3 * fbV0 / 20.0, 1.0) * std::max (0.0, 1.0 - u / 0.5);
	fbAlong = std::max (fbAlong, r * (1.0 + el));
	el = std::max (fbAlong / r - 1.0, 0.0);
	VECTOR3 ext = fbDir * (r * el);
	// its lean: the top of it carried on ahead of its foot by what is left
	// of its momentum (the foot held back by the surface), a quarter of its
	// height ahead at 20 m/s
	{
		VECTOR3 hv = (fbVel - air) - up * dotp (fbVel - air, up);
		double lh = length (hv);
		fbLean = lh > 1e-3 ? hv * (std::min (0.012 * lh, 0.35) / lh * std::min (1.0, 2.0 * lift + 0.3)) : _V(0, 0, 0); // (a third at most: at 60 m/s the ball tipped over)
	}
	// (the volume: its shape now; the vortex ring's turn so far, at about
	// 1.4 sqrt (g / R) once lifted (its surface rolling at about its rise
	// speed): the integral of its lift-off over u)
	fbEl = 1.0 + el, fbLift = lift;
	fbRoll = FireballRenderer::RingTurn (u, tf, R, fbG);
	// (a line source: p0 a step back along its velocity - Emit's convention,
	// the particles at the end of the step spread evenly from -ext to +ext)
	VECTOR3 e0 = fbPos - ext - fbVel * dt, e1 = fbPos + ext;
	if (!fbFire.tex) fbFire.tex = scene->FireballTexture (4), fbFire.emissive = true;
	// the ignition flash (PLAN.md section 3): white, two frames, at its
	// foot - the impact point - under the ball (the volume's own first
	// frames are white-hot too: fireball.frag)
	if (flash) {
		if (!fbFlashR.tex) fbFlashR.tex = scene->FireballTexture (5), fbFlashR.emissive = true; // (a smooth white glow)
		ParticleSystem::Emission e{};
		VECTOR3 at = fbFoot + up * (0.12 * R);
		e.p0 = e.p1 = at, e.vel = air, e.dir = -up, e.dt = dt;
		e.v0 = 0.0, e.spread = 0.1 * R;
		e.lifetime = 0.07;
		e.size = 0.6 * R, e.growth = 0.0;
		e.alpha = 3.0, e.slowdown = 0.0;
		e.emissive = true, e.tinted = true, e.albedo[0] = 1.0f, e.albedo[1] = 0.98f, e.albedo[2] = 0.93f;
		e.cls = ParticleSystem::Class::Fire;
		Emit (fbFlashR, e, 3, 0.3, true);
	}
	// (the sprites' ball: a few large white-hot sprites over it, gone within a tenth of a second)
	if (flash && !vol) {
		ParticleSystem::Emission e{};
		e.p0 = fbPos - fbVel * dt, e.p1 = fbPos, e.vel = fbVel, e.dir = -up, e.dt = dt;
		e.v0 = 0.0, e.spread = 0.8 * R;
		e.lifetime = 0.1;
		e.size = 0.45 * R, e.growth = 0.4 * R / e.lifetime;
		e.alpha = 4.0, e.slowdown = slow;
		e.emissive = true, e.tinted = true, e.albedo[0] = 1.0f, e.albedo[1] = 0.97f, e.albedo[2] = 0.9f;
		e.cls = ParticleSystem::Class::Fire;
		Emit (fbFlashR, e, 5, 0.3, true);
	}
	// its skirt (PLAN.md section 3): the air its front pushes out sweeps the
	// surface it formed on - over the sea a low white skirt of spray and
	// mist thrown outward, over the ground a ring of dust in the ground's
	// colour - a ring at its foot moving out with it for half a second
	if (fbFootKnown && t < 0.5 && fbPlanet) {
		double rate = fbWater ? 900.0 : 500.0;
		fbSkirt.accum += rate * D * (1.0 - t / 0.5) * dt;
		uint32_t n = (uint32_t)fbSkirt.accum;
		fbSkirt.accum -= n;
		if (n) {
			if (!fbSkirt.tex) fbSkirt.tex = sys->SmokeTexture ();
			ParticleSystem::Emission e{};
			e.vel = air, e.dt = dt;
			e.v0 = fbWater ? 18.0 : 12.0, e.spread = 4.0;
			e.lifetime = fbWater ? 1.4 : 2.5;
			e.size = (fbWater ? 0.06 : 0.1) * R * sz, e.growth = 1.5 * e.size / e.lifetime;
			e.alpha = fbWater ? 0.4 : 0.35, e.slowdown = fbWater ? 0.8 : 1.2;
			e.tinted = true, e.cold = true, e.ballistic = fbWater;
			if (fbWater) e.albedo[0] = 0.9f, e.albedo[1] = 0.93f, e.albedo[2] = 0.96f, e.cls = ParticleSystem::Class::Spray;
			else vksurf::DustColourGlobal (scene, fbPlanet, fbFoot, e.albedo), e.cls = ParticleSystem::Class::Dust;
			VECTOR3 ex = length (fbDir) > 0.5 ? fbDir - up * dotp (fbDir, up) : crossp (up, _V(1, 0, 0));
			if (length (ex) < 0.3) ex = crossp (up, _V(0, 1, 0));
			ex = unit (ex);
			VECTOR3 ey = crossp (up, ex);
			double rs = 1.25 * r + 15.0 * t; // (outside its foot: its billows reach 1.2 r, and it sits 0.7 r up)
			const int K = 8;
			double a0 = (fbSkirt.accum > 0.5 ? 0.5 : 0.0) * 2.0 * PI / K;
			for (int k = 0; k < K && n; k++) {
				uint32_t nk = (n + (K - 1 - k)) / (K - k);
				n -= nk;
				double a = a0 + k * 2.0 * PI / K, b = a + 2.0 * PI / K;
				VECTOR3 ra = ex * cos (a) + ey * sin (a), rb = ex * cos (b) + ey * sin (b), rm = unit (ra + rb);
				e.p0 = fbFoot + ra * rs, e.p1 = fbFoot + rb * rs;
				e.dir = -unit (rm * 0.9 + up * 0.35);
				Emit (fbSkirt, e, nk, e.lifetime + 0.5);
			}
		}
	}
	// the sea boiling white under it while it burns (steam from its foot)
	if (fbFootKnown && fbWater && u < 1.0) {
		double k = 1.0 - 0.7 * std::clamp ((u - 0.6) / 0.4, 0.0, 1.0);
		fbSteam.accum += 40.0 * D * k * dt;
		uint32_t n = (uint32_t)fbSteam.accum;
		fbSteam.accum -= n;
		if (n) {
			if (!fbSteam.tex) fbSteam.tex = sys->SmokeTexture ();
			VECTOR3 ex = length (fbDir) > 0.5 ? fbDir - up * dotp (fbDir, up) : crossp (up, _V(1, 0, 0));
			if (length (ex) < 0.3) ex = crossp (up, _V(0, 1, 0));
			ex = unit (ex);
			VECTOR3 ey = crossp (up, ex);
			ParticleSystem::Emission e{};
			e.vel = air, e.dir = -up, e.dt = dt;
			e.v0 = 4.0, e.spread = 3.0;
			e.lifetime = 1.6;
			e.size = 0.12 * R * sz, e.growth = 0.1 * R * sz;
			e.alpha = 0.35, e.slowdown = 1.2;
			e.tinted = true;
			e.albedo[0] = e.albedo[1] = e.albedo[2] = 0.85f; // (steam)
			e.cls = ParticleSystem::Class::Steam;
			// (chords across its foot, along its path and across it)
			double w = 1.0 * r;
			uint32_t n1 = (n + 1) / 2;
			e.p0 = fbFoot - ex * w, e.p1 = fbFoot + ex * w;
			Emit (fbSteam, e, n1, 2.0);
			if (n > n1) {
				e.p0 = fbFoot - ey * w, e.p1 = fbFoot + ey * w;
				Emit (fbSteam, e, n - n1, 2.0);
			}
		}
	}
	if (u < 1.0 && spriteW > 0.001) {
		// (dim each and tinted towards orange: they overlap by the dozen.
		// Brighter while it burns hard (the first half), dimmer as it dies)
		// (particles by its size; many smaller ones: a few hundred large
		// ones read as a pile of balls)
		double total = std::clamp (5.5 * R * D, 10.0 * D, 160.0 * D);
		double hard = std::max (1.0 - 1.6 * u, 0.0);
		fbFire.accum += total / tf * dt;
		if (flash) fbFire.accum += 0.25 * total;
		uint32_t n = (uint32_t)fbFire.accum;
		fbFire.accum -= n;
		if (n) {
			ParticleSystem::Emission e{};
			e.p0 = e0, e.p1 = e1, e.vel = fbVel, e.dir = -up, e.dt = dt;
			e.v0 = 0.1 * R, e.spread = (1.2 + 1.2 * u) * r * std::max (slow, 1.0);
			// (its flames die with it: those born late live only to about
			// 1.3 of its time - no orange ball lingering after the fire)
			e.lifetime = std::min (std::max (std::min (0.25 + 0.15 * u, 1.3 - u) * tf, 0.25), (1.3 - u) * tf);
			// (cooling as it burns out: its late flames born darker, a dull red
			// seen through its soot by t_f, not an orange ball to the end)
			double heat = std::clamp (1.0 - (u - 0.3) / 0.9, 0.3, 1.0);
			e.alpha = (0.3 + 0.3 * hard) * (0.5 + 0.5 * heat) * spriteW, e.slowdown = slow;
			e.emissive = true, e.ramp = true, e.erode = true;
			e.tinted = true, e.albedo[0] = (float)sqrt (heat), e.albedo[1] = (float)(0.58 * heat), e.albedo[2] = (float)(0.12 * heat * heat);
			e.cls = ParticleSystem::Class::Fire;
			// (billows of mixed sizes: one size read as a pile of balls)
			uint32_t nb = (n + 1) / 2;
			e.size = (0.3 + 0.12 * u) * r * sz, e.growth = 0.5 * r / e.lifetime * sz;
			Emit (fbFire, e, nb, e.lifetime);
			if (n > nb) {
				e.size = (0.6 + 0.2 * u) * r * sz, e.growth = 0.8 * r / e.lifetime * sz;
				e.alpha *= 0.8;
				Emit (fbFire, e, n - nb, e.lifetime);
			}
		}
		// its core: while it burns hard, small bright flames near its centre
		if (hard > 0.0) {
			fbCore.accum += 0.6 * total / tf * hard * dt;
			if (flash) fbCore.accum += 0.15 * total;
			uint32_t m = (uint32_t)fbCore.accum;
			fbCore.accum -= m;
			if (m) {
				if (!fbCore.tex) fbCore.tex = scene->FireballTexture (4), fbCore.emissive = true;
				ParticleSystem::Emission e{};
				e.p0 = e0, e.p1 = e1, e.vel = fbVel, e.dir = -up, e.dt = dt;
				e.v0 = 0.05 * R, e.spread = 0.5 * r * std::max (slow, 1.0);
				e.lifetime = std::max (0.18 * tf, 0.25);
				e.size = 0.25 * r * sz, e.growth = 0.4 * r / e.lifetime * sz;
				e.alpha = 1.0 * hard * spriteW, e.slowdown = slow;
				e.emissive = true, e.ramp = true, e.erode = true;
				e.tinted = true, e.albedo[0] = 1.0f, e.albedo[1] = 0.8f, e.albedo[2] = 0.45f; // (hotter: towards yellow-white)
				e.cls = ParticleSystem::Class::Fire;
				Emit (fbCore, e, m, e.lifetime);
			}
		}
	}
	// its soot cap: from its upper half from 0.4 t_f, as it lifts off and
	// burns out, rising on with it for most of a minute and spreading (the
	// stem below it: the spill's, CrashFires)
	// (the volume: from inside it from 0.9 t_f, hidden in it while it is
	// opaque; as it burns out and fades the cap is there, the volume's soot
	// passing into the smoke volume's. Not from 0.55 t_f: the cap's
	// particles go on as the ball did when they left it, and those of its
	// lift-off lagged it by the hand-over - the cap seen below the ball it
	// replaced)
	double uStart = vol ? 0.9 : 0.4, uEnd = vol ? 1.8 : 1.3;
	if (u > uStart && u < uEnd) {
		if (!fbSmoke.tex) fbSmoke.tex = sys->SmokeTexture ();
		FireballModel fm = FireModel (fbFuel, fbWater);
		double total = std::clamp ((vol ? 6.0 : 4.0) * R * D, 8.0 * D, (vol ? 150.0 : 100.0) * D); // (the volume's: filling its soot ball)
		double w = std::clamp ((u - uStart) / 0.25, 0.0, 1.0);
		double k = w * w * (3.0 - 2.0 * w) * std::min (1.0, (uEnd - u) * 3.0);
		fbSmoke.accum += total / ((uEnd - uStart) * tf) * k * dt;
		uint32_t n = (uint32_t)fbSmoke.accum;
		fbSmoke.accum -= n;
		if (n) {
			ParticleSystem::Emission m{};
			// (its top: wrapping the fire, not hidden in it; the volume's: inside
			// it, about its centre as drawn - raised above where it formed)
			VECTOR3 top = up * (vol ? std::max (fbRaise, 0.0) * r : 0.6 * r);
			// (its velocity the ball's - its rise and what is left of its
			// momentum - braked at the rate the ball's rise slows by then,
			// FireballRenderer::Rise: the cap rises on as the ball did, slowing
			// smoothly. Not hot: the particles' own buoyant rise, on top of a
			// fixed brake of 0.5/s, stopped it rising within seconds of taking
			// over from a ball rising at 11 m/s; and spreading as the ball
			// grew, by the air drawn in: Radius)
			m.p0 = fbPos + top - fbVel * dt, m.p1 = fbPos + top, m.vel = fbVel, m.dir = -up, m.dt = dt;
			m.cold = true;
			double capBrake = 0.5 / ((FireballRenderer::RISE_U + u) * tf);
			// (large, little spread: one cap, not a scatter of puffs)
			m.v0 = std::max (0.05 * R, FireballRenderer::ENTRAIN * fbRise), m.spread = (vol ? 0.6 : 0.3) * r;
			m.lifetime = std::clamp (0.8 * fm.soot, 12.0, 90.0);
			// (the volume's: as large as its soot ball, to take it over - its
			// billows reach about 1.2 r: the puffs' 0.85 of their half size
			// plus their spread; at 1.15 r and 0.5 r the cap it faded into
			// was two thirds of its width)
			m.size = (vol ? 1.7 : 0.8) * r * sz, m.growth = (vol ? 2.5 : 2.0) * R / m.lifetime * sz;
			m.alpha = 0.7, m.slowdown = capBrake * buoyant;
			m.tinted = true, m.volumetric = true;
			m.albedo[0] = m.albedo[1] = m.albedo[2] = vol ? 0.025f : 0.07f; // (soot: charcoal, lit on top; on the sea too. The volume's: as dark as its soot ball as drawn - at 0.08 the smoke volume's cap took over a pale grey, 85 to the ball's 51 in sRGB; 0.045: 65)
			m.cls = ParticleSystem::Class::FireballSmoke;
			if (!vol) Emit (fbSmoke, m, n, m.lifetime);
			else {
				// the volume's cap shaped as the vortex ring rolls it: a third
				// of it from its top, rising; the rest from its rim, going out
				// and over (the cap wider than the ball, flat beneath: a
				// mushroom cap over the stem of the spill's smoke)
				uint32_t nt = (n + 2) / 3;
				Emit (fbSmoke, m, nt, m.lifetime);
				n -= nt;
				VECTOR3 ex = length (fbDir) > 0.5 ? unit (fbDir - up * dotp (fbDir, up)) : crossp (up, _V(1, 0, 0));
				if (length (ex) < 0.5) ex = crossp (up, _V(0, 1, 0));
				VECTOR3 ey = crossp (up, ex);
				double ring = 0.65 * r, vo = 0.15 * sqrt (9.81 * R) * lift; // (braked as gently as the cap: 0.25 carried its rim out past three radii)
				const int K = 6;
				for (int k = 0; k < K && n; k++) {
					uint32_t nk = (n + (K - 1 - k)) / (K - k);
					n -= nk;
					double a = (k + 0.5 * (fbSmoke.accum > 0.5 ? 1.0 : 0.0)) * 2.0 * PI / K;
					VECTOR3 rad = ex * cos (a) + ey * sin (a);
					ParticleSystem::Emission e (m);
					VECTOR3 at = fbPos + up * (std::max (fbRaise, 0.0) * r) + rad * ring;
					e.p0 = at - fbVel * dt, e.p1 = at;
					e.dir = -unit (rad * 0.8 + up * 0.35);
					e.v0 = vo, e.spread = 0.3 * r;
					e.size = 1.2 * r * sz;
					Emit (fbSmoke, e, nk, e.lifetime);
				}
			}
		}
	}
}

void VDebris::VacuumFlash (double dt, double R, const VECTOR3 &vel)
{
	// out of the air, small on screen (or the Sprites setting): the free
	// cloud as sprites (VACUUM_PLAN.md 3.3) - nothing holds the propellant
	// together, lifts it or slows it: a white-hot core gone within a tenth
	// of a second, and white streaks of droplets and ice radiating out at
	// the cloud's speeds (three layers: slow and dense inside, fast and thin
	// outside - self-similar, each one's size growing with its distance),
	// coasting, faded out as the cloud thins. (The grey vapour shell before
	// it read as smoke - a cloud lit by the flash, cumulus - f6 iss_t3.1)
	if (!fbFire.tex) fbFire.tex = scene->FireballTexture (4), fbFire.emissive = true;
	if (!fbSmoke.tex) fbSmoke.tex = scene->Particles ()->SmokeTexture ();
	double D = scene->Particles ()->density, sz = std::pow (D, -1.0 / 3.0);
	FreeCloud c = CloudOf (fbFuel, R, fbVacVs);
	double v = fbVacVs, tEnd = std::max (c.TEnd (), 0.2);
	const int NDIR = 48;
	for (int i = 0; i < NDIR; i++) {
		double z = 1.0 - (i + 0.5) * 2.0 / NDIR, rr = sqrt (std::max (1.0 - z * z, 0.0)), ph = i * 2.39996323;
		VECTOR3 d = _V(rr * cos (ph), z, rr * sin (ph));
		ParticleSystem::Emission e{};
		e.p0 = e.p1 = gpos, e.vel = vel, e.dir = -d, e.dt = dt;
		e.v0 = 0.5 * v, e.spread = 0.2 * v;
		e.lifetime = 0.1;
		e.size = 0.25 * R * sz, e.growth = 0.6 * v * sz;
		e.alpha = 0.6, e.slowdown = 0.0;
		e.emissive = true, e.ramp = true;
		e.cls = ParticleSystem::Class::Fire;
		Emit (fbFire, e, 1, e.lifetime, true);
		for (int k = 0; k < 3; k++) {
			double f = 0.6 + 0.5 * k; // (0.6, 1.1, 1.6 of its speed: as far as the volume's cloud is seen)
			ParticleSystem::Emission w (e);
			w.emissive = false, w.ramp = false;
			w.v0 = f * v, w.spread = 0.12 * v;
			w.lifetime = tEnd * (1.0 - 0.2 * k);
			// (each about as wide as its distance from the centre: the 48
			// directions' streaks overlapping twice - smaller and fainter, the
			// cloud did not show at all)
			w.size = 0.35 * R * sz, w.growth = 1.6 * f * v * sz;
			w.alpha = 0.55 - 0.12 * k;
			w.streak = 0.12f; // (drawn out along its motion: radial streaks)
			w.tinted = true, w.cold = true;
			w.albedo[0] = 0.92f, w.albedo[1] = 0.93f, w.albedo[2] = 0.95f; // (droplets and ice: white)
			w.cls = ParticleSystem::Class::Spray;
			Emit (fbSmoke, w, 1, w.lifetime, true);
		}
	}
}

void VDebris::WreckSmoke (double dt, double strength, const VECTOR3 &up, const VECTOR3 &groundVel, bool onWater)
{
	// the smoke of a burning wreck: puffs rising from it (hot smoke rises,
	// particle_update.glsl, some tens of metres over their half minute),
	// and above that, where they have spread, large long-lived puffs
	// carried off by the wind (the trail tier: fading in as the column's
	// fade out)
	ParticleSystem *sys = scene->Particles ();
	double D = sys->density, sz = std::pow (D, -1.0 / 3.0);
	double Rc = std::max (1.2 * size, 3.0);
	// (one column for the pieces lying together: the first of them this
	// frame takes it, the others' smoke is in it)
	static std::vector<VECTOR3> claimed;
	static double claimedT = -1e30;
	double simt = oapiGetSimTime ();
	if (simt != claimedT) claimed.clear (), claimedT = simt;
	for (const VECTOR3 &c : claimed)
		if (length (c - gpos) < 30.0 + 2.0 * Rc) return;
	claimed.push_back (gpos);
	// (afloat too: burning fuel smokes black; as it smoulders, its flames
	// out, a lighter grey - phase 5)
	float alb = (float)(0.09 + 0.22 * (1.0 - std::min (strength, 1.0)));
	if (!column.tex) column.tex = sys->SmokeTexture ();
	if (!drift.tex) drift.tex = sys->SmokeTexture ();
	column.accum += 6.0 * D * dt;
	uint32_t n = (uint32_t)column.accum;
	column.accum -= n;
	if (n) {
		ParticleSystem::Emission e{};
		e.p0 = e.p1 = gpos + up * (0.5 * size), e.vel = groundVel, e.dir = -up, e.dt = dt;
		e.v0 = 3.0, e.spread = 1.5;
		e.lifetime = 35.0;
		e.size = Rc * sz, e.growth = (0.12 * Rc + 0.6) * sz;
		e.alpha = 0.55 * strength, e.slowdown = 0.4;
		e.tinted = true, e.volumetric = true;
		e.albedo[0] = e.albedo[1] = e.albedo[2] = alb;
		e.cls = ParticleSystem::Class::WreckSmoke;
		Emit (column, e, n, e.lifetime);
	}
	drift.accum += 0.6 * D * dt;
	n = (uint32_t)drift.accum;
	drift.accum -= n;
	if (n) {
		ParticleSystem::Emission e{};
		e.p0 = e.p1 = gpos + up * (40.0 + 3.0 * Rc), e.vel = groundVel, e.dir = -up, e.dt = dt;
		e.v0 = 0.5, e.spread = 0.1 * Rc;
		e.lifetime = 240.0;
		e.size = 2.5 * Rc * sz, e.growth = 0.6 * sz;
		e.alpha = 0.45 * strength;
		LongLived (e, 0.3);
		e.tinted = true, e.cold = true, e.volumetric = true;
		e.albedo[0] = e.albedo[1] = e.albedo[2] = alb;
		e.cls = ParticleSystem::Class::WreckSmoke;
		Emit (drift, e, n, e.lifetime);
	}
}

void VDebris::Spray (const VECTOR3 &pos, const VECTOR3 &vel, double v, double airDrag)
{
	// struck at closing speed v [m/s] at pos (global): fragments of it (lit,
	// tumbling, glinting; drifting on in vacuum) and sparks, thrown out from
	// the struck side, more and faster the harder the blow
	ParticleSystem *sys = scene->Particles ();
	double dt = oapiGetPause () ? 0.0 : oapiGetSimStep ();
	if (!sys || dt <= 0.0) return;
	double D = sys->density;
	double s = std::min (v / 100.0, 10.0);                 // (0.12 .. 10)
	double sz = std::clamp (size / 5.0, 0.3, 3.0);         // (bigger pieces: more bits)
	bool air = airDrag > 0.01;
	VECTOR3 out = length (pos - gpos) > 1e-3 ? unit (pos - gpos) : _V(0, 0, 0);
	ParticleSystem::Emission e{};
	e.p0 = e.p1 = pos, e.vel = vel, e.dir = -out, e.dt = dt;
	e.v0 = std::min (0.15 * v, 150.0), e.spread = std::min (0.5 * v, 400.0);
	// (in air: faded within a few seconds, before they read as a cloud of
	// pebbles; in vacuum they coast on for half a minute, fading as they
	// shrink below a pixel on the screen - gpuparticle.vert - not at a set
	// time: VACUUM_PLAN V3)
	e.lifetime = air ? 4.0 : 30.0;
	e.size = std::clamp (0.02 * size, 0.06, 0.3), e.growth = 0.0;
	e.alpha = 1.0, e.slowdown = 0.4 * std::min (airDrag, 1.0); // (none in vacuum: they coast)
	e.tinted = true, e.cold = true, e.speck = true, e.spherical = true;
	e.albedo[0] = 0.8f, e.albedo[1] = 0.74f, e.albedo[2] = 0.6f; // (foil of thermal blankets, bare metal)
	uint32_t nf = (uint32_t)std::clamp ((6.0 + 25.0 * s) * sz * D, 4.0 * D, 120.0 * D);
	// (and some bigger bits, slower: sheets of blanket, bits of panels and struts)
	ParticleSystem::Emission c (e);
	c.size = std::clamp (0.04 * size, 0.25, air ? 1.5 : 0.8), c.v0 *= 0.6, c.spread *= 0.6;
	uint32_t nc = std::max (nf / (air ? 8 : 16), 2u);
	Emit (frags, e, nf, e.lifetime, true);
	Emit (frags, c, nc, c.lifetime, true);

	ParticleSystem::Emission k{};
	k.p0 = k.p1 = pos, k.vel = vel, k.dir = -out, k.dt = dt;
	k.v0 = std::min (0.2 * v, 200.0), k.spread = std::min (0.8 * v, 600.0);
	k.lifetime = air ? 0.5 : 1.5; // (long enough to see them glance off what they meet)
	k.size = 0.1, k.growth = 0.0;
	k.alpha = 20.0, k.slowdown = air ? 3.0 : 0.0; // (white hot: far brighter than sunlit surfaces)
	k.emissive = true, k.speck = true, k.spherical = true;
	k.ramp = !air; // (in vacuum: molten drops cooling as they coast, white to red - not slowed)
	k.cls = ParticleSystem::Class::Spark;
	Emit (sparks, k, (uint32_t)std::clamp ((10.0 + 40.0 * s) * D, 8.0 * D, 150.0 * D), k.lifetime, true);

	// a fast blow flashes (the metal vaporised where it struck)
	if (v > 300.0) {
		hitFlashT = oapiGetSimTime (), hitFlashI = std::min (v / 1000.0, 3.0);
		ParticleSystem::Emission f{};
		f.p0 = f.p1 = pos, f.vel = vel, f.dir = -out, f.dt = dt;
		f.v0 = 0.02 * v, f.spread = 0.04 * v;
		f.lifetime = 0.2, f.size = std::min (0.01 * v, 15.0) * pow (D, -1.0 / 3.0), f.growth = 0.04 * v;
		f.alpha = 0.6, f.slowdown = 0.0;
		f.emissive = true;
		Emit (fire, f, (uint32_t)(6.0 * D), 0.3, true);
	}
}

void VDebris::CollectLights (std::vector<LightSource> &lights)
{
	double simt = oapiGetSimTime ();
	auto add = [&](const VECTOR3 &pos, float I, const float col[3], double range, double radius, double sortRange) {
		LightSource l{};
		l.pos = pos, l.dir = _V(0, 0, 1);
		for (int k = 0; k < 3; k++) l.col[k] = I * col[k];
		l.range = range;
		l.att[0] = 1.0, l.att[1] = 0.0, l.att[2] = 1.0 / std::max (range * range * 0.01, 1.0);
		l.spot = false;
		l.owner = id;
		l.shadowOfs = 0.0;
		l.radius = radius, l.sortRange = sortRange;
		lights.push_back (l);
	};
	static const float fireCol[3] = { 1.0f, 0.55f, 0.2f };
	if (fbTf > 0.0 && simt >= fbT0 && simt - fbT0 < 1.3 * fbTf) {
		// the fireball in air: two lights of one source (the RT shadow rays
		// are shared between the lights of one owner: one ray per pixel):
		// - the ball, an extended source (half its radius) at its centre as
		//   drawn (raised above where it formed, rising), brightest at 0.25
		//   t_f - some ten times the sun's light at its surface - then dimming
		//   and reddening as it cools, out by 1.3 t_f
		// - its foot: the flash as it ignites, white-yellow, gone within 0.4
		//   s, and the fire at the surface it formed on while it burns hard
		//   there (the sea and the ground under it, the wreck beside it, lit
		//   from low down before it lifts off)
		double t = simt - fbT0, u = t / fbTf;
		double b = u < 0.25 ? 0.5 + 0.5 * u / 0.25 : pow (std::max (0.0, (1.3 - u) / 1.05), 1.5);
		double heat = std::clamp (1.0 - (u - 0.25) / 1.05, 0.0, 1.0);
		double k = std::min (1.0, fbR / 10.0);
		double Ib = k * 14.0 * b;
		float col[3] = { 1.0f, (float)(0.42 + 0.2 * heat), (float)(0.1 + 0.2 * heat) };
		double range = 15.0 * std::max (fbR, 2.0);
		VECTOR3 up = length (fbUp) > 0.5 ? fbUp : _V(0, 1, 0);
		double raise = fbVolume ? std::max (fbRaise, 0.0) * fbRNow : 0.0;
		if (Ib > 0.01) add (fbPos + up * raise, (float)Ib, col, range, 0.5 * fbRNow, range);
		double If = t < 0.4 ? k * 20.0 * exp (-10.0 * t) : 0.0;
		double Ig = fbGround > -1e8 && fbGround > -2.0 * fbR ? k * 8.0 * b * std::max (0.0, 1.0 - u / 0.6) : 0.0;
		if (If + Ig > 0.01) {
			double w = If / (If + Ig);
			float cf[3] = { 1.0f, (float)(0.5 + 0.35 * w), (float)(0.15 + 0.45 * w) };
			// (at the surface below its centre; before the ground is known: where it ignited)
			VECTOR3 foot = fbGround > -1e8 ? fbPos + up * (fbGround + 0.1 * fbRNow) : fbFlashPos;
			add (foot, (float)(If + Ig), cf, range, 0.35 * fbRNow, 0.0);
		}
	} else if (fbTf <= 0.0 && flashR > 0.0 && simt - flashT >= 0.0 && simt - flashT < 1.5) {
		// (out of the air: a dozen times the sun's light up close as it
		// flashes, falling as its core cools - fireball.frag: (1 + t / 0.08)
		// ^-1.2 in temperature - a third by the second frame, gone within a
		// fifth of a second)
		// (phase 4: half of it - the cloud carries the look; at 12 the station was lit out white)
		float I = (float)(std::min (1.0, flashR / 10.0) * 6.0 * pow (1.0 + (simt - flashT) / 0.08, -3.0));
		if (I > 0.01f) add (gpos, I, fireCol, 10.0 * std::max (flashR, 2.0), 0.0, 0.0);
	}
	float I = 0.0f;
	double range = 0.0;
	if (simt - hitFlashT < 0.3) { // (a fast blow's flash)
		float b = (float)(hitFlashI * 4.0 * exp (-15.0 * (simt - hitFlashT)));
		if (b > I) I = b, range = 40.0;
	}
	if (burning) {
		// (near the flames about as bright as the sun: the smoke over burning
		// wreckage glows at night, in daylight it stays dark)
		float b = (float)(1.2 * flameF * (0.85 + 0.15 * sin (simt * 23.0) * sin (simt * 7.3)));
		if (b > I) I = b, range = 8.0 * std::max (size, 1.0);
	}
	if (I > 0.01f) add (gpos, I, fireCol, range, 0.0, 0.0);
}

void VDebris::CollectFireballs (FireballRenderer &fr)
{
	// its fireball in air, marched as a volume (VDebris::Fireball decided:
	// large enough on screen), until its soot ball has passed to the smoke
	// volume (1.8 t_f). Its age by the simulation time: frozen while paused
	if (fbTf <= 0.0) {
		// out of the air: its free cloud (VACUUM_PLAN.md 3.3), expanding from
		// where it burst (moving on with the pieces), until it has thinned out
		if (!fbVac || flashR <= 0.0) return;
		double t = oapiGetSimTime () - fbT0;
		FreeCloud c = CloudOf (fbFuel, flashR, fbVacVs);
		if (t < 0.0 || t > c.TEnd ()) return;
		FireballRenderer::Ball b;
		b.owner = id;
		b.vacuum = true;
		b.scale = c.S (t), b.sigma = c.Sigma (t);
		b.r = 2.6 * b.scale;
		b.R = 2.6 * c.SEnd ();
		b.centre = scene->ViewPos (fbPos + fbVel * t);
		// (z up: on the ground, the ground below its centre - its sheet;
		// it moves on with the pieces, the ground stays: at rest on it, about)
		b.up = scene->ViewDir (fbUp);
		b.axis = scene->ViewDir (crossp (fbUp, fabs (fbUp.x) < 0.9 ? _V(1, 0, 0) : _V(0, 1, 0)));
		b.ground = fbGround > -1e8 ? fbGround - dotp (fbVel - GroundVelocity (fbPlanet ? fbPlanet : oapiGetGbodyByName ((char*)"Earth"), fbPos), fbUp) * t : -1e9;
		b.t = t, b.tf = c.TEnd ();
		b.seed = serial * 2654435761u;
		fr.Add (b);
		return;
	}
	if (!fbVolume && fbVolFade <= 0.0) return; // (or fading out to sprites)
	double t = oapiGetSimTime () - fbT0;
	if (t < 0.0 || t > 1.8 * fbTf || fbRNow <= 0.0) return;
	FireballRenderer::Ball b;
	b.owner = id;
	b.fade = fbVolFade;
	// (formed on the ground or the sea: a ball resting on it, not a dome cut
	// by it - its centre raised to 0.7 r above where it formed, rising on
	// from there)
	double raise = std::max (fbRaise, 0.0) * fbRNow;
	b.centre = scene->ViewPos (fbPos + fbUp * raise);
	b.up = scene->ViewDir (fbUp);
	b.axis = scene->ViewDir (length (fbDir) > 0.5 ? fbDir : fbUp);
	b.r = fbRNow, b.R = fbR;
	b.t = t, b.tf = fbTf;
	b.elong = fbEl, b.lift = fbLift, b.roll = fbRoll, b.g = fbG;
	b.lean = scene->ViewDir (fbLean); // (phase 4b meant to pass it and did not: no lean was drawn - phase 4c)
	b.ground = fbGround > -1e8 ? fbGround - raise : -1e9;
	b.seed = serial * 2654435761u;
	fr.Add (b);
}

void VDebris::CollectHeat (PlumeRenderer &pr)
{
	// the heat shimmer of its fireball (PLAN.md section 3): the hot air at
	// the ball's surface, by its temperature (the model's, not its drawn
	// brightness: the march's knee holds that), a column of hot air rising
	// 1.5 R above it, and for its first 0.3 s the thin shell of air its
	// front pushed out (a refraction ring); on past its flames while its
	// soot ball is hot (to 2.5 t_f; the site fire's own heat: CrashFires)
	if (fbTf <= 0.0) return;
	double t = oapiGetSimTime () - fbT0;
	if (t < 0.0 || t > 2.5 * fbTf || fbRNow <= 0.0) return;
	double u = t / fbTf, Th, Ts;
	FireballRenderer::ModelTemps (u, Th, Ts);
	PlumeRenderer::HeatSource h;
	VECTOR3 up = length (fbUp) > 0.5 ? fbUp : _V(0, 1, 0);
	double raise = fbVolume ? std::max (fbRaise, 0.0) * fbRNow : 0.0;
	h.pos = scene->ViewPos (fbPos + up * raise);
	h.up = scene->ViewDir (up);
	h.r = 1.1 * fbRNow;
	h.heat = std::clamp ((Th - 1100.0) / 1100.0, 0.0, 1.0) * std::min (1.0, fbR / 5.0);
	h.column = 1.5 * fbR;
	h.columnHeat = h.heat * (1.0 - exp (-t / 0.3));
	if (t < 0.3) {
		h.ring = 1.15 * fbRNow + 60.0 * t;
		h.ringW = 0.08 * fbR;
		h.ringA = 1.5 * (1.0 - t / 0.3);
	}
	h.drift = 10.0 * t / fbRNow;
	pr.AddHeat (h);
}

void VDebris::DrawParticles ()
{
	ParticleSystem *sys = scene->Particles ();
	if (!sys) return;
	double simt = oapiGetSimTime ();
	for (const Ring *r : { &drift, &column, &fbSmoke, &smoke, &quench, &fbSteam, &dust, &fbSkirt, &frags, &vent, &fire, &fbFire, &fbCore, &fbFlashR, &sparks, &embers })
		if (r->sys == sys && simt - r->lastEmit < 1.5 * r->life + 0.1)
			sys->Draw (*r, r->tex, r->emissive, scene->FrameSet ()); // (if not released while idle)
	water.Draw (scene);
}

void VDebris::Render (RenderPass pass)
{
	if (!mesh || hidden) return;
	Mat4 mv = scene->ModelView (gpos, grot);
	scene->Sunlight (gpos);
	if (pass == RenderPass::Depth) SetMotion ();
	// hot (burned off, burning): glowing, most on its windward side; a
	// burning piece's hull glows where its flames are (phase 5: the side
	// they stream off, flickering with them), whatever its temperature
	float glow = (float)std::clamp ((info.temp - 900.0) / 700.0, 0.0, 1.0);
	if (flameF > 0.0) {
		double simt = oapiGetSimTime ();
		double flick = 0.8 + 0.2 * sin (simt * 19.0 + info.serial) * sin (simt * 5.7);
		glow = std::max (glow, (float)(0.6 * flameF * flick));
	}
	if (glow > 0.0f) {
		// (the side the flames stream off: aft of a flying piece, the top of
		// one at rest - the emission's direction reversed)
		VECTOR3 d = length (flameDir) > 0.5 ? -flameDir : _V(0, 1, 0);
		if (flameF <= 0.0) {
			VECTOR3 v;
			oapiGetGlobalVel (hObj, &v);
			d = length (v) > 1.0 ? unit (v) : _V(0, 1, 0);
		}
		scene->SetObjectHeat (scene->ViewDir (d), glow);
	}
	scene->DrawMesh (mesh.get(), mv, pass);
	if (glow > 0.0f) scene->SetObjectHeat (_V(0, 0, 0), 0.0f);
}

void VDebris::RenderShadow (ShadowMap &sm)
{
	if (!mesh || hidden || length (gpos - sm.Centre()) > sm.Radius() + size) return;
	sm.DrawMesh (mesh.get(), Mat4::FromRotTrans (grot, gpos - sm.Centre()));
}

void VVessel::RenderShadow (ShadowMap &sm)
{
	if (length (gpos - sm.Centre()) > sm.Radius() + size) return;
	Mat4 model = Mat4::FromRotTrans (grot, gpos - sm.Centre());
	for (auto &rec : meshes)
		if (rec.mesh && (rec.visMode & MESHVIS_EXTERNAL)) {
			sm.DrawMesh (rec.mesh.get(), model * Mat4::Translation (rec.ofs), &rec.grpTransform, &rec.grpVisible);
			if (rec.walls) {
				WallTransforms (rec);
				sm.DrawMesh (rec.walls.get(), model * Mat4::Translation (rec.ofs), &wallTf, &wallVis);
			}
		}
}

void VVessel::WallTransforms (const MeshRec &rec)
{
	// the walls move with the groups they are cut in
	size_t n = rec.wallGroup.size();
	wallTf.resize (n), wallPrevTf.resize (n), wallVis.resize (n);
	for (size_t i = 0; i < n; i++) {
		uint32_t g = rec.wallGroup[i];
		wallTf[i] = g < rec.grpTransform.size() ? rec.grpTransform[g] : Mat4::Identity();
		wallPrevTf[i] = g < rec.prevGrpTransform.size() ? rec.prevGrpTransform[g] : wallTf[i];
		wallVis[i] = g < rec.grpVisible.size() ? rec.grpVisible[g] : 1;
	}
}

namespace {
	// beacons as light sources: half intensity at this many beacon radii
	constexpr double BEACON_LIGHT_REACH = 8.0;
	constexpr double BEACON_LIGHT_INTENSITY = 0.8;
	// interior lights seen from outside: intensity factor, range factor, and
	// the emission of the vessel's glass per unit of light
	constexpr double INTERIOR_SPILL = 0.5;
	constexpr double INTERIOR_SPILL_REACH = 4.0;
	constexpr float INTERIOR_GLASS_GLOW = 0.6f;
	constexpr float BEACON_GLOW = 3.0f; // brightness of beacon sprites
	constexpr float STAR_BRIGHTNESS = 20.0f;      // the sun's disc relative to lit white surfaces
	constexpr float STAR_GLOW = 2.0f;             // brightness of the glow around it
	constexpr double STAR_GLOW_SIZE = 6.0;        // glow radius [star radii]
	constexpr double STAR_GLOW_MIN_ANGLE = 0.02;  // minimum glow radius [rad]

	bool BeaconLit (const BEACONLIGHTSPEC *bs, double simt)
	{
		if (!bs->active || !bs->pos || !bs->col) return false;
		return bs->period <= 0.0 || fmod (simt + bs->tofs, bs->period) <= bs->duration;
	}
}

double VVessel::LightExit (const LightEmitter *le)
{
	auto it = lightExit.find (le);
	if (it != lightExit.end()) return it->second;
	VECTOR3 p = le->GetPosition(), d = le->GetDirection();
	double exit = 0.0;
	for (auto &rec : meshes)
		if (rec.mesh && (rec.visMode & MESHVIS_EXTERNAL))
			exit = std::max (exit, rec.mesh->RayExit (p - rec.ofs, d, 2.0 * size));
	if (exit > 0.0) exit += 0.05;
	lightExit[le] = exit;
	return exit;
}

void VVessel::CollectLights (std::vector<LightSource> &lights)
{
	glassGlow[0] = glassGlow[1] = glassGlow[2] = 0.0f;
	bool internal = oapiCameraInternal() && oapiCameraTarget() == hObj;
	VECTOR3 cam = scene->CameraPos();
	for (DWORD i = 0; i < vessel->LightEmitterCount(); i++) {
		const LightEmitter *le = vessel->GetLightEmitter (i);
		if (!le || !le->IsActive()) continue;
		int vis = le->GetVisibility();
		// Interior lights seen from outside spill through the windows: a weaker
		// light reaching further, and a glow of the vessel's glass.
		bool spill = !internal && !(vis & LightEmitter::VIS_EXTERNAL);
		if (internal && !(vis & LightEmitter::VIS_COCKPIT)) continue;
		double in = le->GetIntensity();
		if (in <= 0.0) continue;
		if (spill) in *= INTERIOR_SPILL;
		LightSource l{};
		l.owner = id;
		l.pos = gpos + mul (grot, le->GetPosition());
		if (le->GetType() == LightEmitter::LT_POINT) {
			const PointLight *pl = (const PointLight*)le;
			l.range = pl->GetRange();
			for (int k = 0; k < 3; k++) l.att[k] = pl->GetAttenuation()[k];
		} else if (le->GetType() == LightEmitter::LT_SPOT) {
			const SpotLight *sl = (const SpotLight*)le;
			l.range = sl->GetRange();
			for (int k = 0; k < 3; k++) l.att[k] = sl->GetAttenuation()[k];
			l.spot = true;
			l.dir = mul (grot, le->GetDirection());
			l.umbra = sl->GetUmbra();
			l.penumbra = sl->GetPenumbra();
			l.shadowOfs = LightExit (le);
		} else {
			continue;
		}
		if (spill) {
			l.range *= INTERIOR_SPILL_REACH;
			l.att[1] /= INTERIOR_SPILL_REACH;
			l.att[2] /= INTERIOR_SPILL_REACH * INTERIOR_SPILL_REACH;
		}
		if (length (l.pos - cam) > l.range + 10e3) continue; // nothing in view can be lit
		const COLOUR4 &c = le->GetDiffuseColour();
		l.col[0] = (float)(c.r * in), l.col[1] = (float)(c.g * in), l.col[2] = (float)(c.b * in);
		if (spill)
			for (int k = 0; k < 3; k++) glassGlow[k] += l.col[k] * INTERIOR_GLASS_GLOW;
		lights.push_back (l);
	}

	// engine plumes light their surroundings: an extended source of radiance
	// L and side area A gives an irradiance of L A / (pi d^2) farther away
	// (L in units of a sunlit white surface's radiance, E_sun / pi; the
	// irradiance in units of E_sun), up to L right beside it (the light's
	// attenuation is capped at 1: a surface facing a half space of radiance
	// L receives pi times its radiance in physical units, L E_sun here).
	// Three points along the flame share its area, each a sphere of the jet's
	// radius there (lights.glsl: surfaces beside the flame, at grazing
	// angles, lit); with many plumes, the brightest plumes' only (one point
	// at the flame's middle for the others: MAX_LIGHTS)
	const bool engineLight = vkc::Gfx ().engineLight;
	if (engineLight && !plumeLights.empty ()) {
		std::vector<size_t> order (plumeLights.size ());
		for (size_t i = 0; i < order.size (); i++) order[i] = i;
		auto power = [](const PlumeLight &pl) { return pl.area * std::max (pl.col[0], std::max (pl.col[1], pl.col[2])); };
		std::sort (order.begin (), order.end (), [&](size_t a, size_t b) { return power (plumeLights[a]) > power (plumeLights[b]); });
		const size_t POINTS = 15; // (the Shuttle's five engines)
		size_t extended = order.size () * 3 <= POINTS ? order.size () : order.size () < POINTS ? (POINTS - order.size ()) / 2 : 0;
		for (size_t r = 0; r < order.size (); r++) {
			const PlumeLight &pl = plumeLights[order[r]];
			float peak = std::max (pl.col[0], std::max (pl.col[1], pl.col[2]));
			if (peak <= 0.0f || pl.area <= 0.0) continue;
			int np = r < extended ? 3 : 1;
			double area = pl.area / np;
			// (down to 2e-5 of sunlight, about 8 full moons: at night a launch's
			// flames light the ground for kilometres)
			double reach = sqrt (pl.area * peak / (PI * 2e-5));
			for (int k = 0; k < np; k++) {
				int pt = np == 3 ? k : 1;
				LightSource l{};
				l.owner = id;
				l.pos = pl.pos[pt];
				l.range = std::min (reach, 25e3);
				l.sortRange = reach; // (the brightest first: they get the shadowed slots)
				l.radius = pl.radius[pt];
				// (kept while the camera is far beyond the light's reach: the
				// flame lights its own smoke, which is seen from afar; without
				// it the smoke at the flame's end was dark at night and hid the
				// flame from 60 km away. Farther than this the flame is a point)
				if (length (l.pos - cam) > l.range + 150e3) continue;
				l.att[0] = 0.0, l.att[1] = 0.0, l.att[2] = PI / area;
				for (int c = 0; c < 3; c++) l.col[c] = pl.col[c];
				lights.push_back (l);
			}
		}
	}

	// the reentry plasma sheath, ahead of the windward hull, lights the
	// vessel and what is near it (seen at night): an extended source as the
	// plumes, the windward side's area, its mean radiance
	SheathState sh;
	if (engineLight && reentryLegacy != 1 && Sheath (sh)) {
		float rgb[3];
		SheathState::Colour (sh.level, rgb);
		float L = sh.Radiance ();
		float peak = L * std::max (rgb[0], std::max (rgb[1], rgb[2]));
		double area = PI * size * size;
		LightSource l{};
		l.owner = id;
		l.pos = gpos + sh.dirGlobal * (0.8 * size);
		l.range = std::min (sqrt (area * peak / 0.002), 5000.0);
		if (peak > 0.0f && length (l.pos - cam) < l.range + 10e3) {
			l.att[0] = 0.0, l.att[1] = 0.0, l.att[2] = PI / area;
			// (not all of it reaches the hull: the layer glows mostly outwards)
			for (int k = 0; k < 3; k++) l.col[k] = (float)(0.4 * PI * L * rgb[k]);
			lights.push_back (l);
		}
	}

	// lit beacons also illuminate their surroundings (mainly the vessel itself)
	double simt = oapiGetSimTime ();
	for (DWORD i = 0; ; i++) {
		const BEACONLIGHTSPEC *bs = vessel->GetBeacon (i);
		if (!bs) break;
		if (!BeaconLit (bs, simt)) continue;
		LightSource l{};
		l.owner = id;
		l.pos = gpos + mul (grot, *bs->pos);
		double reach = BEACON_LIGHT_REACH * bs->size; // distance of half intensity
		l.range = 4.0 * reach;
		if (length (l.pos - cam) > l.range + 2e3) continue;
		l.att[0] = 1.0, l.att[1] = 0.0, l.att[2] = 1.0 / (reach * reach);
		for (int k = 0; k < 3; k++) l.col[k] = (float)((&bs->col->x)[k] * BEACON_LIGHT_INTENSITY);
		lights.push_back (l);
	}
}

namespace {
	// ray (global frame) against meshes in a frame at pos, rot
	template <class Meshes, class Ofs>
	bool MeshesRayHit (const Meshes &meshes, Ofs ofs, const VECTOR3 &pos, const MATRIX3 &rot,
		const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n)
	{
		VECTOR3 lo = tmul (rot, o - pos), ld = tmul (rot, d);
		bool found = false;
		t = maxDist;
		for (auto &m : meshes) {
			const VkMesh *mesh = nullptr;
			VECTOR3 mo{};
			if (!ofs (m, mesh, mo) || !mesh) continue;
			double tm;
			VECTOR3 nm;
			if (mesh->RayHit (lo - mo, ld, t, tm, nm)) t = tm, n = mul (rot, nm), found = true;
		}
		return found;
	}
}

bool VVessel::RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n)
{
	// (bounding sphere first; animations are not followed)
	VECTOR3 c = gpos - o;
	double along = std::clamp (dotp (c, d), 0.0, maxDist);
	if (length (c - d * along) > size) return false;
	return MeshesRayHit (meshes, [](const MeshRec &r, const VkMesh *&m, VECTOR3 &ofs) {
		if (!(r.visMode & MESHVIS_EXTERNAL)) return false;
		m = r.mesh.get(), ofs = r.ofs;
		return true;
	}, gpos, grot, o, d, maxDist, t, n);
}

void VVessel::CollectPlumes (PlumeRenderer &pr)
{
	EmitWake (oapiGetPause () ? 0.0 : oapiGetSimStep ());
	contrailBand = -1;
	DWORD n = vessel->GetExhaustCount ();
	plumeExhaust.assign (n, 0);
	plumeLights.clear ();
	plumePoints.clear ();
	if (!pr.enabled) return;
	VECTOR3 as;
	vessel->GetAirspeedVector (FRAME_LOCAL, as);
	VECTOR3 wind = scene->ViewDir (mul (grot, -as)); // the air past the vessel
	double pa = vessel->GetAtmPressure (), rho = vessel->GetAtmDensity ();
	// (testing: ORBITER_VK_THRUST_VACUUM=1 keeps the vacuum thrust, the state
	// before E3; ORBITER_VK_THRUST_LOG=1 logs the thrust of the lit exhausts,
	// every 2 s per vessel)
	static const bool thrustVacuum = getenv ("ORBITER_VK_THRUST_VACUUM") && atoi (getenv ("ORBITER_VK_THRUST_VACUUM"));
	static const bool thrustLog = getenv ("ORBITER_VK_THRUST_LOG") && atoi (getenv ("ORBITER_VK_THRUST_LOG"));
	static std::map<uint32_t, double> thrustLogT;
	bool logNow = false;
	if (thrustLog) {
		double simt = oapiGetSimTime ();
		auto it = thrustLogT.find (id);
		if (it == thrustLogT.end () || simt - it->second >= 2.0 || simt < it->second) thrustLogT[id] = simt, logNow = true;
	}
	float sun = (float)scene->Sunlight (gpos);
	double dt = oapiGetPause () ? 0.0 : oapiGetSimStep ();
	static std::mt19937 rng (54321);
	std::uniform_real_distribution<double> uni (0.0, 1.0);
	for (DWORD i = 0; i < n; i++) {
		EXHAUSTSPEC es;
		if (!vessel->GetExhaustSpec (i, &es) || !es.lpos || !es.ldir) continue;
		PlumeOverride o = pr.config.Get (vessel, i);
		PlumeType type = o.type >= 0 ? (PlumeType)o.type : PlumeConfig::Infer (vessel, es.th);
		if (type == PlumeType::Texture) continue; // (the add-on's flame texture)
		plumeExhaust[i] = 1;
		double lvl = vessel->GetExhaustLevel (i);
		if (lvl <= 0.0) continue;
		VECTOR3 gd = mul (grot, *es.ldir);
		PlumeDraw pd{};
		pd.pos = scene->ViewPos (gpos + mul (grot, *es.lpos) - gd * es.lofs);
		pd.dir = scene->ViewDir (-gd);
		pd.wind = wind;
		pd.exitRadius = 0.5 * es.wsize * o.scale;
		pd.length = es.lsize * o.scale * o.length;
		pd.level = std::min (1.0, lvl);
		// the thrust: the vacuum rating (the class, 'unknown' when 0) and the
		// fraction the ambient pressure leaves of it, as the core applies it
		// (VESSEL::GetThrusterMax (th, p): Isp and thrust fall linearly with p)
		double th0 = es.th ? vessel->GetThrusterMax0 (es.th) : 0.0;
		double phys = th0 > 0.0 ? std::clamp (vessel->GetThrusterMax (es.th, pa) / th0, 0.0, 2.0) : 1.0;
		pd.thrust = th0;
		pd.delivered = thrustVacuum ? 1.0 : phys;
		pd.ambientPressure = pa;
		pd.airDensity = rho;
		pd.type = type;
		pd.brightness = o.brightness;
		pd.flicker = (float)(1.0 - 0.5 * es.modulate * uni (rng));
		pd.sunlight = sun;
		pd.tint = es.tex ? (VkTexture*)es.tex : nullptr;
		pd.seed = id * 131u + i * 977u;
		if (logNow)
			oapiWriteLogV ("VulkanClient: thrust %s exh %u lvl %.3f F0 %.0f N delivered %.3f (physical %.3f) p %.0f Pa",
				vessel->GetName (), (unsigned)i, pd.level, th0, pd.delivered, phys, pa);
		PlumeInfo info{};
		if (pr.Add (pd, id, &info) >= 0) {
			EmitSmoke (i, (int)type, pd, info, dt);
			EmitDust (i, (int)type, pd, dt);
			AddPlumePoint (info, (int)type);
			// (a flame lights its surroundings; cold gas does not glow)
			if (type != PlumeType::ColdGas) {
				PlumeLight pl{ { scene->GlobalPos (info.lightPos[0]), scene->GlobalPos (info.lightPos[1]), scene->GlobalPos (info.lightPos[2]) },
					{ info.lightRadius[0], info.lightRadius[1], info.lightRadius[2] }, { info.lightCol[0], info.lightCol[1], info.lightCol[2] }, info.lightArea };
				plumeLights.push_back (pl);
			}
		}
		else {
			if (i < smoke.size()) smoke[i].havePrev = false;
			if (i < contrails.size()) contrails[i].Reset ();
		}
	}
}

// A flame far away, a few pixels across or less, is easily missed by the
// plume's march (half the resolution, a few steps through a thin volume):
// it also draws as a point of light, a soft sprite of a few pixels that
// carries the flame's light: its mean drawn radiance times its solid angle,
// spread over the sprite's pixels (the sprite's profile: a core and a wide
// skirt, about 0.14 of its square in area). It fades out as the volume
// resolves (3 to 10 pixels across), so that the sum stays the same. Drawn
// in the transparent pass, depth-tested: terrain and vessels hide it
void VVessel::AddPlumePoint (const PlumeInfo &info, int type)
{
	ParticleSystem *sys = scene->Particles ();
	if (!sys || info.solid <= 0.0 || type == (int)PlumeType::ColdGas) return;
	const double POINT_HALF = 4.0, PROFILE_AREA = 0.14; // (half size of the sprite [px])
	double pxr = sys->PxPerRad ();
	double sizePx = std::sqrt (info.solid) * pxr;
	double w = 1.0 - std::clamp ((sizePx - 3.0) / 7.0, 0.0, 1.0);
	w = w * w * (3.0 - 2.0 * w);
	if (w <= 0.0) return;
	VECTOR3 vp = info.lightPos[1];
	if (vp.z <= 0.0) return;
	// (peak radiance: the energy over the sprite's area, in pixels)
	double gain = w * info.solid * pxr * pxr / (PROFILE_AREA * 4.0 * POINT_HALF * POINT_HALF);
	float r = (float)(info.radiance[0] * gain), g = (float)(info.radiance[1] * gain), b = (float)(info.radiance[2] * gain);
	float h = (float)(POINT_HALF * vp.z / pxr), x = (float)vp.x, y = (float)vp.y, z = (float)vp.z;
	PVtx q[4] = {
		{ x - h, y - h, z, 0.0f, 0.0f, r, g, b, 1 }, { x + h, y - h, z, 1.0f, 0.0f, r, g, b, 1 },
		{ x + h, y + h, z, 1.0f, 1.0f, r, g, b, 1 }, { x - h, y + h, z, 0.0f, 1.0f, r, g, b, 1 } };
	plumePoints.insert (plumePoints.end (), { q[0], q[1], q[2], q[0], q[2], q[3] });
}

void VVessel::EmitSmoke (DWORD idx, int type, const PlumeDraw &pd, const PlumeInfo &info, double dt)
{
	ParticleSystem *sys = scene->Particles ();
	if (!sys || !sys->enabled) return;
	if (idx >= smoke.size()) smoke.resize (idx + 1);
	SmokeRing &r = smoke[idx];
	const PlumeClass &c = PlumeConfig::Class ((PlumeType)type);
	double air = pd.ambientPressure / 101325.0;
	double t = c.smokeAir1 > c.smokeAir0 ? std::clamp ((air - c.smokeAir0) / (c.smokeAir1 - c.smokeAir0), 0.0, 1.0) : (air > c.smokeAir0 ? 1.0 : 0.0);
	double fade = c.smokeOpacity * t * t * (3.0 - 2.0 * t) * pd.level;
	// the exhaust's water condensing in cold air: a contrail from where the
	// flame ends (Contrail.h: the temperature gate, the session's humidity)
	if (c.contrail > 0.0f && dt > 0.0 && pd.airDensity > 0.0 && vessel->GetAtmRef ()) {
		if (idx >= contrails.size()) contrails.resize (idx + 1);
		OBJHANDLE hp = vessel->GetAtmRef ();
		VECTOR3 csrc = scene->GlobalPos (info.end), cvel, cair;
		vessel->GetGlobalVel (cvel);
		vessel->GetAirspeedVector (FRAME_GLOBAL, cair);
		double cp;
		double cT = contrail::AirTemperature (hp, csrc, &cp);
		contrail::Gate g = contrail::Evaluate (cT, cp, vessel->GetAltitude (), true, contrail::SessionHumidity (hp));
		contrails[idx].tex = scene->FireballTexture (3);
		if (contrailBand < 0) contrailBand = (int)idx;
		contrails[idx].Emit (sys, csrc, cvel, cair, g, std::max (1.5 * info.endRadius, 2.0), c.contrail * pd.level, dt, contrailBand == (int)idx);
	} else if (idx < contrails.size()) contrails[idx].Reset ();
	if (fade < 0.01 || dt <= 0.0) { r.havePrev = false; return; }
	// the smoke leaves the end of the flame, or spreads from the surface the
	// jet meets
	bool atSurface = info.hit && info.coverage > 0.5;
	VECTOR3 src = scene->GlobalPos (atSurface ? info.hitPos : info.end);
	VECTOR3 flow = scene->GlobalDir (atSurface ? info.hitN : info.endDir);
	VECTOR3 vel;
	vessel->GetGlobalVel (vel);
	VECTOR3 as;
	vessel->GetAirspeedVector (FRAME_LOCAL, as);
	double D = sys->density, sizeScale = std::pow (D, -1.0 / 3.0);
	bool rcs = type == (int)PlumeType::RCS, cold = type == (int)PlumeType::ColdGas;
	// the smoke leaves the end of the flame with some of the jet's speed left,
	// relative to the air, and stops after about 1.5 flame lengths: it follows
	// the flame out of its end instead of the vessel's path
	const double CARRY = 1.5;
	bool jet = !atSurface && !rcs && !cold && c.smokeJet > 0.0f;
	double Lf = std::max (info.length, 1.0);
	// (as wide as the flame where it visibly ends, about)
	// (in thin air the flame's bell is far wider than at sea level (its end
	// radius 6 m to 15 m for the Shuttle's SRBs): the smoke grows with it by
	// the square root only, and the opacity goes down with the square of the
	// size: about the same smoke spread wider, not a ball hiding the flame)
	// (Rref: about the jet's end radius at sea level, 3.2 exit radii; a
	// smaller one, another class's flame, is taken as it is)
	double Rref = 3.2 * pd.exitRadius;
	double Reff = atSurface || info.endRadius <= Rref ? info.endRadius : Rref * std::sqrt (info.endRadius / Rref);
	double thinAlt = std::min (1.0, (info.endRadius <= Rref || atSurface) ? 1.0 : (Rref / Reff) * (Rref / Reff));
	double size = 5.0 * Reff * sizeScale;
	double v0 = 40.0 * pd.level;
	// spacing of a third of the particle size along the trail
	double rate = std::clamp ((length (as) + v0) / (0.35 * std::max (size / sizeScale, 0.5)), 6.0, 250.0) * D;
	if (atSurface) rate = 120.0 * D; // (a cloud building up around the point)
	r.onTime = r.havePrev ? r.onTime + dt : 0.0;
	if (rcs || cold) {
		// hypergolic jets: a brief puff of unburnt propellant as they light,
		// then nothing lingers in vacuum; cold gas: a faint frosty stream
		rate = (rcs ? (r.onTime < 0.08 ? 250.0 : 0.0) : 40.0) * D * pd.level;
	}
	// (the ring for this rate: without one, no smoke, but the trail below
	// still)
	bool ring = sys->Reserve (r, rate, c.smokeLife, rcs || cold ? ParticleSystem::Class::Puff : ParticleSystem::Class::EngineSmoke);
	r.accum += rate * dt;
	uint32_t n = (uint32_t)std::max (r.accum, 0.0);
	r.accum -= n;
	if (!r.havePrev) r.prev = src - vel * dt;
	if (n && ring) {
		ParticleSystem::Emission e{};
		e.p0 = r.prev, e.p1 = src;
		e.vel = vel, e.dir = -flow; // (emitted along -dir)
		// (deflected by a surface: it spreads far along it, carried by the jet,
		// and rises)
		e.v0 = atSurface ? 4.0 : v0;
		e.spread = atSurface ? 1.5 * v0 + 20.0 : 0.6 * v0;
		e.planar = atSurface;
		e.lifetime = c.smokeLife, e.dt = dt;
		e.size = size, e.growth = (jet ? 0.65 : 0.4) * size + 2.0 * sizeScale;
		e.alpha = fade * thinAlt * std::min (1.0, 1.3 / (D * sizeScale * sizeScale));
		e.slowdown = atSurface ? 0.8 : ParticleSystem::ExhaustDrag (pd.airDensity);
		if (jet) {
			VECTOR3 asg;
			vessel->GetAirspeedVector (FRAME_GLOBAL, asg);
			e.vel = vel - asg; // (the air's velocity there)
			e.v0 = c.smokeJet * pd.level;
			e.spread = 15.0 + 0.1 * e.v0;
			e.slowdown = std::max (e.v0 / (CARRY * Lf), 0.05);
		}
		if (rcs || cold) {
			// (along the jet, spreading within its narrow cone)
			e.v0 = rcs ? 60.0 : 30.0, e.spread = rcs ? 25.0 : 10.0;
			e.size = (rcs ? 0.3 : 0.15) * sizeScale, e.growth = (rcs ? 5.0 : 2.0) * sizeScale;
			e.alpha = (rcs ? 0.2 : 0.05) * pd.level;
		}
		e.tinted = true;
		e.volumetric = !rcs && !cold;
		e.cls = rcs || cold ? ParticleSystem::Class::Puff : ParticleSystem::Class::EngineSmoke;
		for (int k = 0; k < 3; k++) e.albedo[k] = c.smokeAlbedo[k];
		sys->Emit (r, e, n);
	}
	// the long trail: where this smoke has spread (its size at about 0.4 of
	// its life), carried on by the wind
	if (!rcs && !cold && c.trail > 0.0f && sys->trails) {
		// (the engine smoke's size at 0.4 of its life, where it has faded for
		// the trail's puffs to take over: of the same width; at least some
		// 100 m for solid motors, from afar their smoke is a column hundreds
		// of metres wide)
		double R = size / sizeScale;
		// (at the pad, or hardly moving: the ground cloud, large puffs; they
		// give way to the narrower column as the vehicle climbs away)
		double speed = length (as), hover = atSurface ? 1.0 : 1.0 - std::clamp ((speed - 40.0) / 100.0, 0.0, 1.0);
		double S0 = std::max (R + (0.4 * R + 2.0) * 0.4 * c.smokeLife, (100.0 + 200.0 * hover) * c.trail);
		// laid where the jet leaves the smoke: some 1.5 flame lengths on along
		// the flow (in the air's frame, where the jet's smoke comes to rest)
		VECTOR3 off = jet ? flow * (0.86 * CARRY * Lf) : _V(0, 0, 0);
		EmitTrail (idx, c, c.trail * fade / std::max (c.smokeOpacity, 1e-3f), src + off, (r.havePrev ? r.prev : src - vel * dt) + off, S0, dt);
	}
	r.prev = src;
	r.havePrev = true;
}

// A launch leaves a trail that stands in the sky for many minutes, from the
// pad up to where the air thins: the smoke spreads, and the wind, of other
// strength and direction in each layer, shears it into a zigzag. The engine
// smoke above is fine and short-lived; this lays large puffs along the path
// through the air (one for every quarter of their size, and a few a second
// when it hardly moves: the cloud at the pad), at rest in the air, which
// take over as that fades. They live about 15 minutes (particle_emit.comp:
// 0.5 to 1.5 of it), grow slowly and thin as they do (ParticleAlpha), and
// drift with the air at their altitude (particle_update.glsl: the wind's
// layers, gentle eddies). strength: 0..1 (PlumeClass::trail, the smoke's
// fading in the thin air and the engine's level)
void VVessel::EmitTrail (DWORD idx, const PlumeClass &c, double strength, const VECTOR3 &src, const VECTOR3 &srcPrev, double S0, double dt)
{
	const double LIFE = 900.0;        // [s]
	ParticleSystem *sys = scene->Particles ();
	if (strength < 0.01 || dt <= 0.0) return;
	if (idx >= trail.size()) trail.resize (idx + 1);
	SmokeRing &r = trail[idx];
	VECTOR3 vel, as;
	vessel->GetGlobalVel (vel);
	vessel->GetAirspeedVector (FRAME_GLOBAL, as);
	double spacing = std::max (0.25 * S0, 20.0);
	double rate = std::min (length (as) / spacing + 2.0, 80.0); // [1/s]
	// (puffs per engine: the ring follows the rate; a solid motor's lays
	// some thousands over the ascent)
	if (!sys->Reserve (r, rate, LIFE, ParticleSystem::Class::Trail)) return;
	r.accum += rate * dt;
	uint32_t n = (uint32_t)std::max (r.accum, 0.0);
	r.accum -= n;
	if (!n) return;
	ParticleSystem::Emission e{};
	e.p0 = srcPrev, e.p1 = src;
	e.vel = vel - as; // (the air's velocity there)
	double sp = length (as);
	e.dir = sp > 1.0 ? as / sp : _V(0, 0, 0);
	e.v0 = 0.0, e.spread = 1.0 + 0.005 * S0; // (a puff does not wander off the column)
	e.lifetime = LIFE, e.dt = dt;
	e.size = S0, e.growth = 4.0;  // (some 4 km across after 15 minutes: turbulent spreading)
	e.alpha = 0.5 * std::min (strength, 1.0);
	e.slowdown = 0.3;             // (it takes on the wind within seconds)
	e.trail = true, e.cls = ParticleSystem::Class::Trail;
	e.tinted = true, e.cold = true; // (cooled: it does not rise)
	e.volumetric = true;
	for (int k = 0; k < 3; k++) e.albedo[k] = c.smokeAlbedo[k];
	sys->Emit (r, e, n);
}

void VVessel::EmitDust (DWORD idx, int type, const PlumeDraw &pd, double dt)
{
	ParticleSystem *sys = scene->Particles ();
	PlumeRenderer *pr = scene->Plumes ();
	if (idx >= dust.size()) dust.resize (idx + 1);
	if (idx >= mist.size()) mist.resize (idx + 1);
	SmokeRing &r = dust[idx];
	// a jet stirs up the ground within a distance growing with the thrust it delivers
	double thrust = pd.DeliveredThrust (); // (in this air: none at Venus' surface, E3)
	if (!pr || !pr->rayCast || dt <= 0.0 || thrust < 1e3 ||
		type == (int)PlumeType::RCS || type == (int)PlumeType::ColdGas || type == (int)PlumeType::Ion) { r.havePrev = false; mist[idx].havePrev = false; return; }
	// (in vacuum the jet expands freely and reaches several times as far:
	// Apollo LMs raised a sheet of dust from some 30 m)
	bool vacuum = pd.ambientPressure <= 100.0;
	double reach = (vacuum ? 11.0 : 2.5) * sqrt (thrust / 1e3);
	double t;
	VECTOR3 vn;
	int kind = 2;
	if (!pr->rayCast (id, pd.pos, unit (pd.dir), reach, t, vn, &kind) || kind == 2) { r.havePrev = false; mist[idx].havePrev = false; return; }
	// (in vacuum: falling off more slowly, the sheet thins towards the reach)
	double strength = vacuum ? (1.0 - t / reach) * (1.0 - 0.5 * t / reach) : (1.0 - t / reach) * (1.0 - t / reach);
	OBJHANDLE hp = vessel->GetSurfaceRef ();
	if (!hp || strength < 0.02) { r.havePrev = false; mist[idx].havePrev = false; return; }
	VECTOR3 hit = scene->GlobalPos (pd.pos + unit (pd.dir) * t);
	VECTOR3 n = scene->GlobalDir (vn);
	// the jet scorches the ground (or a pad) where it is still hot: close to
	// the nozzle, darkening over seconds of firing
	double hot = std::clamp (1.0 - t / (0.25 * reach), 0.0, 1.0);
	bool water = false;
	vksurf::Point sp;
	if (kind == 0) { sp = vksurf::QueryGlobal (scene, hp, hit); water = sp.IsWater (); } // (the drawn water mask)
	// the jet on the water: its footprint pressed flat, chop, rings and foam (WaterFx: the tile shader)
	if (water) if (WaterFx *wf = scene->Water ())
		wf->Feed (hp, ((uint64_t)id << 16) | idx, hit, std::max (1.0, pd.exitRadius + 0.25 * t), sqrt (strength), thrust * strength, WaterFx::Kind::Jet);
	if (!sys || !sys->enabled) { r.havePrev = false; mist[idx].havePrev = false; return; }
	if (hot > 0.0 && !water)
		pr->Scorch (hp, hit, n, std::max (1.0, 0.12 * reach) * (0.7 + 0.3 * hot), 0.08 * hot * hot * dt);
	if (kind == 1) {
		mist[idx].havePrev = false;
		// launch pads: the sound suppression water flashes into steam under
		// large engines, billowing out and rising in great white clouds
		if (thrust < 5e5 || pd.ambientPressure < 5e4) { r.havePrev = false; mist[idx].havePrev = false; return; }
		double life = 25.0;
		double D = sys->density, sizeScale = std::pow (D, -1.0 / 3.0);
		double rate = 60.0 * D * std::min (1.0, strength * 2.0); // [1/s] (the ring: for the rate emitted)
		if (!sys->Reserve (r, rate, life, ParticleSystem::Class::Steam)) { r.havePrev = false; mist[idx].havePrev = false; return; } // (no stale source next time)
		VECTOR3 vel;
		vessel->GetGlobalVel (vel);
		VECTOR3 src = hit + n * 2.0;
		if (!r.havePrev) r.prev = src;
		r.accum += rate * dt;
		uint32_t cnt = (uint32_t)r.accum;
		r.accum -= cnt;
		if (cnt) {
			ParticleSystem::Emission e{};
			e.p0 = r.prev, e.p1 = src;
			e.vel = vel, e.dir = -n;
			e.v0 = 6.0, e.spread = 70.0;
			e.planar = true;
			e.lifetime = life, e.dt = dt;
			e.size = 12.0 * sizeScale, e.growth = 3.5 * sizeScale;
			e.alpha = 0.5 * std::min (1.0, 1.3 / (D * sizeScale * sizeScale));
			e.slowdown = 0.6;
			e.tinted = true; // (hot: rises)
			e.volumetric = true;
			e.albedo[0] = e.albedo[1] = e.albedo[2] = 0.97f;
			e.cls = ParticleSystem::Class::Steam;
			sys->Emit (r, e, cnt);
		}
		r.prev = src;
		r.havePrev = true;
		return;
	}
	// water: spray instead of dust
	bool air = pd.ambientPressure > 100.0;

	double life = air ? (water ? 2.2 : 6.0) : 2.5; // (a sheet from hovering lingered half a minute at 10 s)
	double D = sys->density, sizeScale = std::pow (D, -1.0 / 3.0);
	double rate = water ? 420.0 * D * std::min (1.0, 0.3 + strength) : (air ? 120.0 : 900.0) * D * std::min (1.0, 0.3 + strength); // [1/s] (spray: many fine droplets, W1)
	ParticleSystem::Class cls = water ? ParticleSystem::Class::Spray : ParticleSystem::Class::Dust;
	if (!sys->Reserve (r, rate, life, cls)) { r.havePrev = false; mist[idx].havePrev = false; return; } // (no stale source next time)
	double radial = air ? 6.0 + 18.0 * strength : 15.0 + 40.0 * strength; // [m/s]
	if (water && air) radial = 8.0 + 22.0 * strength; // (droplets torn off by the jet: thrown out a little faster than dust)
	VECTOR3 vel;
	vessel->GetGlobalVel (vel);
	VECTOR3 src = hit + n * (air ? 0.3 : 0.8); // (in vacuum: clear of the ground, the sheet's particles start a metre across)
	if (!r.havePrev) r.prev = src;
	r.accum += rate * dt;
	uint32_t cnt = (uint32_t)r.accum;
	r.accum -= cnt;
	if (cnt) {
		ParticleSystem::Emission e{};
		e.p0 = r.prev, e.p1 = src;
		// (thrown up from the ground, at rest with it - not with the vessel:
		// climbing away or settling, its velocity would lift the dust or
		// slam it down)
		e.vel = GroundVelocity (hp, src);
		// (spreading along the local horizontal, not the hit's normal: a
		// tilted normal - a crater's wall, the terrain's detail - tilted the
		// whole sheet and threw dust tens of metres up in low gravity, f5
		// moon_t12)
		{
			VECTOR3 pc;
			oapiGetGlobalPos (hp, &pc);
			VECTOR3 vert = unit (src - pc);
			e.dir = -(air ? unit (n + vert * 2.0) : vert);
		}
		if (water) e.v0 = 0.2 * radial, e.spread = 1.8 * radial; // (droplets torn off the surface, thrown out low)
		else e.v0 = air ? 0.12 * radial : 2.0, e.spread = air ? 2.0 * radial : radial; // (low angles; in vacuum they would arc far)
		e.planar = true;
		e.lifetime = life, e.dt = dt;
		// (in air: lit puffs drawn as sprites (dust is never in the smoke
		// volume, which summed many faint ones into a cloud): denser and a
		// little larger, so that the cloud behind a hovering vessel shows)
		double size = air ? 5.0 : 3.0;
		e.size = size * sizeScale, e.growth = (air ? 4.0 : 1.0) * sizeScale;
		e.alpha = (air ? 1.0 : 0.9) * strength * std::min (1.0, 1.3 / (D * sizeScale * sizeScale));
		e.slowdown = air ? 2.0 : 0.0;
		if (water) { // (the sheet of spray: small, quick droplets falling back)
			// (pass 2, W1: a fine, translucent sheet of droplet clusters that stay small - 2.5 m sprites
			// growing to 9 m read as cotton balls of dust - many more of them, so the sheet is as dense)
			e.size = 0.9 * sizeScale, e.growth = 0.7 * sizeScale;
			e.alpha = 0.5 * strength * std::min (1.0, 1.3 / (D * sizeScale * sizeScale));
			e.slowdown = 1.0;
		}
		e.tinted = true, e.cold = true, e.ballistic = true;
		e.volumetric = false; // (ParticleSystem::Queue: dust is sprites)
		if (water) e.albedo[0] = 0.88f, e.albedo[1] = 0.91f, e.albedo[2] = 0.95f;
		else vksurf::DustColourGlobal (scene, hp, hit, e.albedo, &sp);
		// (in vacuum: a dense, low sheet racing out along the ground, the
		// particles streaking along their paths (gpuparticle.vert) and landing
		// within a second or two. Larger grains lofted higher looked like
		// floating rocks against the black sky: left out)
		e.cls = cls;
		sys->Emit (r, e, cnt);
	}
	r.prev = src;
	r.havePrev = true;
	if (!water || !air) { mist[idx].havePrev = false; return; } // (the mist: over water in air only)
	// the mist: fine spray that hangs and drifts over the sheet
	SmokeRing &m = mist[idx];
	double mrate = 40.0 * D * std::min (1.0, 0.3 + strength); // [1/s]
	if (!sys->Reserve (m, mrate, 3.0, ParticleSystem::Class::Spray)) { m.havePrev = false; return; }
	if (!m.havePrev) m.prev = src;
	m.accum += mrate * dt;
	uint32_t mc = (uint32_t)m.accum;
	m.accum -= mc;
	if (mc) {
		ParticleSystem::Emission e{};
		e.p0 = m.prev, e.p1 = src;
		e.vel = GroundVelocity (hp, src);
		VECTOR3 pc;
		oapiGetGlobalPos (hp, &pc);
		e.dir = -unit (n + unit (src - pc) * 2.0);
		e.v0 = 0.15 * radial, e.spread = 1.4 * radial;
		e.planar = true;
		e.lifetime = 3.0, e.dt = dt;
		e.size = 3.0 * sizeScale, e.growth = 2.5 * sizeScale;
		e.alpha = 0.5 * strength * std::min (1.0, 1.3 / (D * sizeScale * sizeScale));
		e.slowdown = 2.5;
		e.tinted = true, e.cold = true, e.ballistic = false;
		e.volumetric = false;
		e.albedo[0] = 0.92f, e.albedo[1] = 0.94f, e.albedo[2] = 0.97f;
		e.cls = ParticleSystem::Class::Spray;
		sys->Emit (m, e, mc);
	}
	m.prev = src;
	m.havePrev = true;
}

void VVessel::DrawParticles ()
{
	ParticleSystem *sys = scene->Particles ();
	if (!sys) return;
	double simt = oapiGetSimTime ();
	for (auto *rings : { &smoke, &dust, &mist, &trail })
		for (const SmokeRing &r : *rings)
			if (r.sys == sys && simt - r.lastEmit < 1.5 * r.life + 0.1)
				sys->Draw (r, sys->SmokeTexture (), false, scene->FrameSet ()); // (if not released while idle)
	for (const auto &c : contrails) c.Draw (sys, scene->FrameSet ());
	water.Draw (scene);
	if (wake.sys == sys) sys->Draw (wake, scene->FireballTexture (5), true, scene->FrameSet ()); // (EmitWake)
	if (afterglow.sys == sys) sys->Draw (afterglow, scene->FireballTexture (3), true, scene->FrameSet ());
}

void VVessel::RenderExhaust ()
{
	// (plume volumes: Scene::Render, after the transparent pass)
	if (!plumePoints.empty ()) scene->DrawBillboards (scene->GlowTex (), true, plumePoints.data (), plumePoints.size ());
	// flame billboards: exhaust textures hold the flame strip in u = [0, 1/4]
	// (nozzle at v = 0) and a glow sprite in [1/2, 1] x [0, 1/2]
	DWORD n = vessel->GetExhaustCount ();
	static std::mt19937 rng (12345);
	std::uniform_real_distribution<double> uni (0.0, 1.0);
	std::map<VkTexture*, std::vector<PVtx>> batches;
	for (DWORD i = 0; i < n; i++) {
		if (i < plumeExhaust.size() && plumeExhaust[i]) continue;
		double lvl = vessel->GetExhaustLevel (i);
		if (lvl <= 0.0) continue;
		EXHAUSTSPEC es;
		if (!vessel->GetExhaustSpec (i, &es) || !es.lpos || !es.ldir) continue;
		VECTOR3 gd = mul (grot, *es.ldir);
		VECTOR3 s0 = gpos + mul (grot, *es.lpos) - gd * es.lofs;
		VECTOR3 s1 = s0 - gd * es.lsize;
		float a = (float)(std::min (1.0, lvl) * (1.0 - es.modulate * uni (rng)));
		VECTOR3 vs = scene->ViewPos (s0), ve = scene->ViewPos (s1);
		VECTOR3 axis = ve - vs, mid = (vs + ve) * 0.5;
		VECTOR3 perp = crossp (axis, mid);
		double len = length (perp);
		if (len < 1e-9) continue; // looking straight along the flame
		perp *= 0.5 * es.wsize / len;
		VkTexture *tex = es.tex ? (VkTexture*)es.tex : scene->DefaultExhaustTex ();
		auto &v = batches[tex];
		auto P = [&](const VECTOR3 &p, float u, float t) {
			return PVtx{ (float)p.x, (float)p.y, (float)p.z, u, t, 1.0f, 1.0f, 1.0f, a };
		};
		PVtx q[4] = { P (vs + perp, 0.0f, 0.0f), P (vs - perp, 0.25f, 0.0f), P (ve - perp, 0.25f, 1.0f), P (ve + perp, 0.0f, 1.0f) };
		v.insert (v.end(), { q[0], q[1], q[2], q[0], q[2], q[3] });
		// glow at the nozzle, facing the camera
		float h = (float)es.wsize;
		float x = (float)vs.x, y = (float)vs.y, z = (float)vs.z;
		PVtx g[4] = {
			{ x - h, y - h, z, 0.5f, 0.0f, 1, 1, 1, a }, { x + h, y - h, z, 1.0f, 0.0f, 1, 1, 1, a },
			{ x + h, y + h, z, 1.0f, 0.5f, 1, 1, 1, a }, { x - h, y + h, z, 0.5f, 0.5f, 1, 1, 1, a } };
		v.insert (v.end(), { g[0], g[1], g[2], g[0], g[2], g[3] });
	}
	// beacons: blinking glows
	double simt = oapiGetSimTime ();
	for (DWORD i = 0; ; i++) {
		const BEACONLIGHTSPEC *bs = vessel->GetBeacon (i);
		if (!bs) break;
		if (!BeaconLit (bs, simt)) continue;
		VECTOR3 vp = scene->ViewPos (gpos + mul (grot, *bs->pos));
		// keep a minimum apparent size, so that beacons stay visible from afar
		float h = (float)std::max (bs->size, vp.z * 0.002);
		float x = (float)vp.x, y = (float)vp.y, z = (float)vp.z;
		// beacons are much brighter than lit surfaces (they bloom in HDR)
		float r = (float)bs->col->x * BEACON_GLOW, g = (float)bs->col->y * BEACON_GLOW, b = (float)bs->col->z * BEACON_GLOW;
		PVtx q[4] = {
			{ x - h, y - h, z, 0.0f, 0.0f, r, g, b, 1 }, { x + h, y - h, z, 1.0f, 0.0f, r, g, b, 1 },
			{ x + h, y + h, z, 1.0f, 1.0f, r, g, b, 1 }, { x - h, y + h, z, 0.0f, 1.0f, r, g, b, 1 } };
		auto &v = batches[scene->GlowTex ()];
		v.insert (v.end(), { q[0], q[1], q[2], q[0], q[2], q[3] });
	}
	for (auto &b : batches) scene->DrawBillboards (b.first, true, b.second.data(), b.second.size());
}

// ======================================================================
// VPlanet: lit sphere (textured planets follow with the tile manager)

namespace {
	struct BodyColour { const char *name; float r, g, b; };
	const BodyColour bodyColours[] = {
		{ "Sun",     1.00f, 0.95f, 0.85f },
		{ "Mercury", 0.55f, 0.52f, 0.50f },
		{ "Venus",   0.90f, 0.82f, 0.62f },
		{ "Earth",   0.30f, 0.45f, 0.75f },
		{ "Moon",    0.55f, 0.55f, 0.53f },
		{ "Mars",    0.75f, 0.42f, 0.25f },
		{ "Jupiter", 0.80f, 0.70f, 0.55f },
		{ "Saturn",  0.85f, 0.78f, 0.58f },
		{ "Uranus",  0.62f, 0.82f, 0.88f },
		{ "Neptune", 0.35f, 0.50f, 0.85f },
	};

	// a gas giant's map, for its decks: the (linear) luminance below which
	// its belts lie and above which its zones (the quartiles), and its
	// white (the brightest cloud: the 99th percentile), within 60 degrees
	// of the equator, by area; from RGBA8 (sRGB) maps of w x h texels
	// from pole to pole (e.g. hemispheres)
	void MapLevels (std::initializer_list<const uint8_t*> maps, uint32_t w, uint32_t h, float out[3])
	{
		auto lin = [](uint8_t v) { double c = v / 255.0; return c <= 0.04045 ? c / 12.92 : pow ((c + 0.055) / 1.055, 2.4); };
		double lut[256];
		for (int i = 0; i < 256; i++) lut[i] = lin ((uint8_t)i);
		std::vector<std::pair<float, float>> lw; // luminance, weight
		for (const uint8_t *m : maps)
			for (uint32_t y = 0; y < h; y += 2) {
				double lat = PI05 - PI * (y + 0.5) / h;
				if (fabs (lat) > 60.0 * RAD) continue;
				for (uint32_t x = 0; x < w; x += 2) {
					const uint8_t *p = m + ((size_t)y * w + x) * 4;
					lw.push_back ({ (float)(0.2126 * lut[p[0]] + 0.7152 * lut[p[1]] + 0.0722 * lut[p[2]]), (float)cos (lat) });
				}
			}
		if (lw.empty()) return;
		std::sort (lw.begin(), lw.end());
		double sum = 0.0;
		for (auto &e : lw) sum += e.second;
		const double q[3] = { 0.25, 0.75, 0.99 };
		double acc = 0.0;
		size_t i = 0;
		for (int k = 0; k < 3; k++) {
			for (; i < lw.size() - 1 && acc + lw[i].second < q[k] * sum; i++) acc += lw[i].second;
			out[k] = lw[i].first;
		}
		// (a nearly uniform map: no bands to speak of)
		float mid = 0.5f * (out[0] + out[1]);
		out[0] = std::min (out[0], mid - 0.05f), out[1] = std::max (out[1], mid + 0.05f);
	}
}

VPlanet::VPlanet (OBJHANDLE h, Scene *s, bool star)
	: VObject(h, s), isStar(star)
{
	char name[256];
	oapiGetObjectName (hObj, name, 256);
	colour[0] = colour[1] = colour[2] = 0.5f;
	colour[3] = 1.0f;
	auto iequal = [](const char *a, const char *b) {
		for (; *a && *b; a++, b++)
			if (tolower ((unsigned char)*a) != tolower ((unsigned char)*b)) return false;
		return *a == *b;
	};
	for (auto &bc : bodyColours)
		if (iequal (bc.name, name)) {
			colour[0] = bc.r, colour[1] = bc.g, colour[2] = bc.b;
			break;
		}
	if (!isStar) {
		atmosphere = std::make_unique<Atmosphere> (hObj, scene);
		surface = PlanetSurface::Create (hObj, scene);
		if (surface) clouds = PlanetSurface::Create (hObj, scene, true);
		atmosphere->SetGround (surface ? surface->RootTexture (0) : nullptr, surface ? surface->RootTexture (1) : nullptr, colour);
		if (clouds) {
			const double *alt = (const double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_CLOUDALT);
			atmosphere->SetClouds (clouds->RootTexture (0), clouds->RootTexture (1), alt ? *alt : 0.0);
		}
		LoadLegacyTextures ();
	}
}

VPlanet::~VPlanet ()
{
	TextureManager *tm = scene->Client()->TexMgr();
	tm->Release (globalTex);
	for (auto *t : groundTex) if (t) tm->Release (t);
	tm->Release (ringTex);
	tm->Release (cloudTex);
	if (ringNIdx) {
		vkc::Context *c = scene->Client()->Ctx();
		vkc::Buffer vb = ringVtx, ib = ringIdx;
		c->DeferDestroy ([c, vb, ib]() mutable { c->DestroyBuffer (vb); c->DestroyBuffer (ib); });
	}
}

vksurf::TileProbe *VPlanet::MaskProbe ()
{
	if (!maskProbeInit) {
		maskProbeInit = true; // (once: the ground's layers, never the clouds')
		if (surface && surface->Layers () && surface->Layers ()->mask) {
			char nm[64];
			oapiGetObjectName (hObj, nm, 64);
			maskProbe = std::make_unique<vksurf::TileProbe> (surface->Layers (), vksurf::TileProbe::Layer::Mask, 16, nm);
		}
	}
	return maskProbe.get ();
}

vksurf::TileProbe *VPlanet::SurfProbe ()
{
	if (!surfProbeInit) {
		surfProbeInit = true; // (once: the ground's layers, never the clouds')
		if (surface && surface->Layers () && surface->Layers ()->surf) {
			char nm[64];
			oapiGetObjectName (hObj, nm, 64);
			surfProbe = std::make_unique<vksurf::TileProbe> (surface->Layers (), vksurf::TileProbe::Layer::Surf, 16, nm);
		}
	}
	return surfProbe.get ();
}

void VPlanet::LoadLegacyTextures ()
{
	char name[256], path[512];
	oapiGetObjectName (hObj, name, 256);
	// .tex files live in the Textures directory, or in the planet texture directory
	std::vector<std::string> dirs = { "Textures/" };
	if (scene->Client()->PlanetTexturePath (name, path)) {
		std::string d (path);
		std::replace (d.begin(), d.end(), '\\', '/');
		size_t sl = d.find_last_of ('/');
		if (sl != std::string::npos) dirs.push_back (d.substr (0, sl + 1));
	}
	auto load = [&](const std::string &file, std::vector<std::vector<uint8_t>> &images) {
		for (auto &d : dirs)
			if (legacytex::Read (d + file, images)) return true;
		return false;
	};
	TextureManager *tm = scene->Client()->TexMgr();
	std::vector<std::vector<uint8_t>> images;
	uint32_t w, h;
	std::vector<uint8_t> rgba;

	if (!surface && load (std::string (name) + ".tex", images) && legacytex::GlobalMap (images, w, h, rgba)) {
		globalTex = tm->CreateRGBA (w, h, rgba.data());
		// its hemispheres, for the atmosphere (reflections, the cloud decks' bands)
		if (atmosphere && w >= 2) {
			uint32_t hw = w / 2;
			std::vector<uint8_t> half ((size_t)hw * h * 4);
			for (int k = 0; k < 2; k++) {
				for (uint32_t y = 0; y < h; y++)
					memcpy (half.data() + (size_t)y * hw * 4, rgba.data() + ((size_t)y * w + k * hw) * 4, (size_t)hw * 4);
				groundTex[k] = tm->CreateRGBA (hw, h, half.data());
			}
			atmosphere->SetGround (groundTex[0], groundTex[1], colour);
		}
	}
	// a procedural gas giant (the exoplanets): its map of belts and zones
	if (!surface && !globalTex) {
		auto proc = ProcPlanet::Create (hObj);
		if (proc && proc->Gas ()) {
			w = 2048, h = 1024;
			proc->GasMap ((int)w, (int)h, rgba);
			globalTex = tm->CreateRGBA (w, h, rgba.data());
			if (atmosphere) {
				uint32_t hw = w / 2;
				std::vector<uint8_t> half ((size_t)hw * h * 4);
				for (int k = 0; k < 2; k++) {
					for (uint32_t y = 0; y < h; y++)
						memcpy (half.data() + (size_t)y * hw * 4, rgba.data() + ((size_t)y * w + k * hw) * 4, (size_t)hw * 4);
					groundTex[k] = tm->CreateRGBA (hw, h, half.data());
				}
				atmosphere->SetGround (groundTex[0], groundTex[1], colour);
			}
			oapiWriteLogV ("VulkanClient: procedural gas giant map for %s", name);
		}
	}
	if (!oapiPlanetHasSurface (hObj)) hasDecks = VolumetricClouds::LoadDecks (name, decks);
	if (hasDecks && globalTex) MapLevels ({ rgba.data() }, w, h, decks.mapLevels);
	// a gas giant's decks take their colours from its map: the finest
	// complete level of its surface tiles (up to 6: 4096 x 2048 texels)
	// instead of the level-4 roots, assembled into the two hemispheres
	if (hasDecks && surface && atmosphere && scene->Client()->PlanetTexturePath (name, path)) {
		std::string d (path);
		std::replace (d.begin(), d.end(), '\\', '/');
		while (!d.empty() && d.back() == '/') d.pop_back();
		TileLayer layer (d, TileLayer::SURF);
		std::vector<uint8_t> dds, px;
		int lvl = 6;
		for (; lvl > 4; lvl--) {
			bool all = true;
			for (int a = 0; a < (1 << (lvl - 4)) && all; a++)
				for (int b = 0; b < (1 << (lvl - 3)) && all; b++) all = layer.HasTile (lvl, a, b);
			if (all) break;
		}
		const int nlat = 1 << (lvl - 4), nlng = 1 << (lvl - 3);
		uint32_t T = 0, hw = 0;
		std::vector<uint8_t> half[2];
		bool ok = lvl > 4;
		for (int a = 0; a < nlat && ok; a++)
			for (int b = 0; b < nlng && ok; b++) {
				uint32_t tw, th;
				ok = layer.Read (lvl, a, b, dds) && legacytex::Decode (dds, tw, th, px) && tw == th && (!T || tw == T);
				if (!ok) break;
				if (!T) {
					T = tw, hw = T * nlng / 2;
					for (auto &hm : half) hm.resize ((size_t)hw * T * nlat * 4);
				}
				int k = b / (nlng / 2), x0 = (b % (nlng / 2)) * T;
				for (uint32_t y = 0; y < T; y++)
					memcpy (half[k].data() + (((size_t)(a * T + y) * hw) + x0) * 4, px.data() + (size_t)y * T * 4, (size_t)T * 4);
			}
		if (ok) {
			for (int k = 0; k < 2; k++) groundTex[k] = tm->CreateRGBA (hw, T * nlat, half[k].data());
			MapLevels ({ half[0].data(), half[1].data() }, hw, T * nlat, decks.mapLevels);
			atmosphere->SetGround (groundTex[0], groundTex[1], colour);
			oapiWriteLogV ("VulkanClient: %s's decks coloured from its level-%d map (%u x %u; belts below %.3f, zones above %.3f, white %.3f)",
				name, lvl, 2 * hw, T * nlat, decks.mapLevels[0], decks.mapLevels[1], decks.mapLevels[2]);
		}
	}

	const bool *hasClouds = (const bool*)oapiGetObjectParam (hObj, OBJPRM_PLANET_HASCLOUDS);
	if (!clouds && hasClouds && *hasClouds && load (std::string (name) + "_cloud.tex", images) &&
		legacytex::GlobalMap (images, w, h, rgba)) {
		cloudTex = tm->CreateRGBA (w, h, rgba.data());
		const double *alt = (const double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_CLOUDALT);
		cloudAlt = alt ? *alt : 0.0;
	}

	const bool *hasRings = (const bool*)oapiGetObjectParam (hObj, OBJPRM_PLANET_HASRINGS);
	if (hasRings && *hasRings) {
		// a radial profile (<name>_ring_<size>.dds: one row of 32-bit pixels
		// from the inner to the outer radius), else the legacy texture
		for (int size : { 8192, 4096, 2048 }) {
			for (auto &d : dirs) {
				std::ifstream f (d + name + "_ring_" + std::to_string (size) + ".dds", std::ios::binary);
				std::vector<uint8_t> dds ((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
				if (dds.size() < 128 + 4 * (size_t)size || memcmp (dds.data(), "DDS ", 4)) continue;
				uint32_t hw[2], pf[8];
				memcpy (hw, dds.data() + 12, 8);
				memcpy (pf, dds.data() + 76, 32);
				// (uncompressed 32-bit, B8G8R8A8)
				if (hw[1] != (uint32_t)size || (pf[1] & 0x4) || pf[3] != 32 || pf[4] != 0xff0000) continue;
				rgba.resize (4 * (size_t)size);
				for (int i = 0; i < size; i++) {
					const uint8_t *s = dds.data() + 128 + 4 * i;
					rgba[4*i] = s[2], rgba[4*i+1] = s[1], rgba[4*i+2] = s[0], rgba[4*i+3] = 255;
				}
				ringTex = tm->CreateRGBA (size, 1, rgba.data());
				break;
			}
			if (ringTex) break;
		}
		ringPrm[3] = 0.0f;
		if (!ringTex && load (std::string (name) + "_ring.tex", images) && legacytex::Decode (images.back(), w, h, rgba)) {
			ringTex = tm->CreateRGBA (w, h, rgba.data());
			ringPrm[3] = 1.0f;
		}
		if (ringTex)
			BuildRing (*(const double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_RINGMINRAD),
			           *(const double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_RINGMAXRAD));
	}
}

void VPlanet::BuildRing (double irad, double orad)
{
	// Flat annulus in the equatorial plane (units of the planet radius). The
	// ring texture is laid out for a ring of 16 straight sections: a radial
	// line runs through it from (fo,0) at the outer section corner to (fi,1)
	// at the inner radius. Every radial line of the annulus samples it that
	// way, which keeps the bands circular.
	const double alpha = PI / 16.0;
	double nrad = orad / cos (alpha);
	double fo = 0.5 * (1.0 - orad / nrad), fi = 0.5 * (1.0 - irad / nrad);
	double t0 = (nrad - orad) / (nrad - irad);
	ringPrm[0] = (float)irad, ringPrm[1] = (float)orad, ringPrm[2] = 1.0f;
	ringLegacy[0] = (float)fo, ringLegacy[1] = (float)fi, ringLegacy[2] = (float)nrad;
	const int N = 512;
	std::vector<NTVERTEX> vtx;
	std::vector<uint32_t> idx;
	for (int i = 0; i <= N; i++) {
		double phi = PI2 * i / N, c = cos (phi), s = sin (phi);
		NTVERTEX vo{}, vi{};
		vo.x = (float)(orad * c), vo.z = (float)(orad * s);
		vi.x = (float)(irad * c), vi.z = (float)(irad * s);
		vo.ny = vi.ny = 1.0f;
		vo.tu = (float)(fo + (fi - fo) * t0), vo.tv = (float)t0;
		vi.tu = (float)fi, vi.tv = 1.0f;
		vtx.push_back (vo), vtx.push_back (vi);
	}
	for (int i = 0; i < N; i++) {
		uint32_t a = 2 * i, b = a + 1, c = a + 2, d = a + 3;
		idx.insert (idx.end(), { a, b, c, b, d, c });  // seen from above
		idx.insert (idx.end(), { a, c, b, b, c, d });  // seen from below
	}
	vkc::Context *ctx = scene->Client()->Ctx();
	ringVtx = ctx->CreateDeviceBuffer (vtx.data(), vtx.size() * sizeof(NTVERTEX), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	ringIdx = ctx->CreateDeviceBuffer (idx.data(), idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	ringNIdx = (uint32_t)idx.size();
}

void VPlanet::RenderRing ()
{
	if (!ringNIdx || !ringTex) return;
	Mat4 mv = scene->ModelView (gpos, grot) * Mat4::Scale (size, size, size);
	scene->DrawRing (ringVtx.buffer, ringIdx.buffer, ringNIdx, mv, ringTex, ringPrm, ringLegacy, false);
}

// ======================================================================
// VBase

namespace {
	constexpr double BASE_VIS_RANGE = 300e3;  // bases closer than this are rendered [m]
	// Ground structures lie on the terrain; they are drawn pulled towards the
	// camera by this fraction of their distance (a uniform scale in view space
	// keeps their screen position), which avoids depth fighting.
	constexpr double GROUND_DEPTH_PULL = 1e-3;
	// a base light's glow (a glare, of the eye or the camera: a size on the
	// image, not in the world): its half size at least and at most these
	// fractions of the image's half height, its own size between (about 1.7
	// and 4.6 pixels in 1080 lines; at a 25 degree aperture what were the
	// angles 0.0015 and 0.004 rad). As angles they grew with the zoom: at a
	// 1 degree aperture runway lights 45 pixels across, a string of pearls.
	constexpr double LIGHT_GLOW_MIN_NDC = 0.0015 * 2.1445, LIGHT_GLOW_MAX_NDC = 0.004 * 2.1445;
	// a floodlight's light: its configured colour (Config/VK/Bases, which
	// also sets its glow's brightness: about 6 for a xenon flood) times this
	// is its irradiance [units of sunlight] up to a fifth of its range, the
	// inverse square beyond: 6 lights the pad 100 m away with ~2 % of
	// sunlight (~2000 lux, a floodlit stadium), not twice the sun
	constexpr double FLOOD_IRRADIANCE = 0.01;
}

VBase::VBase (OBJHANDLE h, Scene *s) : hBase(h), scene(s)
{
	VulkanClient *gc = scene->Client();
	MESHHANDLE *bs = nullptr, *as = nullptr;
	DWORD nbs = 0, nas = 0;
	gc->GetBaseStructures (hBase, &bs, &nbs, &as, &nas);
	for (DWORD i = 0; i < nbs; i++)
		if (bs[i]) ground.push_back (std::make_unique<VkMesh> (gc->Ctx(), gc->TexMgr(), bs[i]));
	for (DWORD i = 0; i < nas; i++)
		if (as[i]) structures.push_back (std::make_unique<VkMesh> (gc->Ctx(), gc->TexMgr(), as[i]));
	ReadLights ();
}

VBase::~VBase ()
{
}

namespace {
	std::string BaseLower (std::string t) { for (auto &c : t) c = (char)tolower ((unsigned char)c); return t; }
	std::string Trim (const std::string &t)
	{
		size_t a = t.find_first_not_of (" \t\r"), b = t.find_last_not_of (" \t\r");
		return a == std::string::npos ? std::string () : t.substr (a, b - a + 1);
	}
}

void VBase::ReadLights ()
{
	char name[256], pname[256];
	oapiGetObjectName (hBase, name, 256);
	oapiGetObjectName (oapiGetBasePlanet (hBase), pname, 256);
	// the base's configuration: the file whose Name matches
	std::error_code ec;
	std::string cfg;
	for (auto &e : std::filesystem::directory_iterator (std::string ("Config/") + pname + "/Base", ec)) {
		std::ifstream f (e.path ());
		std::string line;
		for (int i = 0; i < 10 && std::getline (f, line); i++) {
			auto eq = line.find ('=');
			if (eq != std::string::npos && BaseLower (Trim (line.substr (0, eq))) == "name" && Trim (line.substr (eq + 1)) == name) {
				cfg = e.path ().string ();
				break;
			}
		}
		if (!cfg.empty ()) break;
	}
	auto vec = [](std::istringstream &ss) { VECTOR3 v{}; ss >> v.x >> v.y >> v.z; return v; };
	if (!cfg.empty ()) {
		std::ifstream f (cfg);
		std::string line, block;
		VECTOR3 e1{}, e2{};
		double width = 0.0, size = 1.0;
		int count = 0;
		float col[3] = { 1, 1, 1 };
		while (std::getline (f, line)) {
			std::istringstream ss (line);
			std::string tag;
			if (!(ss >> tag)) continue;
			tag = BaseLower (tag);
			if (tag == "runwaylights" || tag == "beaconarray") {
				block = tag, width = 0.0, count = 0, size = 1.0, col[0] = col[1] = col[2] = 1.0f;
			} else if (block.empty ()) {
				continue;
			} else if (tag == "end1") e1 = vec (ss);
			else if (tag == "end2") e2 = vec (ss);
			else if (tag == "width") ss >> width;
			else if (tag == "count") ss >> count;
			else if (tag == "size") ss >> size;
			else if (tag == "col") ss >> col[0] >> col[1] >> col[2];
			else if (tag == "end") {
				VECTOR3 d = e2 - e1;
				double len = length (d);
				if (block == "runwaylights" && len > 0.0) {
					// edge lights every 60 m (white), threshold lights (green, red)
					VECTOR3 u = d / len, side = unit (crossp (u, _V(0, 1, 0))) * (0.5 * width + 2.0);
					for (double t = 0.0; t <= len; t += 60.0)
						for (int k = -1; k <= 1; k += 2)
							lights.push_back ({ e1 + u * t + side * k + _V(0, 6, 0), { 1.0f, 0.95f, 0.8f }, 1.2f });
					for (int k = -8; k <= 8; k++) {
						VECTOR3 a = side * (k / 8.0);
						lights.push_back ({ e1 + a + _V(0, 6, 0), { 0.2f, 1.0f, 0.3f }, 1.2f });
						lights.push_back ({ e2 + a + _V(0, 6, 0), { 1.0f, 0.15f, 0.1f }, 1.2f });
					}
				} else if (block == "beaconarray" && count > 0) {
					for (int i = 0; i < count; i++)
						lights.push_back ({ e1 + d * (count > 1 ? (double)i / (count - 1) : 0.0) + _V(0, 6, 0), { col[0], col[1], col[2] }, (float)size });
				}
				block.clear ();
			}
		}
	}
	// floodlights (Vulkan client additions): FLOODLIGHT x y z dx dy dz range r g b umbra penumbra [deg]
	std::string vk = std::string ("Config/VK/Bases/") + name + ".cfg";
	for (auto &c : vk) if (c == ' ') c = '_';
	std::ifstream f (vk);
	std::string line;
	while (std::getline (f, line)) {
		if (auto c = line.find (';'); c != std::string::npos) line.resize (c);
		std::istringstream ss (line);
		std::string tag;
		if (!(ss >> tag) || BaseLower (tag) != "floodlight") continue;
		Flood fl{};
		fl.pos = vec (ss), fl.dir = vec (ss);
		ss >> fl.range >> fl.col[0] >> fl.col[1] >> fl.col[2] >> fl.umbra >> fl.penumbra;
		if (!ss) continue;
		fl.dir = unit (fl.dir), fl.umbra *= RAD, fl.penumbra *= RAD;
		floods.push_back (fl);
	}
	if (!lights.empty () || !floods.empty ())
		oapiWriteLogV ("VulkanClient: base %s: %zu lights, %zu floodlights", name, lights.size (), floods.size ());
}

bool VBase::Night () const
{
	// lights on when the sun is below a few degrees above the horizon
	OBJHANDLE hSun = scene->Sun ();
	if (!hSun) return false;
	VECTOR3 gpos, sp, pp;
	oapiGetGlobalPos (hBase, &gpos);
	oapiGetGlobalPos (hSun, &sp);
	oapiGetGlobalPos (oapiGetBasePlanet (hBase), &pp);
	return dotp (unit (gpos - pp), unit (sp - gpos)) < 0.05;
}

void VBase::CollectLights (std::vector<LightSource> &out)
{
	if (floods.empty () || !Night ()) return;
	VECTOR3 gpos, cam = scene->CameraPos ();
	MATRIX3 grot;
	scene->ObservedPos (hBase, gpos);
	oapiGetRotationMatrix (hBase, &grot);
	for (const Flood &fl : floods) {
		LightSource l{};
		l.pos = gpos + mul (grot, fl.pos);
		if (length (l.pos - cam) > fl.range + 10e3) continue;
		l.dir = mul (grot, fl.dir);
		l.spot = true;
		l.range = fl.range;
		l.att[0] = 1.0, l.att[1] = 0.0, l.att[2] = 1.0 / (0.04 * fl.range * fl.range);
		l.umbra = fl.umbra, l.penumbra = fl.penumbra;
		for (int k = 0; k < 3; k++) l.col[k] = (float)(FLOOD_IRRADIANCE * fl.col[k]);
		out.push_back (l);
	}
}

void VBase::Render (RenderPass pass)
{
	VECTOR3 gpos;
	MATRIX3 grot;
	scene->ObservedPos (hBase, gpos);
	oapiGetRotationMatrix (hBase, &grot);
	Mat4 mv = scene->ModelView (gpos, grot);
	scene->Sunlight (gpos);
	Mat4 pulled = Mat4::Scale (1.0 - GROUND_DEPTH_PULL, 1.0 - GROUND_DEPTH_PULL, 1.0 - GROUND_DEPTH_PULL) * mv;
	for (auto &m : ground) scene->DrawMesh (m.get(), pulled, pass);
	for (auto &m : structures) scene->DrawMesh (m.get(), mv, pass);
	// lights at night: glows (floodlights larger, and brighter towards their beam)
	if (pass == RenderPass::Transparent && (!lights.empty () || !floods.empty ()) && Night ()) {
		std::vector<PVtx> v;
		auto glow = [&](const VECTOR3 &bp, const float *c, double size, float bright) {
			VECTOR3 vp = scene->ViewPos (gpos + mul (grot, bp));
			if (vp.z <= 0.0) return;
			// (a few pixels at least; near, at most a few more: a glare, not a
			// ball of fog metres across in front of the lamp; the same on the
			// image at any zoom)
			double fy = std::max (std::fabs (scene->ProjY ()), 1e-3);
			float h = (float)std::clamp (size, vp.z * LIGHT_GLOW_MIN_NDC / fy, vp.z * LIGHT_GLOW_MAX_NDC / fy);
			float x = (float)vp.x, y = (float)vp.y, z = (float)vp.z;
			float r = c[0] * bright, g = c[1] * bright, b = c[2] * bright;
			PVtx q[4] = {
				{ x - h, y - h, z, 0.0f, 0.0f, r, g, b, 1 }, { x + h, y - h, z, 1.0f, 0.0f, r, g, b, 1 },
				{ x + h, y + h, z, 1.0f, 1.0f, r, g, b, 1 }, { x - h, y + h, z, 0.0f, 1.0f, r, g, b, 1 } };
			v.insert (v.end(), { q[0], q[1], q[2], q[0], q[2], q[3] });
		};
		for (const Light &l : lights) glow (l.pos, l.col, l.size, 4.0f);
		VECTOR3 cam = scene->CameraPos ();
		for (const Flood &fl : floods) {
			VECTOR3 toCam = unit (cam - (gpos + mul (grot, fl.pos)));
			double facing = std::max (0.0, dotp (toCam, mul (grot, fl.dir)));
			glow (fl.pos, fl.col, 3.0 + 6.0 * facing, (float)(2.0 + 10.0 * facing));
		}
		scene->DrawBillboards (scene->GlowTex (), true, v.data (), v.size ());
	}
}

bool VBase::RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n)
{
	VECTOR3 gpos;
	MATRIX3 grot;
	scene->ObservedPos (hBase, gpos);
	oapiGetRotationMatrix (hBase, &grot);
	auto get = [](const std::unique_ptr<VkMesh> &p, const VkMesh *&m, VECTOR3 &ofs) { m = p.get(), ofs = _V(0, 0, 0); return true; };
	double t1, t2;
	VECTOR3 n1, n2;
	bool h1 = MeshesRayHit (ground, get, gpos, grot, o, d, maxDist, t1, n1);
	bool h2 = MeshesRayHit (structures, get, gpos, grot, o, d, h1 ? t1 : maxDist, t2, n2);
	if (h2) t = t2, n = n2;
	else if (h1) t = t1, n = n1;
	return h1 || h2;
}

void VBase::RenderShadow (ShadowMap &sm)
{
	VECTOR3 gpos;
	MATRIX3 grot;
	scene->ObservedPos (hBase, gpos);
	oapiGetRotationMatrix (hBase, &grot);
	Mat4 model = Mat4::FromRotTrans (grot, gpos - sm.Centre());
	for (auto &m : structures) sm.DrawMesh (m.get(), model);
}

// ======================================================================

void VPlanet::RenderShadow (ShadowMap &sm)
{
	// terrain casts only into maps near the surface
	if (surface && camDist < size + 50e3) surface->RenderShadow (sm);
	for (auto &b : bases) b->RenderShadow (sm);
	if (gallery) gallery->RenderShadow (sm);
	if (field) field->RenderShadow (sm);
	if (cities) cities->RenderShadow (sm);
}

void VPlanet::UpdateBases ()
{
	DWORD n = oapiGetBaseCount (hObj);
	for (DWORD i = 0; i < n; i++) {
		OBJHANDLE hb = oapiGetBaseByIndex (hObj, i);
		VECTOR3 bp;
		oapiGetGlobalPos (hb, &bp);
		double d = length (bp - scene->CameraPos());
		auto it = std::find_if (bases.begin(), bases.end(), [hb](const auto &b) { return b->Handle() == hb; });
		bool have = it != bases.end();
		if (!have && d < BASE_VIS_RANGE)
			bases.push_back (std::make_unique<VBase> (hb, scene));
		else if (have && d > BASE_VIS_RANGE * 1.2)
			bases.erase (it);
	}
}

void VPlanet::Update ()
{
	VObject::Update ();
	if (!isStar) UpdateBases ();
	if (surface) surface->Update ();
	if (clouds) clouds->Update ();
}

void VPlanet::RenderSky ()
{
	if (!atmosphere) return;
	atmosphere->Update ();
	atmosphere->RenderSky ();
}

bool VPlanet::RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n)
{
	bool found = false;
	t = maxDist;
	for (auto &b : bases) {
		double tb;
		VECTOR3 nb;
		if (b->RayHit (o, d, t, tb, nb)) t = tb, n = nb, found = true, lastHitKind = 1;
	}
	// the terrain: none where the segment passes clear of the highest the
	// ground could be (the elevations are costly: procedural detail at the
	// finest level; plumes cast their rays every frame)
	if (terrainTop < 0.0) {
		const double *me = (const double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_MAXELEVATION);
		terrainTop = std::max ((me ? std::max (0.0, *me) : 0.0) * 1.1 + 1000.0, 0.01 * size);
	}
	{
		VECTOR3 c = gpos - o;
		double tc = std::clamp (dotp (c, d), 0.0, t);
		if (length (c - d * tc) > size + terrainTop) return found;
	}
	// the ray's altitude over the surface elevation
	auto alt = [this](const VECTOR3 &p) {
		VECTOR3 loc;
		oapiGlobalToLocal (hObj, &p, &loc);
		double rad = length (loc);
		double lng = atan2 (loc.z, loc.x), lat = asin (std::clamp (loc.y / rad, -1.0, 1.0));
		return rad - size - oapiSurfaceElevation (hObj, lng, lat);
	};
	double a0 = alt (o);
	if (a0 < 0.0 || a0 > t) return found; // (beneath, or out of reach)
	const int N = 8;
	double tPrev = 0.0;
	for (int i = 1; i <= N; i++) {
		double ti = t * i / N, ai = alt (o + d * ti);
		if (ai < 0.0) {
			double lo = tPrev, hi = ti;
			for (int k = 0; k < 10; k++) {
				double m = 0.5 * (lo + hi);
				(alt (o + d * m) < 0.0 ? hi : lo) = m;
			}
			t = 0.5 * (lo + hi);
			n = unit (o + d * t - gpos); // (the local vertical)
			lastHitKind = 0;
			return true;
		}
		tPrev = ti;
	}
	return found;
}

void VPlanet::CollectLights (std::vector<LightSource> &lights)
{
	for (auto &b : bases) b->CollectLights (lights);
}

void VPlanet::PrepareFrame ()
{
	if (surface) surface->Prepare ();
	// the ground scatter: made when the camera comes near the surface
	if (surface && !fieldInit && camDist < size + 20e3) {
		fieldInit = true;
		field = std::make_unique<vkscatter::Field> (scene, hObj);
		if (!field->Active ()) field.reset ();
	}
	if (field) field->Prepare ();
	// the cities: likewise
	if (surface && !citiesInit && camDist < size + 30e3) {
		citiesInit = true;
		cities = std::make_unique<vkcity::Field> (scene, hObj);
		if (!cities->Active ()) cities.reset ();
	}
	if (cities) cities->Prepare ();
	if (clouds) clouds->Prepare (); // (coverage map of the volumetric clouds)
}

double VPlanet::CloudAlt () const
{
	const double *alt = (const double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_CLOUDALT);
	return alt ? *alt : 0.0;
}

void VPlanet::UpdateGallery ()
{
	// the gallery is on the planet the focus vessel stands on
	if (gallery || !vkscatter::Gallery::Enabled ()) return;
	OBJHANDLE hv = oapiGetFocusObject ();
	if (hv && oapiGetVesselInterface (hv)->GetSurfaceRef () == hObj)
		gallery = std::make_unique<vkscatter::Gallery> (scene, hObj);
}

void VPlanet::Render (RenderPass pass)
{
	if (pass == RenderPass::Depth) {
		SetMotion (); // terrain and bases move with the planet
		// (a star: its disc, a sphere of its own, writes its depth; its tiles' would fight it)
		if (surface && !isStar && !DecksHide () && scene->PixelSize (size, camDist) >= 0.5) surface->RenderDepth ();
		for (auto &b : bases) b->Render (pass);
		UpdateGallery ();
		if (gallery) gallery->Render (pass);
		if (field) field->Render (pass);
		if (cities) cities->Render (pass);
		return;
	}
	if (isStar && !scene->SunAsBody ()) return; // (far: a point of the star field)
	if (isStar && hObj != scene->Sun ()) return; // (the other stars: drawn as near stars, Scene)
	if (pass == RenderPass::Transparent) {
		if (isStar) { RenderGlow (); return; }
		for (auto &b : bases) b->Render (pass);
		if (cities) cities->Render (pass); // (street lamps)
		// (a gas giant's 2D cloud layer: not while its decks are drawn; while
		// they fade in from afar, fading out over them (drawn after them: at
		// full opacity it hid their fade, and vanished at once where it ended))
		float deckOp = hasDecks && scene->CloudsActive () && oapiCameraProxyGbody () == hObj ? scene->VClouds ()->DeckOpacity () : 0.0f;
		if (clouds && deckOp < 1.0f) {
			clouds->layerAlpha = 1.0f - deckOp;
			clouds->Render (atmosphere.get());
		}
		if (scene->PixelSize (size, camDist) >= 0.5) {
			if (cloudTex) {
				// legacy cloud layer: longitude 0 of the map at the -x axis
				double r = size + cloudAlt;
				MATRIX3 R = mul (grot, _M(-1, 0, 0, 0, 1, 0, 0, 0, -1));
				const float white[4] = { 1, 1, 1, 1 };
				scene->DrawGeometry (scene->SphereVtx(), scene->SphereIdx(), scene->SphereNIdx(),
					scene->ModelView (gpos, R) * Mat4::Scale (r, r, r), cloudTex, white, false, true);
			}
			if (!RingsBehindDecks ()) RenderRing (); // (else before the decks: Scene)
		}
		return;
	}
	if (scene->PixelSize (size, camDist) < 0.5) return; // too small to see
	if (DecksHide ()) return; // (inside a gas giant's clouds: they are all there is)
	if (surface) {
		surface->Render (atmosphere.get());
		scene->GpuMark ("terrain");
		for (auto &b : bases) b->Render (pass);
		UpdateGallery ();
		if (gallery) gallery->Render (pass);
		if (field) field->Render (pass);
		scene->GpuMark ("scatter");
		if (cities) cities->Render (pass);
		scene->GpuMark ("cities");
		return;
	}

	// Until planetary surfaces are rendered from terrain tiles, a sphere stands
	// in for the planet. Close to the surface it is drawn with the local surface
	// radius below the camera, and with its pole (where the mesh vertices are
	// densest) pointing at the camera, so that the facets match the surface there.
	double radius = size;
	MATRIX3 R = grot;
	if (globalTex) {
		// whole-sphere map: longitude -180 (left edge) at the mesh's u = 0
		R = mul (grot, _M(-1, 0, 0, 0, 1, 0, 0, 0, -1));
		Mat4 mv = scene->ModelView (gpos, R) * Mat4::Scale (size, size, size);
		const float white[4] = { 1, 1, 1, 1 };
		scene->DrawSphere (mv, white, false, globalTex);
		if (ringNIdx && ringTex) // (the rings' shadow over it)
			scene->DrawRing (scene->SphereVtx(), scene->SphereIdx(), scene->SphereNIdx(), mv, ringTex, ringPrm, ringLegacy, true);
		return;
	}
	if (!isStar && camDist < 2.0 * size) {
		double lng, lat, rad;
		oapiGlobalToEqu (hObj, scene->CameraPos(), &lng, &lat, &rad);
		radius = size + oapiSurfaceElevation (hObj, lng, lat);
		VECTOR3 up = tmul (grot, scene->CameraPos() - gpos);
		up /= length (up);
		// rotation taking the mesh y axis to 'up'
		VECTOR3 ref = fabs (up.y) < 0.9 ? _V(0, 1, 0) : _V(1, 0, 0);
		VECTOR3 ex = crossp (ref, up);
		ex /= length (ex);
		VECTOR3 ez = crossp (ex, up);
		MATRIX3 Q = { ex.x, up.x, ez.x,
		              ex.y, up.y, ez.y,
		              ex.z, up.z, ez.z };
		R = mul (grot, Q);
	}
	Mat4 mv = scene->ModelView (gpos, R) * Mat4::Scale (radius, radius, radius);
	if (isStar) {
		// the sun's disc is far brighter than any lit surface: its photosphere
		// (star.frag: limb darkening, granulation, spots)
		float c[4] = { colour[0] * STAR_BRIGHTNESS, colour[1] * STAR_BRIGHTNESS, colour[2] * STAR_BRIGHTNESS, 1.0f };
		scene->RelShiftColour (gpos, c); // (relativistic view: not in the prepass, shifted here)
		scene->DrawStar (mv, c, 5772.0, radius, 1u);
	} else {
		scene->DrawSphere (mv, colour, false);
	}
}

bool VPlanet::RingsBehindDecks () const
{
	return hasDecks && ringNIdx && scene->CloudsActive () && oapiCameraProxyGbody () == hObj;
}

void VPlanet::RenderRingBehindDecks ()
{
	if (RingsBehindDecks () && scene->PixelSize (size, camDist) >= 0.5) RenderRing ();
}

bool VPlanet::DecksHide () const
{
	// (the decks are drawn in the prepass, before the scene: their state
	// is this frame's)
	return hasDecks && scene->CloudsActive () && oapiCameraProxyGbody () == hObj && camDist - size < decks.CloudTop ();
}

void VPlanet::RenderGlow ()
{
	// glow around a star, with a minimum apparent size so that it remains
	// visible when the disc itself is smaller than a pixel
	// (relativistic view: about where the star is seen, round: glare is the optics')
	VECTOR3 vp = scene->RelApparentView (scene->ViewPos (gpos));
	if (vp.z <= 0.0) return;
	double dt = scene->DeckTransmittance (scene->CameraPos (), true); // (deep in a gas giant's clouds: the sun's beam)
	if (dt < 1e-3) return;
	double h = Scene::StarGlowSize (size, vp.z, STAR_GLOW_SIZE, STAR_GLOW_MIN_ANGLE);
	// at its true distance, so that planets in front hide it (reversed float depth)
	float x = (float)vp.x, y = (float)vp.y, z = (float)vp.z, hh = (float)h;
	float gc[3] = { colour[0], colour[1], colour[2] };
	scene->RelShiftColour (gpos, gc); // (relativistic view: as the disc)
	dt *= Scene::StarGlowFade (scene->PixelSize (size, camDist));
	if (dt <= 0.0) return;
	float r = gc[0] * STAR_GLOW * (float)dt, g = gc[1] * STAR_GLOW * (float)dt, b = gc[2] * STAR_GLOW * (float)dt;
	PVtx q[4] = {
		{ x - hh, y - hh, z, 0.0f, 0.0f, r, g, b, 1 }, { x + hh, y - hh, z, 1.0f, 0.0f, r, g, b, 1 },
		{ x + hh, y + hh, z, 1.0f, 1.0f, r, g, b, 1 }, { x - hh, y + hh, z, 0.0f, 1.0f, r, g, b, 1 } };
	PVtx v[6] = { q[0], q[1], q[2], q[0], q[2], q[3] };
	scene->DrawBillboards (scene->GlowTex (), true, v, 6, true);
}

// =======================================================================
// The water's splash, spray and wake (Water W9): WaterSpray, from the core's
// water state

void VVessel::UpdateWaterSpray ()
{
	WATERSTATE ws;
	if (!vessel->GetWaterState (&ws)) return;
	VECTOR3 vel;
	vessel->GetGlobalVel (vel);
	water.Update (scene, vessel->GetSurfaceRef (), ws, gpos, vel, size, camDist, id);
}

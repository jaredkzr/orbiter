// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "VObjects.h"
#include "GroundTracks.h"
#include "ScatterAssets.h"
#include "ScatterField.h"
#include "VkMesh.h"
#include "VulkanClient.h"
#include "Particles.h"
#include "PlanetSurface.h"
#include "Atmosphere.h"
#include "Shadows.h"
#include "LegacyTex.h"
#include "TileArchive.h"
#include <map>
#include <random>
#include "VesselAPI.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <unordered_map>
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
	oapiGetGlobalPos (hObj, &gpos);
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
	ParticleSystem *sys = scene->Particles ();
	for (auto *rings : { &smoke, &dust })
		for (auto &r : *rings)
			if (r.sys && r.sys == sys) sys->Free (r.first, r.capacity);
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
}

void VVessel::UpdateDamage ()
{
	// the triangles of parts broken off are not drawn
	DWORD v = oapiGetVesselDamageVersion (hObj);
	if (v == damageVersion) return;
	damageVersion = v;
	std::vector<BYTE> mask;
	for (size_t m = 0; m < meshes.size(); m++) {
		MeshRec &rec = meshes[m];
		if (!rec.mesh) continue;
		for (uint32_t g = 0; g < rec.mesh->nGroup(); g++) {
			DWORD ntri = rec.mesh->GetGroup (g).nIndex / 3;
			mask.assign (ntri, 0);
			DWORD n = v ? oapiGetVesselHiddenTriangles (hObj, (UINT)m, g, mask.data(), ntri) : 0;
			rec.mesh->SetHiddenTriangles (g, n ? mask.data() : nullptr);
		}
	}
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
		if (!near || !vessel->GetTouchdownPoint (tv, i)) { gt->NoContact (hObj, (int)i); continue; }
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
				continue;
			}
		} else t = 1.0;
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
	}
	static const float noGlow[3] = { 0, 0, 0 };
	scene->SetObjectGlassGlow (noGlow);
	scene->SetObjectHeat (_V(0, 0, 0), 0.0f);
	if (pass == RenderPass::Transparent) RenderExhaust ();
}

void VVessel::SetHeating ()
{
	// stagnation-point heat flux (Sutton-Graves, 1 m nose radius) [W/m^2]
	VESSEL *v = oapiGetVesselInterface (hObj);
	VECTOR3 as;
	v->GetAirspeedVector (FRAME_LOCAL, as);
	double spd = length (as), rho = v->GetAtmDensity ();
	double q = 1.74e-4 * sqrt (rho) * spd * spd * spd;
	const double qMin = 1e5, qMax = 8e5; // glow onset, full glow
	float t = (float)std::clamp ((q - qMin) / (qMax - qMin), 0.0, 1.0);
	if (t <= 0.0f || spd <= 0.0) {
		scene->SetObjectHeat (_V(0, 0, 0), 0.0f);
		return;
	}
	VECTOR3 dir = scene->ViewDir (mul (grot, as / spd));
	scene->SetObjectHeat (dir, t);
}

// ======================================================================
// VDebris

VDebris::VDebris (OBJHANDLE h, Scene *s, DWORD sn)
	: VObject (h, s), serial (sn)
{
	DEBRISINFO info;
	if (!oapiGetDebrisInfo (hObj, &info)) return;
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
	if (parts.empty()) return;
	VulkanClient *gc = scene->Client();
	mesh = std::make_unique<VkMesh> (gc->Ctx(), gc->TexMgr(), parts);
}

VDebris::~VDebris ()
{
	ParticleSystem *sys = scene->Particles ();
	for (Ring *r : { &fire, &smoke, &sparks, &dust })
		if (r->sys && r->sys == sys) sys->Free (r->first, r->capacity);
}

void VDebris::Update ()
{
	VObject::Update ();
	hidden = !oapiGetDebrisInfo (hObj, &info) || (info.flags & DEBRIS_HIDDEN);
	if (!hidden) Effects ();
}

double VDebris::FireballRadius () const
{
	// (hydrocarbon fireballs: diameter about 5.8 m per cube root of kg)
	return 2.9 * cbrt (std::max (info.fuel, 0.0));
}

bool VDebris::Emit (Ring &r, ParticleSystem::Emission e, uint32_t n, double ringLife)
{
	ParticleSystem *sys = scene->Particles ();
	if (!sys || !sys->enabled || !n) return false;
	if (r.sys != sys) {
		Ring keep = r;
		r = Ring{};
		r.tex = keep.tex, r.emissive = keep.emissive;
		r.prev = keep.prev, r.havePrev = keep.havePrev, r.accum = keep.accum;
		uint32_t want = (uint32_t)std::clamp ((double)n * 4.0 + ringLife * 400.0 * sys->density, 512.0, 16384.0);
		if (!sys->Allocate (want, r.first, r.capacity)) return false;
		r.sys = sys;
	}
	// (particle_emit.comp: a particle emitted at fraction f of the step at
	// mix (p0, p1, f) moves on with its velocity for the rest of the step:
	// a source at rest in the global frame moves with the velocity given,
	// e.g. the planet's orbital velocity, from p1 - vel dt to p1)
	if (e.p0.x == e.p1.x && e.p0.y == e.p1.y && e.p0.z == e.p1.z) e.p0 = e.p1 - e.vel * e.dt;
	e.first = r.first, e.capacity = r.capacity, e.head = r.head, e.count = std::min (n, r.capacity);
	sys->Queue (e);
	r.head = (r.head + e.count) % r.capacity;
	r.lastEmit = oapiGetSimTime ();
	r.life = std::max (r.life, e.lifetime);
	return true;
}

void VDebris::Effects ()
{
	ParticleSystem *sys = scene->Particles ();
	double dt = oapiGetSimStep ();
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
	if (ac) oapiGetPlanetAtmParams (hp, length (gpos - pp), 0, 0, &atm);
	double rho = ac ? atm.rho : 0.0;
	double drag = ParticleSystem::ExhaustDrag (rho);
	if (fire.tex == nullptr) fire.tex = scene->FireTexture (), fire.emissive = true;
	if (sparks.tex == nullptr) sparks.tex = scene->SparkTexture (), sparks.emissive = true;
	if (smoke.tex == nullptr) smoke.tex = sys->SmokeTexture ();
	if (dust.tex == nullptr) dust.tex = sys->SmokeTexture ();

	// the fireball: propellant released as it broke off (if seen then)
	if (!burst && info.fuel > 1.0 && simt - info.created < 0.5) {
		burst = true;
		double R = FireballRadius ();
		flashT = info.created, flashR = R;
		ParticleSystem::Emission e{};
		e.p0 = e.p1 = gpos, e.vel = vel, e.dir = -up;
		e.v0 = 0.2 * R, e.spread = R / 0.6;
		e.lifetime = 1.6 + 0.04 * R, e.dt = dt;
		e.size = 0.4 * R * sz, e.growth = 0.5 * R * sz;
		e.alpha = 0.22, e.slowdown = 3.0;
		e.emissive = true;
		// (particles by the fireball's size: a piece's share of the propellant)
		Emit (fire, e, (uint32_t)std::clamp (6.0 * R * D, 8.0 * D, 160.0 * D), e.lifetime);
		// the black smoke rising from it
		ParticleSystem::Emission m{};
		m.p0 = m.p1 = gpos, m.vel = vel, m.dir = -up;
		m.v0 = 0.25 * R, m.spread = R / 1.2;
		m.lifetime = 14.0, m.dt = dt;
		m.size = 0.6 * R * sz, m.growth = 0.35 * R * sz;
		m.alpha = 0.6, m.slowdown = 1.0;
		m.tinted = true, m.volumetric = true;
		m.albedo[0] = m.albedo[1] = m.albedo[2] = 0.07f;
		Emit (smoke, m, (uint32_t)std::clamp (3.0 * R * D, 6.0 * D, 80.0 * D), m.lifetime);
	}
	// on the ground: a scorch mark where it burned or crashed
	if (!scorched && alt < 3.0 * size + 2.0 && (info.fuel > 1.0 || info.impact > 8.0) && scene->Tracks ()) {
		scorched = true;
		double R = info.fuel > 1.0 ? std::max (0.8 * FireballRadius (), 2.0 * size) : 1.2 * size;
		scene->Tracks ()->Scorch (hp, gpos - up * (alt + 0.0), up, R, 0.04 * R);
	}

	// burning: fire and black smoke trailing from it
	if (info.burn > 0.0) {
		double f = std::min (1.0, info.burn / 4.0); // (dying down)
		double s = std::max (0.6 * size, 0.5);
		if (!fire.havePrev) fire.prev = gpos - vel * dt;
		fire.accum += 45.0 * D * f * dt;
		uint32_t n = (uint32_t)fire.accum;
		fire.accum -= n;
		if (n) {
			ParticleSystem::Emission e{};
			e.p0 = fire.prev, e.p1 = gpos, e.vel = vel, e.dir = -up;
			e.v0 = 2.0, e.spread = 1.5;
			e.lifetime = 0.7, e.dt = dt;
			e.size = s * sz, e.growth = 0.8 * s * sz;
			e.alpha = 0.35 * f, e.slowdown = std::max (drag, 0.5);
			e.emissive = true;
			Emit (fire, e, n, 0.7);
		}
		fire.prev = gpos, fire.havePrev = true;
		if (rho > 1e-4) { // (smoke in air)
			if (!smoke.havePrev) smoke.prev = gpos - vel * dt;
			smoke.accum += 20.0 * D * f * dt;
			uint32_t m = (uint32_t)smoke.accum;
			smoke.accum -= m;
			if (m) {
				ParticleSystem::Emission e{};
				e.p0 = smoke.prev, e.p1 = gpos, e.vel = vel, e.dir = -up;
				e.v0 = 1.5, e.spread = 1.0;
				e.lifetime = 12.0, e.dt = dt;
				e.size = 1.2 * s * sz, e.growth = 0.6 * s * sz;
				e.alpha = 0.7 * f, e.slowdown = std::max (drag, 0.3);
				e.tinted = true, e.volumetric = true;
				e.albedo[0] = e.albedo[1] = e.albedo[2] = 0.08f;
				Emit (smoke, e, m, 12.0);
			}
			smoke.prev = gpos, smoke.havePrev = true;
		}
	} else fire.havePrev = smoke.havePrev = false;

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
			Emit (sparks, e, n, 0.5);
		}
		if (GroundTracks *gt = scene->Tracks ()) gt->Contact (hObj, 0, hp, contact, up, 0.25 * size, 0.08);
	} else if (GroundTracks *gt = scene->Tracks ()) gt->NoContact (hObj, 0);
	if (info.impact > 1.5 && rho > 1e-4) {
		uint32_t n = (uint32_t)std::min (info.impact * 6.0 * D, 400.0);
		ParticleSystem::Emission e{};
		e.p0 = e.p1 = contact, e.vel = vel, e.dir = -up;
		e.v0 = 0.3 * info.impact, e.spread = 0.6 * info.impact;
		e.lifetime = 6.0, e.dt = dt;
		e.size = 0.8 * size * sz, e.growth = 0.5 * size * sz;
		e.alpha = 0.5, e.slowdown = 1.5;
		e.tinted = true, e.cold = true, e.ballistic = false, e.planar = true, e.volumetric = true;
		e.albedo[0] = 0.6f, e.albedo[1] = 0.53f, e.albedo[2] = 0.43f;
		Emit (dust, e, n, 6.0);
	}
}

void VDebris::CollectLights (std::vector<LightSource> &lights)
{
	// the fireball's light, fading over a second and a half; then the
	// flames while it burns
	double simt = oapiGetSimTime ();
	double t = simt - flashT;
	float I = 0.0f;
	double range = 0.0;
	if (t >= 0.0 && t < 1.5) {
		I = (float)(std::min (1.0, flashR / 10.0) * 40.0 * exp (-3.0 * t));
		range = 15.0 * std::max (flashR, 2.0);
	}
	if (info.burn > 0.0) {
		float b = (float)(4.0 * std::min (1.0, info.burn / 4.0) * (0.85 + 0.15 * sin (simt * 23.0) * sin (simt * 7.3)));
		if (b > I) I = b, range = 12.0 * std::max (size, 1.0);
	}
	if (I <= 0.01f) return;
	LightSource l{};
	l.pos = gpos, l.dir = _V(0, 0, 1);
	l.col[0] = I, l.col[1] = 0.55f * I, l.col[2] = 0.2f * I;
	l.range = range;
	l.att[0] = 1.0, l.att[1] = 0.0, l.att[2] = 1.0 / std::max (range * range * 0.01, 1.0);
	l.spot = false;
	l.owner = id;
	l.shadowOfs = 0.0;
	lights.push_back (l);
}

void VDebris::DrawParticles ()
{
	ParticleSystem *sys = scene->Particles ();
	if (!sys) return;
	double simt = oapiGetSimTime ();
	for (const Ring *r : { &smoke, &dust, &fire, &sparks })
		if (r->sys == sys && simt - r->lastEmit < 1.5 * r->life + 0.1)
			sys->Draw (r->first, r->capacity, r->tex, r->emissive, scene->FrameSet ());
}

void VDebris::Render (RenderPass pass)
{
	if (!mesh || hidden) return;
	Mat4 mv = scene->ModelView (gpos, grot);
	scene->Sunlight (gpos);
	if (pass == RenderPass::Depth) SetMotion ();
	// hot (burned off, burning): glowing, most on its windward side
	float glow = (float)std::clamp ((info.temp - 900.0) / 700.0, 0.0, 1.0);
	if (glow > 0.0f) {
		VECTOR3 v;
		oapiGetGlobalVel (hObj, &v);
		VECTOR3 d = length (v) > 1.0 ? unit (v) : _V(0, 1, 0);
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
		if (rec.mesh && (rec.visMode & MESHVIS_EXTERNAL))
			sm.DrawMesh (rec.mesh.get(), model * Mat4::Translation (rec.ofs), &rec.grpTransform, &rec.grpVisible);
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
	// L and side area A gives an irradiance of L A / d^2 farther away, up to
	// about pi L right beside it (the light's attenuation is capped at 1)
	static const bool engineLight = !getenv ("ORBITER_VK_ENGINE_LIGHT") || atoi (getenv ("ORBITER_VK_ENGINE_LIGHT")) != 0;
	for (const PlumeLight &pl : plumeLights) {
		if (!engineLight) break;
		float peak = std::max (pl.col[0], std::max (pl.col[1], pl.col[2]));
		if (peak <= 0.0f || pl.area <= 0.0) continue;
		LightSource l{};
		l.owner = id;
		l.pos = pl.pos;
		l.range = std::min (sqrt (pl.area * peak / 0.002), 5000.0); // (down to 0.2% of sunlight)
		if (length (l.pos - cam) > l.range + 10e3) continue;
		l.att[0] = 0.0, l.att[1] = 0.0, l.att[2] = PI / pl.area;
		for (int k = 0; k < 3; k++) l.col[k] = (float)(PI * pl.col[k]);
		lights.push_back (l);
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
	DWORD n = vessel->GetExhaustCount ();
	plumeExhaust.assign (n, 0);
	plumeLights.clear ();
	if (!pr.enabled) return;
	VECTOR3 as;
	vessel->GetAirspeedVector (FRAME_LOCAL, as);
	VECTOR3 wind = scene->ViewDir (mul (grot, -as)); // the air past the vessel
	double pa = vessel->GetAtmPressure (), rho = vessel->GetAtmDensity ();
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
		pd.thrust = es.th ? vessel->GetThrusterMax0 (es.th) : 0.0;
		pd.ambientPressure = pa;
		pd.airDensity = rho;
		pd.type = type;
		pd.brightness = o.brightness;
		pd.flicker = (float)(1.0 - 0.5 * es.modulate * uni (rng));
		pd.sunlight = sun;
		pd.tint = es.tex ? (VkTexture*)es.tex : nullptr;
		pd.seed = id * 131u + i * 977u;
		PlumeInfo info{};
		if (pr.Add (pd, id, &info) >= 0) {
			EmitSmoke (i, (int)type, pd, info, dt);
			EmitDust (i, (int)type, pd, dt);
			// (a flame lights its surroundings; cold gas does not glow)
			if (type != PlumeType::ColdGas) {
				PlumeLight pl{ scene->GlobalPos (info.lightPos), { info.lightCol[0], info.lightCol[1], info.lightCol[2] }, info.lightArea };
				plumeLights.push_back (pl);
			}
		}
		else if (i < smoke.size()) smoke[i].havePrev = false;
	}
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
	if (fade < 0.01 || dt <= 0.0) { r.havePrev = false; return; }
	if (!EnsureRing (r, 250.0, c.smokeLife)) return;
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
	double size = (atSurface ? 5.0 : 3.0) * info.endRadius * sizeScale;
	double v0 = 40.0 * pd.level;
	// spacing of a third of the particle size along the trail
	double rate = std::clamp ((length (as) + v0) / (0.35 * std::max (size / sizeScale, 0.5)), 6.0, 250.0) * D;
	if (atSurface) rate = 120.0 * D; // (a cloud building up around the point)
	bool rcs = type == (int)PlumeType::RCS, cold = type == (int)PlumeType::ColdGas;
	r.onTime = r.havePrev ? r.onTime + dt : 0.0;
	if (rcs || cold) {
		// hypergolic jets: a brief puff of unburnt propellant as they light,
		// then nothing lingers in vacuum; cold gas: a faint frosty stream
		rate = (rcs ? (r.onTime < 0.08 ? 250.0 : 0.0) : 40.0) * D * pd.level;
	}
	r.accum += rate * dt;
	uint32_t n = (uint32_t)std::max (r.accum, 0.0);
	r.accum -= n;
	if (!r.havePrev) r.prev = src - vel * dt;
	if (n) {
		ParticleSystem::Emission e{};
		e.p0 = r.prev, e.p1 = src;
		e.vel = vel, e.dir = -flow; // (emitted along -dir)
		// (deflected by a surface: it spreads far along it, carried by the jet,
		// and rises)
		e.v0 = atSurface ? 4.0 : v0;
		e.spread = atSurface ? 1.5 * v0 + 20.0 : 0.6 * v0;
		e.planar = atSurface;
		e.lifetime = c.smokeLife, e.dt = dt;
		e.size = size, e.growth = 0.4 * size + 2.0 * sizeScale;
		e.alpha = fade * std::min (1.0, 1.3 / (D * sizeScale * sizeScale));
		if (rcs || cold) {
			// (along the jet, spreading within its narrow cone)
			e.v0 = rcs ? 60.0 : 30.0, e.spread = rcs ? 25.0 : 10.0;
			e.size = (rcs ? 0.3 : 0.15) * sizeScale, e.growth = (rcs ? 5.0 : 2.0) * sizeScale;
			e.alpha = (rcs ? 0.2 : 0.05) * pd.level;
		}
		e.slowdown = atSurface ? 0.8 : ParticleSystem::ExhaustDrag (pd.airDensity);
		e.first = r.first, e.capacity = r.capacity, e.head = r.head, e.count = std::min (n, r.capacity);
		e.tinted = true;
		e.volumetric = !rcs && !cold;
		for (int k = 0; k < 3; k++) e.albedo[k] = c.smokeAlbedo[k];
		sys->Queue (e);
		r.head = (r.head + e.count) % r.capacity;
		r.lastEmit = oapiGetSimTime ();
		r.life = c.smokeLife;
	}
	r.prev = src;
	r.havePrev = true;
}

bool VVessel::EnsureRing (SmokeRing &r, double rate, double life)
{
	ParticleSystem *sys = scene->Particles ();
	if (r.sys == sys) return true;
	r = SmokeRing{}; // (the pool belongs to the scene)
	double want = rate * sys->density * life * 1.2;
	if (!sys->Allocate ((uint32_t)std::min (want, 65536.0), r.first, r.capacity)) return false;
	r.sys = sys;
	return true;
}

namespace {
	// colour of dust thrown up from the surface of a body (fine dust is
	// brighter than the surface); spray on water
	void DustColour (OBJHANDLE hp, float c[3])
	{
		char name[64];
		oapiGetObjectName (hp, name, 64);
		struct { const char *name; float c[3]; } tab[] = {
			{ "Earth", { 0.62f, 0.55f, 0.45f } }, { "Moon", { 0.52f, 0.51f, 0.49f } },
			{ "Mars", { 0.66f, 0.45f, 0.31f } }, { "Mercury", { 0.45f, 0.44f, 0.42f } },
			{ "Venus", { 0.60f, 0.50f, 0.38f } }, { "Titan", { 0.50f, 0.40f, 0.28f } } };
		for (auto &t : tab)
			if (!strcmp (name, t.name)) { for (int k = 0; k < 3; k++) c[k] = t.c[k]; return; }
		c[0] = c[1] = c[2] = 0.5f;
	}
}

void VVessel::EmitDust (DWORD idx, int type, const PlumeDraw &pd, double dt)
{
	ParticleSystem *sys = scene->Particles ();
	PlumeRenderer *pr = scene->Plumes ();
	if (idx >= dust.size()) dust.resize (idx + 1);
	SmokeRing &r = dust[idx];
	// a jet stirs up the ground within a distance growing with its thrust
	double thrust = pd.thrust * pd.level;
	if (!sys || !sys->enabled || !pr || !pr->rayCast || dt <= 0.0 || thrust < 1e3 ||
		type == (int)PlumeType::RCS || type == (int)PlumeType::ColdGas || type == (int)PlumeType::Ion) { r.havePrev = false; return; }
	double reach = 2.5 * sqrt (thrust / 1e3);
	double t;
	VECTOR3 vn;
	int kind = 2;
	if (!pr->rayCast (id, pd.pos, unit (pd.dir), reach, t, vn, &kind) || kind == 2) { r.havePrev = false; return; }
	double strength = (1.0 - t / reach) * (1.0 - t / reach);
	OBJHANDLE hp = vessel->GetSurfaceRef ();
	if (!hp || strength < 0.02) { r.havePrev = false; return; }
	VECTOR3 hit = scene->GlobalPos (pd.pos + unit (pd.dir) * t);
	VECTOR3 n = scene->GlobalDir (vn);
	// the jet scorches the ground (or a pad) where it is still hot: close to
	// the nozzle, darkening over seconds of firing
	double hot = std::clamp (1.0 - t / (0.25 * reach), 0.0, 1.0);
	bool onWater = false;
	{
		VECTOR3 l;
		oapiGlobalToLocal (hp, &hit, &l);
		double rl = length (l);
		char nm[64];
		oapiGetObjectName (hp, nm, 64);
		onWater = kind == 0 && !strcmp (nm, "Earth") &&
			oapiSurfaceElevation (hp, atan2 (l.z, l.x), asin (std::clamp (l.y / rl, -1.0, 1.0))) <= 0.0;
	}
	if (hot > 0.0 && !onWater)
		pr->Scorch (hp, hit, n, std::max (1.0, 0.12 * reach) * (0.7 + 0.3 * hot), 0.08 * hot * hot * dt);
	if (kind == 1) {
		// launch pads: the sound suppression water flashes into steam under
		// large engines, billowing out and rising in great white clouds
		if (thrust < 5e5 || pd.ambientPressure < 5e4) { r.havePrev = false; return; }
		double life = 25.0;
		if (!EnsureRing (r, 120.0, life)) return;
		double D = sys->density, sizeScale = std::pow (D, -1.0 / 3.0);
		VECTOR3 vel;
		vessel->GetGlobalVel (vel);
		VECTOR3 src = hit + n * 2.0;
		if (!r.havePrev) r.prev = src;
		r.accum += 60.0 * D * std::min (1.0, strength * 2.0) * dt;
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
			e.first = r.first, e.capacity = r.capacity, e.head = r.head, e.count = std::min (cnt, r.capacity);
			e.tinted = true; // (hot: rises)
			e.volumetric = true;
			e.albedo[0] = e.albedo[1] = e.albedo[2] = 0.97f;
			sys->Queue (e);
			r.head = (r.head + e.count) % r.capacity;
			r.lastEmit = oapiGetSimTime ();
			r.life = life;
		}
		r.prev = src;
		r.havePrev = true;
		return;
	}
	// water: spray instead of dust (the sea's surface is at elevation 0)
	VECTOR3 loc;
	oapiGlobalToLocal (hp, &hit, &loc);
	double rad = length (loc);
	double elev = oapiSurfaceElevation (hp, atan2 (loc.z, loc.x), asin (std::clamp (loc.y / rad, -1.0, 1.0)));
	char name[64];
	oapiGetObjectName (hp, name, 64);
	bool water = !strcmp (name, "Earth") && elev <= 0.0;
	bool air = pd.ambientPressure > 100.0;

	double life = air ? (water ? 3.0 : 10.0) : 5.0;
	if (!EnsureRing (r, 250.0, life)) return;
	double D = sys->density, sizeScale = std::pow (D, -1.0 / 3.0);
	double radial = air ? 6.0 + 18.0 * strength : 30.0 + 70.0 * strength; // [m/s]
	VECTOR3 vel;
	vessel->GetGlobalVel (vel);
	VECTOR3 src = hit + n * 0.3;
	if (!r.havePrev) r.prev = src;
	r.accum += (air ? 120.0 : 250.0) * D * std::min (1.0, 0.3 + strength) * dt;
	uint32_t cnt = (uint32_t)r.accum;
	r.accum -= cnt;
	if (cnt) {
		ParticleSystem::Emission e{};
		e.p0 = r.prev, e.p1 = src;
		// (thrown up from the ground, at rest with it: the vessel's velocity,
		// close enough near the ground)
		e.vel = vel;
		e.dir = -n;
		e.v0 = (air ? 0.12 : 0.025) * radial; // (low angles; in vacuum they would arc far)
		e.spread = 2.0 * radial;
		e.planar = true;
		e.lifetime = life, e.dt = dt;
		double size = air ? 4.0 : 0.6;
		e.size = size * sizeScale, e.growth = (air ? 2.5 : 0.2) * sizeScale;
		e.alpha = (air ? 0.22 : 0.45) * strength * std::min (1.0, 1.3 / (D * sizeScale * sizeScale));
		e.slowdown = air ? 2.0 : 0.0;
		e.first = r.first, e.capacity = r.capacity, e.head = r.head, e.count = std::min (cnt, r.capacity);
		e.tinted = true, e.cold = true, e.ballistic = true;
		e.volumetric = air;
		if (water) e.albedo[0] = e.albedo[1] = e.albedo[2] = 0.95f;
		else DustColour (hp, e.albedo);
		sys->Queue (e);
		r.head = (r.head + e.count) % r.capacity;
		r.lastEmit = oapiGetSimTime ();
		r.life = life;
	}
	r.prev = src;
	r.havePrev = true;
}

void VVessel::DrawParticles ()
{
	ParticleSystem *sys = scene->Particles ();
	if (!sys) return;
	double simt = oapiGetSimTime ();
	for (auto *rings : { &smoke, &dust })
		for (const SmokeRing &r : *rings)
			if (r.sys == sys && simt - r.lastEmit < 1.5 * r.life + 0.1)
				sys->Draw (r.first, r.capacity, sys->SmokeTexture (), false, scene->FrameSet ());
}

void VVessel::RenderExhaust ()
{
	// (plume volumes: Scene::Render, after the transparent pass)
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
	constexpr double GROUND_DEPTH_PULL = 2e-4;
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
	oapiGetGlobalPos (hBase, &gpos);
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
		for (int k = 0; k < 3; k++) l.col[k] = fl.col[k];
		out.push_back (l);
	}
}

void VBase::Render (RenderPass pass)
{
	VECTOR3 gpos;
	MATRIX3 grot;
	oapiGetGlobalPos (hBase, &gpos);
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
			float h = (float)std::max (size, vp.z * 0.0015); // (a few pixels at least)
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
	oapiGetGlobalPos (hBase, &gpos);
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
	oapiGetGlobalPos (hBase, &gpos);
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
	// the terrain: the ray's altitude over the surface elevation
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
		if (surface && !DecksHide () && scene->PixelSize (size, camDist) >= 0.5) surface->RenderDepth ();
		for (auto &b : bases) b->Render (pass);
		UpdateGallery ();
		if (gallery) gallery->Render (pass);
		if (field) field->Render (pass);
		return;
	}
	if (pass == RenderPass::Transparent) {
		if (isStar) { RenderGlow (); return; }
		for (auto &b : bases) b->Render (pass);
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
		// the sun's disc is far brighter than any lit surface
		float c[4] = { colour[0] * STAR_BRIGHTNESS, colour[1] * STAR_BRIGHTNESS, colour[2] * STAR_BRIGHTNESS, 1.0f };
		scene->DrawSphere (mv, c, true);
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
	VECTOR3 vp = scene->ViewPos (gpos);
	if (vp.z <= 0.0) return;
	double dt = scene->DeckTransmittance (scene->CameraPos (), true); // (deep in a gas giant's clouds: the sun's beam)
	if (dt < 1e-3) return;
	double h = std::max (size * STAR_GLOW_SIZE, vp.z * STAR_GLOW_MIN_ANGLE);
	// at its true distance, so that planets in front hide it (reversed float depth)
	float x = (float)vp.x, y = (float)vp.y, z = (float)vp.z, hh = (float)h;
	float r = colour[0] * STAR_GLOW * (float)dt, g = colour[1] * STAR_GLOW * (float)dt, b = colour[2] * STAR_GLOW * (float)dt;
	PVtx q[4] = {
		{ x - hh, y - hh, z, 0.0f, 0.0f, r, g, b, 1 }, { x + hh, y - hh, z, 1.0f, 0.0f, r, g, b, 1 },
		{ x + hh, y + hh, z, 1.0f, 1.0f, r, g, b, 1 }, { x - hh, y + hh, z, 0.0f, 1.0f, r, g, b, 1 } };
	PVtx v[6] = { q[0], q[1], q[2], q[0], q[2], q[3] };
	scene->DrawBillboards (scene->GlowTex (), true, v, 6);
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "VkMesh.h"
#include "VkTexture.h"
#include <algorithm>

namespace {
	constexpr DWORD SPEC_NONE = (DWORD)-1; // "no texture" / "default material" index

	bool IsTransparent (const MATERIAL &m, const VkMesh::Group &g)
	{
		return m.diffuse.a < 0.999f || (g.usrFlag & VkMesh::GRP_ADDITIVE);
	}
}

VkMesh::VkMesh (vkc::Context *c, TextureManager *tm, MESHHANDLE h)
	: ctx(c), texmgr(tm), hMesh(h)
{
	// materials: slot 0 is the default (white, unlit by specular)
	MATERIAL def{};
	def.diffuse = { 1, 1, 1, 1 };
	def.ambient = { 1, 1, 1, 1 };
	materials.push_back (def);
	DWORD nMtrl = oapiMeshMaterialCount (hMesh);
	for (DWORD i = 0; i < nMtrl; i++) {
		MATERIAL *m = oapiMeshMaterial (hMesh, i);
		materials.push_back (m ? *m : def);
	}

	for (auto &m : materials) pbr.push_back (PBRParams::FromMaterial (m));
	nTemplateMtrl = materials.size();

	// textures are owned by the mesh template; keep references while we use them
	DWORD nTex = oapiMeshTextureCount (hMesh);
	for (DWORD i = 0; i < nTex; i++) {
		VkTexture *tex = (VkTexture*)oapiGetTextureHandle (hMesh, i + 1);
		texmgr->AddRef (tex);
		textures.push_back (tex);
	}

	// geometry: all groups in one vertex and one index buffer
	DWORD nGrp = oapiMeshGroupCount (hMesh);
	for (DWORD i = 0; i < nGrp; i++) {
		MESHGROUP *mg = oapiMeshGroup (hMesh, i);
		Group g;
		if (mg) {
			g.firstIndex = (uint32_t)idx.size();
			g.nIndex = mg->nIdx;
			g.vertexOffset = (int32_t)vtx.size();
			g.material = (mg->MtrlIdx == SPEC_NONE || mg->MtrlIdx + 1 >= materials.size()) ? 0 : mg->MtrlIdx + 1;
			g.flags = mg->Flags;
			g.usrFlag = mg->UsrFlag;
			DWORD ti = (mg->TexIdx == SPEC_NONE || mg->TexIdx >= textures.size()) ? SPEC_NONE : mg->TexIdx;
			groupTexIdx.push_back (ti);
			groupMtrlIdx.push_back (mg->MtrlIdx);
			g.tex = (ti == SPEC_NONE ? nullptr : textures[ti]);
			g.transparent = IsTransparent (materials[g.material], g);
			vtx.insert (vtx.end(), mg->Vtx, mg->Vtx + mg->nVtx);
			idx.insert (idx.end(), mg->Idx, mg->Idx + mg->nIdx);
		} else {
			groupTexIdx.push_back (SPEC_NONE);
			groupMtrlIdx.push_back (SPEC_NONE);
		}
		groups.push_back (g);
	}
	if (!vtx.empty())
		vbuf = ctx->CreateDeviceBuffer (vtx.data(), vtx.size() * sizeof(NTVERTEX), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	if (!idx.empty())
		ibuf = ctx->CreateDeviceBuffer (idx.data(), idx.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

VkMesh::~VkMesh ()
{
	for (auto tex : textures) texmgr->Release (tex);
	for (auto tex : cfgTextures) texmgr->Release (tex);
	vkc::Context *c = ctx;
	vkc::Buffer vb = vbuf, ib = ibuf;
	ctx->DeferDestroy ([c, vb, ib]() mutable {
		c->DestroyBuffer (vb);
		c->DestroyBuffer (ib);
	});
}

bool VkMesh::SetTexture (DWORD texidx, VkTexture *tex)
{
	// texidx is 1-based, as in oapiGetTextureHandle
	if (!texidx || texidx > textures.size()) return false;
	DWORD i = texidx - 1;
	texmgr->AddRef (tex);
	texmgr->Release (textures[i]);
	textures[i] = tex;
	for (size_t g = 0; g < groups.size(); g++)
		if (groupTexIdx[g] == i) groups[g].tex = tex;
	return true;
}

void VkMesh::ApplyMaterialConfig (const MaterialConfig &cfg, const std::string &meshName)
{
	int n = 0;
	size_t nMat = pbr.size();
	for (size_t i = 1; i < nMat; i++)
		if (cfg.Apply (meshName, (int)i - 1, pbr[i])) n++;
	if (n) oapiWriteLogV ("VulkanClient: material configuration applied to %d materials of %s", n, meshName.c_str());

	// texture replacements
	for (size_t i = 1; i < nMat; i++) {
		MaterialTextures t;
		if (!cfg.Textures (meshName, (int)i - 1, pbr[i], t)) continue;
		auto load = [&](const std::string &fn) -> VkTexture* {
			VkTexture *tex = texmgr->Load (fn.c_str());
			if (tex) cfgTextures.push_back (tex);
			else oapiWriteLogV ("VulkanClient: texture %s of the material configuration of %s not found", fn.c_str(), meshName.c_str());
			return tex;
		};
		VkTexture *tex = t.texture.empty() ? nullptr : load (t.texture);
		VkTexture *sideTex = t.sideTexture.empty() ? nullptr : load (t.sideTexture);
		uint32_t sideMat = 0;
		if (sideTex) { // a material slot for the side
			sideMat = (uint32_t)materials.size();
			materials.push_back (materials[i]);
			pbr.push_back (t.sidePBR);
		}
		for (Group &g : groups) {
			if (g.material != i) continue;
			if (tex) g.tex = tex;
			if (!sideTex || g.nIndex < 3) continue;
			// triangles facing the direction (by their vertex normals) to the end
			std::vector<uint16_t> front, side;
			for (uint32_t k = 0; k + 2 < g.nIndex; k += 3) {
				const uint16_t *tri = &idx[g.firstIndex + k];
				float d = 0.0f;
				for (int j = 0; j < 3; j++) {
					const NTVERTEX &v = vtx[g.vertexOffset + tri[j]];
					d += v.nx * t.sideDir[0] + v.ny * t.sideDir[1] + v.nz * t.sideDir[2];
				}
				(d > 0.0f ? side : front).insert ((d > 0.0f ? side : front).end(), tri, tri + 3);
			}
			if (side.empty()) continue;
			std::copy (front.begin(), front.end(), idx.begin() + g.firstIndex);
			std::copy (side.begin(), side.end(), idx.begin() + g.firstIndex + front.size());
			if (ibuf.buffer)
				ctx->UpdateBuffer (ibuf.buffer, g.firstIndex * sizeof(uint16_t), idx.data() + g.firstIndex, (front.size() + side.size()) * sizeof(uint16_t));
			g.nSide = (uint32_t)side.size();
			g.sideTex = sideTex;
			g.sideMaterial = sideMat;
		}
	}
}

int VkMesh::SetMaterial (DWORD matidx, const MATERIAL *mat)
{
	if (matidx + 1 >= nTemplateMtrl) return 4;
	materials[matidx + 1] = *mat;
	pbr[matidx + 1] = PBRParams::FromMaterial (*mat);
	for (auto &g : groups)
		if (g.material == matidx + 1) g.transparent = IsTransparent (materials[g.material], g);
	return 0;
}

int VkMesh::GetMaterial (DWORD matidx, MATERIAL *mat) const
{
	if (matidx + 1 >= nTemplateMtrl) return 4;
	*mat = materials[matidx + 1];
	return 0;
}

int VkMesh::GetGroupData (DWORD grp, GROUPREQUESTSPEC *grs) const
{
	if (grp >= groups.size()) return 1;
	const Group &g = groups[grp];
	DWORD nv = (grp + 1 < groups.size() ? groups[grp+1].vertexOffset : (int32_t)vtx.size()) - g.vertexOffset;
	int res = 0;
	if (grs->nVtx && grs->Vtx) {
		for (DWORD i = 0; i < grs->nVtx; i++) {
			DWORD vi = grs->VtxPerm ? grs->VtxPerm[i] : i;
			if (vi < nv) grs->Vtx[i] = vtx[g.vertexOffset + vi];
			else res = 1;
		}
	}
	if (grs->nIdx && grs->Idx) {
		for (DWORD i = 0; i < grs->nIdx; i++) {
			DWORD ii = grs->IdxPerm ? grs->IdxPerm[i] : i;
			if (ii < g.nIndex) grs->Idx[i] = idx[g.firstIndex + ii];
			else res = 1;
		}
	}
	grs->MtrlIdx = groupMtrlIdx[grp];
	grs->TexIdx = groupTexIdx[grp];
	return res;
}

int VkMesh::EditGroup (DWORD grp, GROUPEDITSPEC *ges)
{
	if (grp >= groups.size()) return 1;
	Group &g = groups[grp];
	DWORD flag = ges->flags;
	if (flag & GRPEDIT_SETUSERFLAG)      g.usrFlag = ges->UsrFlag;
	else if (flag & GRPEDIT_ADDUSERFLAG) g.usrFlag |= ges->UsrFlag;
	else if (flag & GRPEDIT_DELUSERFLAG) g.usrFlag &= ~ges->UsrFlag;

	if ((flag & GRPEDIT_VTXMOD) && ges->Vtx) {
		DWORD nv = (grp + 1 < groups.size() ? groups[grp+1].vertexOffset : (int32_t)vtx.size()) - g.vertexOffset;
		NTVERTEX *gv = vtx.data() + g.vertexOffset;
		DWORD vmin = nv, vmax = 0;
		for (DWORD i = 0; i < ges->nVtx; i++) {
			DWORD vi = ges->vIdx ? ges->vIdx[i] : i;
			if (vi >= nv) continue;
			NTVERTEX &v = gv[vi];
			const NTVERTEX &s = ges->Vtx[i];
			if (flag & GRPEDIT_VTXCRDX) v.x = s.x;   else if (flag & GRPEDIT_VTXCRDADDX) v.x += s.x;
			if (flag & GRPEDIT_VTXCRDY) v.y = s.y;   else if (flag & GRPEDIT_VTXCRDADDY) v.y += s.y;
			if (flag & GRPEDIT_VTXCRDZ) v.z = s.z;   else if (flag & GRPEDIT_VTXCRDADDZ) v.z += s.z;
			if (flag & GRPEDIT_VTXNMLX) v.nx = s.nx; else if (flag & GRPEDIT_VTXNMLADDX) v.nx += s.nx;
			if (flag & GRPEDIT_VTXNMLY) v.ny = s.ny; else if (flag & GRPEDIT_VTXNMLADDY) v.ny += s.ny;
			if (flag & GRPEDIT_VTXNMLZ) v.nz = s.nz; else if (flag & GRPEDIT_VTXNMLADDZ) v.nz += s.nz;
			if (flag & GRPEDIT_VTXTEXU) v.tu = s.tu; else if (flag & GRPEDIT_VTXTEXADDU) v.tu += s.tu;
			if (flag & GRPEDIT_VTXTEXV) v.tv = s.tv; else if (flag & GRPEDIT_VTXTEXADDV) v.tv += s.tv;
			vmin = std::min (vmin, vi);
			vmax = std::max (vmax, vi);
		}
		if (vmin <= vmax && vbuf.buffer)
			ctx->UpdateBuffer (vbuf.buffer, (g.vertexOffset + vmin) * sizeof(NTVERTEX), gv + vmin, (vmax - vmin + 1) * sizeof(NTVERTEX));
	}
	return 0;
}

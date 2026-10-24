// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "VkMesh.h"
#include "VkTexture.h"

namespace {
	constexpr DWORD SPEC_NONE = (DWORD)-1; // "no texture" / "default material" index

	bool IsTransparent (const MATERIAL &m, const VkMesh::Group &g)
	{
		return m.diffuse.a < 0.999f || (g.flags & VkMesh::GRP_ADDITIVE);
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

	// textures are owned by the mesh template; keep references while we use them
	DWORD nTex = oapiMeshTextureCount (hMesh);
	for (DWORD i = 0; i < nTex; i++) {
		VkTexture *tex = (VkTexture*)oapiGetTextureHandle (hMesh, i + 1);
		texmgr->AddRef (tex);
		textures.push_back (tex);
	}

	// geometry: all groups in one vertex and one index buffer
	std::vector<NTVERTEX> vtx;
	std::vector<uint16_t> idx;
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
			g.tex = (ti == SPEC_NONE ? nullptr : textures[ti]);
			g.transparent = IsTransparent (materials[g.material], g);
			vtx.insert (vtx.end(), mg->Vtx, mg->Vtx + mg->nVtx);
			idx.insert (idx.end(), mg->Idx, mg->Idx + mg->nIdx);
		} else {
			groupTexIdx.push_back (SPEC_NONE);
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

int VkMesh::SetMaterial (DWORD matidx, const MATERIAL *mat)
{
	if (matidx + 1 >= materials.size()) return 4;
	materials[matidx + 1] = *mat;
	for (auto &g : groups)
		if (g.material == matidx + 1) g.transparent = IsTransparent (materials[g.material], g);
	return 0;
}

int VkMesh::GetMaterial (DWORD matidx, MATERIAL *mat) const
{
	if (matidx + 1 >= materials.size()) return 4;
	*mat = materials[matidx + 1];
	return 0;
}

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Device mesh: GPU copy of an Orbiter mesh
// =======================================================================

#ifndef __VKMESH_H
#define __VKMESH_H

#include "VkContext.h"
#include "OrbiterAPI.h"
#include <vector>

class VkTexture;
class TextureManager;

class VkMesh {
public:
	struct Group {
		uint32_t firstIndex = 0;
		uint32_t nIndex = 0;
		int32_t vertexOffset = 0;
		uint32_t material = 0;  // index into materials (0 = default material)
		VkTexture *tex = nullptr;
		DWORD flags = 0;         // mesh group flags (see Orbiter mesh format)
		DWORD usrFlag = 0;
		bool transparent = false;
	};

	/// Mesh group flags from the Orbiter mesh format
	static constexpr DWORD GRP_NOSHADOW = 0x1;
	static constexpr DWORD GRP_HIDDEN   = 0x2;
	static constexpr DWORD GRP_NOLIGHT  = 0x4;
	static constexpr DWORD GRP_ADDITIVE = 0x8;

	VkMesh (vkc::Context *ctx, TextureManager *texmgr, MESHHANDLE hMesh);
	~VkMesh ();

	uint32_t nGroup () const { return (uint32_t)groups.size(); }
	const Group &GetGroup (uint32_t i) const { return groups[i]; }
	const MATERIAL &Material (uint32_t i) const { return materials[i]; }
	VkBuffer VertexBuffer () const { return vbuf.buffer; }
	VkBuffer IndexBuffer () const { return ibuf.buffer; }
	MESHHANDLE Template () const { return hMesh; }

	bool SetTexture (DWORD texidx, VkTexture *tex);
	int SetMaterial (DWORD matidx, const MATERIAL *mat);
	int GetMaterial (DWORD matidx, MATERIAL *mat) const;

private:
	vkc::Context *ctx;
	TextureManager *texmgr;
	MESHHANDLE hMesh;
	vkc::Buffer vbuf, ibuf;
	std::vector<Group> groups;
	std::vector<MATERIAL> materials; // [0] = default material, [i+1] = mesh material i
	std::vector<VkTexture*> textures; // mesh texture list (mesh texture index order)
	std::vector<DWORD> groupTexIdx;  // per group: index into textures, or -1
};

#endif // !__VKMESH_H

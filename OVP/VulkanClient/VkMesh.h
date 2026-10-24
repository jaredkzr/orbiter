// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Device mesh: GPU copy of an Orbiter mesh
// =======================================================================

#ifndef __VKMESH_H
#define __VKMESH_H

#include "VkContext.h"
#include "OrbiterAPI.h"
#include "MaterialConfig.h"
#include <string>
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
		// triangles drawn with another texture and material (material
		// configuration SIDE_TEXTURE): the last nSide indices of the group
		uint32_t nSide = 0;
		VkTexture *sideTex = nullptr;
		uint32_t sideMaterial = 0;
	};

	/// Mesh group flags from the Orbiter mesh format
	// group user flags (UsrFlag, the FLAG entry of mesh files)
	static constexpr DWORD GRP_NOSHADOW = 0x1;
	static constexpr DWORD GRP_HIDDEN   = 0x2;
	static constexpr DWORD GRP_NOLIGHT  = 0x4;
	static constexpr DWORD GRP_ADDITIVE = 0x8;

	VkMesh (vkc::Context *ctx, TextureManager *texmgr, MESHHANDLE hMesh);
	~VkMesh ();

	uint32_t nGroup () const { return (uint32_t)groups.size(); }
	const Group &GetGroup (uint32_t i) const { return groups[i]; }
	const MATERIAL &Material (uint32_t i) const { return materials[i]; }
	const PBRParams &PBR (uint32_t i) const { return pbr[i]; }
	/// Apply material configuration for the mesh file the mesh was loaded from
	void ApplyMaterialConfig (const MaterialConfig &cfg, const std::string &meshName);
	VkBuffer VertexBuffer () const { return vbuf.buffer; }
	VkBuffer IndexBuffer () const { return ibuf.buffer; }
	MESHHANDLE Template () const { return hMesh; }
	/// unique over the session (ray tracing structures are keyed by it)
	uint64_t Id () const { return id; }
	/// incremented when vertices change
	uint32_t GeomVersion () const { return geomVersion; }
	uint32_t GroupVertexCount (uint32_t g) const;
	/// called with the id of each mesh destroyed or edited (ray tracing)
	static void (*onGeometryChange) (uint64_t id);

	bool SetTexture (DWORD texidx, VkTexture *tex);
	int GetGroupData (DWORD grp, GROUPREQUESTSPEC *grs) const;
	int EditGroup (DWORD grp, GROUPEDITSPEC *ges);
	int SetMaterial (DWORD matidx, const MATERIAL *mat);
	int GetMaterial (DWORD matidx, MATERIAL *mat) const;

private:
	vkc::Context *ctx;
	TextureManager *texmgr;
	MESHHANDLE hMesh;
	uint64_t id;
	uint32_t geomVersion = 0;
	vkc::Buffer vbuf, ibuf;
	std::vector<Group> groups;
	std::vector<MATERIAL> materials; // [0] = default material, [i+1] = mesh material i, then side materials
	size_t nTemplateMtrl = 0;        // default and mesh materials
	std::vector<PBRParams> pbr;      // shading parameters, same indexing
	std::vector<VkTexture*> textures; // mesh texture list (mesh texture index order)
	std::vector<DWORD> groupTexIdx;  // per group: index into textures, or -1
	std::vector<DWORD> groupMtrlIdx; // per group: material index of the template
	std::vector<NTVERTEX> vtx;       // CPU copies of the geometry, for group queries and edits
	std::vector<uint16_t> idx;
	std::vector<VkTexture*> cfgTextures; // textures loaded for the material configuration
};

#endif // !__VKMESH_H

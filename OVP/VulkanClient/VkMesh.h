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
		float alphaTest = 0.0f;  // texture alpha below which fragments are discarded (foliage; 0: none)
	};

	/// Mesh group flags from the Orbiter mesh format
	// group user flags (UsrFlag, the FLAG entry of mesh files)
	static constexpr DWORD GRP_NOSHADOW = 0x1;
	static constexpr DWORD GRP_HIDDEN   = 0x2;
	static constexpr DWORD GRP_NOLIGHT  = 0x4;
	static constexpr DWORD GRP_ADDITIVE = 0x8;

	VkMesh (vkc::Context *ctx, TextureManager *texmgr, MESHHANDLE hMesh);

	/// A group of generated geometry (procedural meshes): vertices,
	/// triangles, texture (a reference is taken), material and shading
	struct Part {
		std::vector<NTVERTEX> vtx;
		std::vector<uint16_t> idx;
		VkTexture *tex = nullptr;
		MATERIAL mat{ {1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, 0.0f };
		PBRParams pbr;
		float alphaTest = 0.0f;  // (see Group)
	};
	/// Mesh from generated geometry (no template mesh)
	VkMesh (vkc::Context *ctx, TextureManager *texmgr, const std::vector<Part> &parts);
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
	/// ray tracing geometry id (RTNewGeometryId)
	uint64_t Id () const { return id; }
	/// incremented when vertices change
	uint32_t GeomVersion () const { return geomVersion; }
	uint32_t GroupVertexCount (uint32_t g) const;
	/// Distance along a ray (mesh frame) to its last intersection with the
	/// mesh within maxDist, 0 if none (where a ray from inside leaves it)
	double RayExit (const VECTOR3 &o, const VECTOR3 &d, double maxDist) const;
	/// Nearest intersection of a ray (mesh frame, |d| = 1) with the mesh
	/// within maxDist: distance t and the triangle's unit normal n
	bool RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double &t, VECTOR3 &n) const;
	/// groups that were ever drawn with an animation transform (separate
	/// ray tracing instances)
	mutable std::vector<uint8_t> rtAnimated;


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
	// triangle hierarchy for RayHit (built when first needed; static geometry)
	struct BvhNode { float lo[3], hi[3]; uint32_t first, count; }; // count 0: children at first, first + 1
	struct BvhTri { float v[3][3]; };
	mutable std::vector<BvhNode> bvh;
	mutable std::vector<BvhTri> bvhTris;
	mutable bool haveBvh = false;
	void BuildBvh () const;
	std::vector<uint16_t> idx;
	std::vector<VkTexture*> cfgTextures; // textures loaded for the material configuration
};

#endif // !__VKMESH_H

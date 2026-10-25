// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Ray tracing scene for hardware ray queries (reflections): a bottom-level
// acceleration structure per drawn mesh group range (built once, rebuilt
// after vertex edits), and per frame a top-level structure of the opaque
// geometry drawn into the prepass, in view space. Each instance carries
// what a shader needs to shade a hit: the geometry's device addresses, a
// texture slot in a bindless texture array, and material parameters.
// =======================================================================

#ifndef __RAYTRACING_H
#define __RAYTRACING_H

#include "VkContext.h"
#include "VkMath.h"
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

class VkTexture;
class TextureManager;

/// Unique id for geometry in the ray tracing scene
uint64_t RTNewGeometryId ();
/// Geometry with this id was destroyed (no-op without ray tracing)
void RTForgetGeometry (uint64_t id);

class RTScene {
public:
	static constexpr uint32_t MAX_INSTANCES = 16384;
	static constexpr uint32_t MAX_RECORDS = 65536; // geometries of all instances
	static constexpr uint32_t MAX_TEXTURES = 4096;

	/// Material of an instance (must match RTInstance in rt.glsl)
	struct Material {
		float diffuse[4];
		float emissive[3];
		float light;        // sunlight reaching the object (eclipses)
		float pbr[4];       // metalness, roughness, reflectivity, F0 scale
		float uv[4] = { 0, 0, 1, 1 }; // texture coordinates: offset, scale
		bool unlit;
		bool alphaTest = false; // texture alpha decides where it is solid (foliage)
		bool shadowOnly = false; // a stand-in that only blocks light (not seen in reflections; instance mask 0x04)
		uint32_t owner = 0;   // VObject::Id (0: terrain)
	};
	/// Indexed triangles (NTVERTEX vertices, 16-bit indices)
	struct Geometry {
		uint64_t id;          // RTNewGeometryId
		uint32_t version;     // changes when the vertices change
		VkBuffer vb;
		VkDeviceSize vbOffset; // [bytes]
		uint32_t nVtx;
		VkBuffer ib;          // (whole 32-bit words: the shaders read pairs)
		uint32_t firstIndex, nIndex;
	};

	bool Init (vkc::Context *ctx, TextureManager *texmgr);
	void Shutdown ();
	bool Enabled () const { return ctx != nullptr; }

	/// Start collecting the instances of this frame
	void BeginFrame ();
	/// Geometry drawn with a texture and material
	struct Part {
		Geometry geo;
		VkTexture *tex;
		Material mat;
	};
	/// An instance of parts sharing the model-view transform (e.g. the
	/// static groups of a mesh: one bottom-level structure for all keeps
	/// rays from descending into many overlapping ones). The instance mask
	/// is that of the first part's owner.
	void AddInstance (const Part *parts, uint32_t nParts, const Mat4 &modelView);
	void AddInstance (const Geometry &geo, const Mat4 &modelView, VkTexture *tex, const Material &mat)
	{ Part p{ geo, tex, mat }; AddInstance (&p, 1, modelView); }
	/// Build the acceleration structures of this frame (outside rendering
	/// scopes; before the shaders that trace)
	void Build ();

	/// descriptor set: top-level structure (binding 0) and instance records
	/// (binding 1) of this frame
	VkDescriptorSet Set () const;
	VkDescriptorSetLayout SetLayout () const { return setLayout; }
	/// bindless texture array (binding 0)
	VkDescriptorSet TextureSet () const { return texSet; }
	VkDescriptorSetLayout TextureLayout () const { return texLayout; }

	/// geometry was destroyed
	void ForgetGeometry (uint64_t id);
	/// a texture is destroyed
	void ForgetTexture (VkTexture *tex);

	bool enabled = true;

private:
	struct Blas {
		vkc::Buffer buf;
		VkAccelerationStructureKHR as = VK_NULL_HANDLE;
		VkDeviceAddress addr = 0;
		uint64_t geomVersion = 0; // of the parts' vertices
		uint64_t lastUse = 0;     // frame
	};
	uint64_t frameCount = 0;
	uint32_t TextureSlot (VkTexture *tex);
	void DestroyBlas (Blas &b);

	vkc::Context *ctx = nullptr;
	TextureManager *texmgr = nullptr;
	VkDeviceSize scratchAlign = 256;
	// bottom level: (geometry id of the first part, hash of the parts)
	std::map<std::pair<uint64_t, uint64_t>, Blas> blas;
	struct BlasBuild {
		Blas *b;
		std::vector<VkAccelerationStructureGeometryKHR> geoms;
		std::vector<uint32_t> nPrim;
	};
	std::vector<BlasBuild> builds; // new this frame
	// top level, per frame in flight
	struct Frame {
		vkc::Buffer instances, records, tlasBuf, scratch;
		VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
	} frames[vkc::FRAMES_IN_FLIGHT];
	uint32_t nInst = 0, nRec = 0;
	// bindless textures
	std::unordered_map<VkTexture*, std::pair<uint32_t, VkImageView>> texSlots;
	std::vector<uint32_t> freeSlots;
	uint32_t nextSlot = 0;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE, texLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet texSet = VK_NULL_HANDLE;
	VkImageView defaultView = VK_NULL_HANDLE;
};

#endif // !__RAYTRACING_H

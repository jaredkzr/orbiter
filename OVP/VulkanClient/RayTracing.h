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

class VkMesh;
class VkTexture;
class TextureManager;

class RTScene {
public:
	static constexpr uint32_t MAX_INSTANCES = 16384;
	static constexpr uint32_t MAX_TEXTURES = 4096;

	/// Material of an instance (must match RTInstance in rt.glsl)
	struct Material {
		float diffuse[4];
		float emissive[3];
		float light;        // sunlight reaching the object (eclipses)
		float pbr[4];       // metalness, roughness, reflectivity, F0 scale
		bool unlit;
	};

	bool Init (vkc::Context *ctx, TextureManager *texmgr);
	void Shutdown ();
	bool Enabled () const { return ctx != nullptr; }

	/// Start collecting the instances of this frame
	void BeginFrame ();
	/// A range of a mesh group drawn with the model-view transform
	void AddInstance (const VkMesh *mesh, uint32_t group, uint32_t firstIndex, uint32_t nIndex,
		const Mat4 &modelView, VkTexture *tex, const Material &mat);
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

	/// a mesh was destroyed, or its vertices changed
	void ForgetMesh (uint64_t meshId);
	/// a texture is destroyed
	void ForgetTexture (VkTexture *tex);

	bool enabled = true;

private:
	struct Blas {
		vkc::Buffer buf;
		VkAccelerationStructureKHR as = VK_NULL_HANDLE;
		VkDeviceAddress addr = 0;
		uint32_t version = 0;
	};
	uint32_t TextureSlot (VkTexture *tex);
	void DestroyBlas (Blas &b);

	vkc::Context *ctx = nullptr;
	TextureManager *texmgr = nullptr;
	VkDeviceSize scratchAlign = 256;
	// bottom level: (mesh id, first index, index count)
	std::map<std::tuple<uint64_t, uint32_t, uint32_t>, Blas> blas;
	struct BlasBuild {
		Blas *b;
		VkAccelerationStructureGeometryKHR geom;
		uint32_t nPrim;
	};
	std::vector<BlasBuild> builds; // new this frame
	// top level, per frame in flight
	struct Frame {
		vkc::Buffer instances, records, tlasBuf, scratch;
		VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
	} frames[vkc::FRAMES_IN_FLIGHT];
	uint32_t nInst = 0;
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

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Scene: camera, visual objects and render passes
// =======================================================================

#ifndef __SCENE_H
#define __SCENE_H

#include "VkContext.h"
#include "VkMath.h"
#include <map>
#include <memory>
#include <vector>

class VulkanClient;
class VObject;
class VkMesh;
class StarSphere;
class VkTexture;

/// Per-draw push constants of the mesh pipeline (must match mesh.vert/frag)
struct MeshPushConstants {
	float modelView[16];
	float diffuse[4];
	float emissive[4]; // a: 1 = unlit
	float specular[4]; // a: specular power
};

enum class RenderPass { Opaque, Transparent };

/// Replacement texture for a mesh group, rendered unlit (e.g. VC MFD displays)
struct GroupTexOverride {
	uint32_t group;
	VkTexture *tex;
	bool additive;
};

class Scene {
public:
	explicit Scene (VulkanClient *gc);
	~Scene ();

	bool Init ();
	void Shutdown ();

	/// Per-frame update of the visual list (called from clbkUpdate)
	void Update ();
	/// Record the scene into the current frame's command buffer
	void Render ();

	VObject *GetVisual (OBJHANDLE hObj);
	void DeleteVisual (OBJHANDLE hObj);

	// --- accessed by visual objects during rendering ---
	VulkanClient *Client () const { return gc; }
	VkCommandBuffer Cmd () const;
	const VECTOR3 &CameraPos () const { return camPos; }
	/// object-to-view transform for an object at global position gpos with rotation grot
	Mat4 ModelView (const VECTOR3 &gpos, const MATRIX3 &grot) const;
	void DrawMesh (const VkMesh *mesh, const Mat4 &modelView, RenderPass pass,
		const std::vector<Mat4> *groupTransforms = nullptr, const std::vector<uint8_t> *groupVisible = nullptr,
		const std::vector<GroupTexOverride> *texOverride = nullptr);
	void DrawSphere (const Mat4 &modelView, const float colour[4], bool unlit);
	double PixelSize (double size, double dist) const;

	/// view-space position of a global point
	VECTOR3 ViewPos (const VECTOR3 &gpos) const { return view.TransformPoint (gpos - camPos); }
	/// view-space direction of a global direction
	VECTOR3 ViewDir (const VECTOR3 &gdir) const { return view.TransformPoint (gdir); }
	/// draw view-space billboard triangles (particles, exhaust flames)
	void DrawBillboards (VkTexture *tex, bool additive, const struct PVtx *v, size_t n);
	VkTexture *DefaultExhaustTex () const { return exhaustTex; }

private:
	void CreatePipelines ();
	VkPipeline CreateMeshPipeline (bool blend, bool additive, bool depthWrite);
	VkPipeline CreateParticlePipeline (bool additive);
	void UpdateCamera ();
	void BuildSphere ();

	VulkanClient *gc;
	vkc::Context *ctx = nullptr;

	// pipelines
	VkDescriptorSetLayout frameLayout = VK_NULL_HANDLE;
	VkDescriptorPool framePool = VK_NULL_HANDLE;
	VkDescriptorSet frameSet[vkc::FRAMES_IN_FLIGHT] = {};
	vkc::Buffer frameUbo[vkc::FRAMES_IN_FLIGHT];
	VkPipelineLayout meshLayout = VK_NULL_HANDLE;
	VkPipeline meshOpaque = VK_NULL_HANDLE;
	VkPipeline meshBlend = VK_NULL_HANDLE;
	VkPipeline meshAdditive = VK_NULL_HANDLE;
	VkPipelineLayout starLayout = VK_NULL_HANDLE;
	VkPipeline starPipeline = VK_NULL_HANDLE;
	VkPipelineLayout particleLayout = VK_NULL_HANDLE;
	VkPipeline particleAdditive = VK_NULL_HANDLE;
	VkPipeline particleBlend = VK_NULL_HANDLE;
	VkTexture *exhaustTex = nullptr;

	// sphere geometry for planets
	vkc::Buffer sphereVtx, sphereIdx;
	uint32_t sphereNIdx = 0;

	std::unique_ptr<StarSphere> stars;

	// camera
	VECTOR3 camPos{};
	MATRIX3 camRot{};     // camera frame -> global
	Mat4 view;            // global direction -> view (rotation only)
	float proj[16] = {};
	double tanAperture = 1.0;
	double nearPlane = 0.1;
	uint32_t viewW = 1, viewH = 1;

	// visuals
	std::map<OBJHANDLE, std::unique_ptr<VObject>> visuals;
	OBJHANDLE hSun = nullptr;
};

#endif // !__SCENE_H

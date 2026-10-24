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
class TileRenderer;
class AtmosphereRenderer;
class ShadowMap;

/// Per-draw push constants of the mesh pipeline (must match mesh.vert/frag)
struct MeshPushConstants {
	float modelView[16];
	float diffuse[4];
	float emissive[4]; // a: 1 = unlit
	float specular[4]; // a: specular power
	float light[4];    // rgb: sunlight reaching the object
	float pbr[4];      // metalness, roughness, reflectivity, dielectric F0 scale
};

enum class RenderPass { Opaque, Transparent };

/// Local light source (vessel light emitter), global frame
struct LightSource {
	VECTOR3 pos, dir;
	float col[3];
	double range;
	double att[3];
	bool spot;
	double umbra, penumbra; // full cone angles [rad]
};
constexpr int MAX_LIGHTS = 32; // must match frame.glsl

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
	/// Render the sun shadow map (before the main rendering scope)
	void RenderShadows ();

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

	/// Fraction of the sun's disc visible from a global position (planets
	/// may eclipse it); also sets the sunlight used by subsequent DrawMesh calls
	double Sunlight (const VECTOR3 &gpos);
	void SetObjectLight (double f) { objLight = (float)f; }
	/// emission added to transparent (glass) groups of subsequent DrawMesh calls
	void SetObjectGlassGlow (const float g[3]) { for (int i = 0; i < 3; i++) objGlassGlow[i] = g[i]; }
	/// view-space position of a global point
	VECTOR3 ViewPos (const VECTOR3 &gpos) const { return view.TransformPoint (gpos - camPos); }
	/// view-space direction of a global direction
	VECTOR3 ViewDir (const VECTOR3 &gdir) const { return view.TransformPoint (gdir); }
	/// draw view-space billboard triangles (particles, exhaust flames)
	void DrawBillboards (VkTexture *tex, bool additive, const struct PVtx *v, size_t n);
	VkTexture *DefaultExhaustTex () const { return exhaustTex; }
	/// radial glow sprite (white, falling off to zero at the edge), full uv range
	VkTexture *GlowTex () const { return glowTex; }
	TileRenderer *Tiles () { return tiles.get(); }
	AtmosphereRenderer *Atm () { return atm.get(); }
	const MATRIX3 &CamRot () const { return camRot; }
	double ProjX () const { return proj[0]; }   ///< horizontal focal factor fx
	double ProjY () const { return -proj[5]; }  ///< vertical focal factor fy
	OBJHANDLE Sun () const { return hSun; }
	VkDescriptorSet FrameSet () const { return frameSet[ctx->FrameIndex()]; }
	/// screen pixels per radian at the view centre
	double PixPerRad () const { return 0.5 * viewH / tanAperture; }

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
	VkTexture *glowTex = nullptr;
	std::unique_ptr<TileRenderer> tiles;
	std::unique_ptr<AtmosphereRenderer> atm;
	std::unique_ptr<ShadowMap> shadows;
	bool shadowsValid = false;
	VkDescriptorSet camAtmSet = VK_NULL_HANDLE; // atmosphere for vessels and bases

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
	float objLight = 1.0f;
	float objGlassGlow[3] = { 0, 0, 0 };
};

#endif // !__SCENE_H

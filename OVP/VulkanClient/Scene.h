// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Scene: camera, visual objects and render passes
// =======================================================================

#ifndef __SCENE_H
#define __SCENE_H

#include "VkContext.h"
#include "VkMath.h"
#include "RayTracing.h"
#include "Plumes.h"
#include <algorithm>
#include <cmath>
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
	float heat[4];     // xyz: direction of motion through the air (view), w: heating glow 0..1
	float maps[4];     // x: flags of the additional texture maps present (TexMaps::flags)
};

/// Opaque and transparent geometry of the main pass; Depth: opaque geometry
/// into the ambient occlusion prepass
enum class RenderPass { Opaque, Transparent, Depth };

/// Local light source (vessel light emitter), global frame
struct LightSource {
	VECTOR3 pos, dir;
	float col[3];
	double range;
	double att[3];
	bool spot;
	double umbra, penumbra; // full cone angles [rad]
	uint32_t owner;         // VObject::Id of the vessel carrying it
	double shadowOfs;       // shadow rays start this far along dir (spot light emitters inside the hull)
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
	/// whether a sphere (centre relative to the camera, global frame) may be in view
	bool SphereInView (const VECTOR3 &rel, double radius) const
	{
		double d = length (rel);
		if (d <= radius) return true;
		VECTOR3 fwd = { camRot.m13, camRot.m23, camRot.m33 };
		double cosA = dotp (rel, fwd) / d;
		double half = atan (tanAperture * sqrt (1.0 + (double)viewW * viewW / ((double)viewH * viewH)));
		return acos (std::clamp (cosA, -1.0, 1.0)) < half + asin (std::min (1.0, radius / d));
	}
	/// object-to-view transform for an object at global position gpos with rotation grot
	Mat4 ModelView (const VECTOR3 &gpos, const MATRIX3 &grot) const;
	void DrawMesh (const VkMesh *mesh, const Mat4 &modelView, RenderPass pass,
		const std::vector<Mat4> *groupTransforms = nullptr, const std::vector<uint8_t> *groupVisible = nullptr,
		const std::vector<GroupTexOverride> *texOverride = nullptr);
	/// Instances of a mesh (opaque and alpha-tested groups; depth or opaque
	/// pass): transforms object -> global frame relative to the camera, as
	/// rows of a 3x4 matrix (INSTANCE_STRIDE bytes each), in buffer inst
	/// from byte instOfs. Lit and moving as set for the current object.
	/// frameToView: if set, the transforms are in that frame instead (e.g. a
	/// planet frame relative to an anchor: view * its rotation and position
	/// relative to the camera). impostor: the mesh is a plant's impostor
	/// (impostor_inst.vert), shrunk away over the last 15% of range [m].
	static constexpr uint32_t INSTANCE_STRIDE = 48;
	void DrawMeshInstanced (const VkMesh *mesh, VkBuffer inst, VkDeviceSize instOfs, uint32_t count, RenderPass pass,
		const Mat4 *frameToView = nullptr, bool impostor = false, float range = 0.0f);
	/// A mesh into the ray tracing scene of this frame (without drawing it)
	/// (shadowOnly: a stand-in that only blocks light)
	void AddMeshRT (const VkMesh *mesh, const Mat4 &modelView, bool shadowOnly = false);
	/// the camera's view matrix (global frame relative to the camera -> view)
	const Mat4 &View () const { return view; }
	/// Per-frame uniforms and state (after RenderShadows, before the prepass)
	void BeginFrame ();
	/// Opaque geometry into the ambient occlusion prepass, and the occlusion
	void RenderPrepass ();
	/// Temporal anti-aliasing of the rendered scene (after Render). Returns
	/// the image post-processing reads (the scene image if it is off).
	VkImageView ResolveTemporal (VkExtent2D &size);
	/// incremented at the start of each frame (RenderShadows)
	uint64_t FrameNo () const { return frameNo; }
	void DrawSphere (const Mat4 &modelView, const float colour[4], bool unlit, VkTexture *tex = nullptr);
	VkBuffer SphereVtx () const { return sphereVtx.buffer; }
	VkBuffer SphereIdx () const { return sphereIdx.buffer; }
	uint32_t SphereNIdx () const { return sphereNIdx; }
	/// draw indexed NTVERTEX geometry (32-bit indices) with a texture and
	/// diffuse colour, opaque or alpha-blended
	void DrawGeometry (VkBuffer vb, VkBuffer ib, uint32_t nIdx, const Mat4 &modelView,
		VkTexture *tex, const float colour[4], bool unlit, bool blend);
	double PixelSize (double size, double dist) const;

	/// Fraction of the sun's disc visible from a global position (planets
	/// may eclipse it); also sets the sunlight used by subsequent DrawMesh calls
	double Sunlight (const VECTOR3 &gpos);
	void SetObjectLight (double f) { objLight = (float)f; }
	float ObjectLight () const { return objLight; }
	/// emission added to transparent (glass) groups of subsequent DrawMesh calls
	void SetObjectGlassGlow (const float g[3]) { for (int i = 0; i < 3; i++) objGlassGlow[i] = g[i]; }
	/// aerodynamic heating glow of subsequent DrawMesh calls: windward
	/// direction (view space) and strength 0..1
	void SetObjectHeat (const VECTOR3 &dir, float strength)
	{ objHeat[0] = (float)dir.x, objHeat[1] = (float)dir.y, objHeat[2] = (float)dir.z, objHeat[3] = strength; }
	/// Motion of the next objects drawn into the prepass: their model-view
	/// transform now and in the previous frame (rigid)
	void SetObjectMotion (const Mat4 &modelView, const Mat4 &prevModelView);
	/// model-view transform of the previous frame for an object pose then
	Mat4 PrevModelView (const VECTOR3 &gpos, const MATRIX3 &grot) const;
	/// view space now -> clip space of the previous frame (unjittered)
	const float *ObjectMotion () const { return objMotion; }
	/// distance from the camera within which the 2D cloud layer gives way to
	/// the volumetric clouds (0: none this frame)
	float CloudFadeRadius () const;
	/// temporal anti-aliasing active (projection jitter, motion vectors)
	bool TemporalAAEnabled () const { return taaEnabled; }
	/// view-space position of a global point
	VECTOR3 ViewPos (const VECTOR3 &gpos) const { return view.TransformPoint (gpos - camPos); }
	/// view-space direction of a global direction
	VECTOR3 ViewDir (const VECTOR3 &gdir) const { return view.TransformPoint (gdir); }
	/// global position of a view-space point, global direction of a view-space direction
	VECTOR3 GlobalPos (const VECTOR3 &vpos) const { return camPos + mul (camRot, vpos); }
	VECTOR3 GlobalDir (const VECTOR3 &vdir) const { return mul (camRot, vdir); }
	/// draw view-space billboard triangles (particles, exhaust flames)
	void DrawBillboards (VkTexture *tex, bool additive, const struct PVtx *v, size_t n);
	VkTexture *DefaultExhaustTex () const { return exhaustTex; }
	/// GPU particles of the particle streams
	class ParticleSystem *Particles () { return particles.get(); }
	/// engine plume volumes (null: billboards)
	class PlumeRenderer *Plumes () { return plumes && plumes->enabled ? plumes.get() : nullptr; }
	/// reactive mask of this frame (shader-read layout after Render): where
	/// plumes and particles were drawn, which temporal filters should trust
	/// their history less
	static constexpr VkFormat REACTIVE_FORMAT = VK_FORMAT_R8_UNORM;
	VkImageView ReactiveView () const { return reactive.view; }
	VkImage ReactiveImage () const { return reactive.image; }
	/// radial glow sprite (white, falling off to zero at the edge), full uv range
	VkTexture *GlowTex () const { return glowTex; }
	TileRenderer *Tiles () { return tiles.get(); }
	/// ray tracing scene, if enabled (geometry of the prepass goes into it)
	class RTScene *RT () const;
	AtmosphereRenderer *Atm () { return atm.get(); }
	const MATRIX3 &CamRot () const { return camRot; }
	double ProjX () const { return proj[0]; }   ///< horizontal focal factor fx
	double ProjY () const { return -proj[5]; }  ///< vertical focal factor fy
	OBJHANDLE Sun () const { return hSun; }
	/// the sun's position on the screen (texture coordinates); false if
	/// behind the camera or far off the screen
	bool SunOnScreen (float uv[2]) const;
	/// fraction of the sun's disc visible from the camera
	double CameraSunlight () { return Sunlight (camPos); }
	VkDescriptorSet FrameSet () const { return inProbe ? probeFrameSet[ctx->FrameIndex()] : frameSet[ctx->FrameIndex()]; }
	/// rendering the environment probe (not the main view)
	bool InProbe () const { return inProbe; }
	/// Render one face of the environment probe around the focus vessel
	/// (after the prepass, outside rendering scopes)
	void RenderProbe ();
	/// screen pixels per radian at the view centre
	double PixPerRad () const { return 0.5 * viewH / tanAperture; }

private:
	void CreatePipelines ();
	VkPipeline CreateMeshPipeline (bool blend, bool additive, bool depthWrite, bool prepass = false, bool instanced = false, bool impostor = false);
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
	VkDescriptorSet probeFrameSet[vkc::FRAMES_IN_FLIGHT] = {};
	vkc::Buffer probeFrameUbo[vkc::FRAMES_IN_FLIGHT];
	std::unique_ptr<class EnvProbe> probe;
	bool inProbe = false, probeEnabled = true;
	int probeFace = 0, probeFacesDone = 0;
	OBJHANDLE probeTarget = nullptr;
	void FillFrameUniforms (void *fu);
	void UpdateCamAtmSet ();
	VkPipelineLayout meshLayout = VK_NULL_HANDLE;
	VkPipeline meshOpaque = VK_NULL_HANDLE;
	VkPipeline meshBlend = VK_NULL_HANDLE;
	VkPipeline meshAdditive = VK_NULL_HANDLE;
	VkPipeline meshPrepass = VK_NULL_HANDLE;
	VkPipeline meshOpaqueInst = VK_NULL_HANDLE;  // instanced (ground scatter)
	VkPipeline meshPrepassInst = VK_NULL_HANDLE;
	VkPipeline meshOpaqueImp = VK_NULL_HANDLE;   // instanced impostors (far plants)
	VkPipeline meshPrepassImp = VK_NULL_HANDLE;
	std::unique_ptr<class AmbientOcclusion> ao;
	std::unique_ptr<class ScreenReflections> ssr;
	std::unique_ptr<class RTScene> rt;
	std::unique_ptr<class RTLighting> rtl;
	std::vector<RTScene::Part> rtParts; // static groups of the mesh being drawn into the prepass
	void WriteRTLightDescriptors ();
	static void PlanetAlbedo (OBJHANDLE hp, float alb[3]);
	bool cameraCut = true; // this frame does not continue the previous one
	void WriteSSRDescriptors ();
	std::unique_ptr<class TemporalAA> taa;
	std::unique_ptr<class FsrUpscaler> fsr;
	std::unique_ptr<class VolumetricClouds> vclouds;
	std::unique_ptr<PlumeRenderer> plumes;
	std::unique_ptr<class ParticleSystem> particles;
	vkc::Image reactive, reactiveMS; // (multisampled: drawn to, resolved into reactive)
	uint32_t reactiveGen = ~0u;
	void EnsureReactiveMask ();
	bool cloudsActive = false;   // volumetric clouds this frame
	float ambientRGB[3] = {};
	void RenderVolumetricClouds ();
	bool taaEnabled = true;  // temporal anti-aliasing (jitter, motion vectors)
	bool useFsr = false;     // ... by the FSR 3 upscaler instead of our own
	float jitterPixels[2] = { 0.0f, 0.0f };
	double lastFrameTime = -1.0;
	float objMotion[16] = {};
	float jitter[2] = { 0.0f, 0.0f };     // projection jitter (NDC)
	uint32_t jitterIndex = 0;
	// camera of the previous frame
	bool havePrev = false;
	VECTOR3 prevCamPos{};
	Mat4 prevView;
	float prevProj[16] = {};             // unjittered
	uint64_t frameNo = 0;
	void WriteAODescriptors ();
	VkPipelineLayout starLayout = VK_NULL_HANDLE;
	VkPipeline starPipeline = VK_NULL_HANDLE;
	VkPipelineLayout particleLayout = VK_NULL_HANDLE;
	VkPipeline particleAdditive = VK_NULL_HANDLE;
	VkPipeline particleBlend = VK_NULL_HANDLE;
	VkTexture *exhaustTex = nullptr;
	VkTexture *glowTex = nullptr;
	std::unique_ptr<TileRenderer> tiles;
	std::unique_ptr<AtmosphereRenderer> atm;
	// shadow cascades: around the camera target, then around the camera
	// (mid and far range)
	static constexpr int SHADOW_CASCADES = 3;
	std::unique_ptr<ShadowMap> shadows[SHADOW_CASCADES];
	bool shadowValid[SHADOW_CASCADES] = {};
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
	float objHeat[4] = { 0, 0, 0, 0 };
	uint32_t objOwner = 0;  // VObject::Id of the object being drawn into the prepass
};

#endif // !__SCENE_H

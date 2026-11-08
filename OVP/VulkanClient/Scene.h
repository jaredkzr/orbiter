// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Scene: camera, visual objects and render passes
// =======================================================================

#ifndef __SCENE_H
#define __SCENE_H

#include "VkContext.h"
#include "GalacticFrame.h"
#include "VkMath.h"
#include "RayTracing.h"
#include "Plumes.h"
#include "Plasma.h"
#include "SkySettings.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

class VulkanClient;
class VObject;
class VkMesh;
class StarSphere;
class StarField;
class GalaxySky;
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
	/// (relativistic view: where it is seen, aberrated, its size seen 1/D times)
	bool SphereInView (const VECTOR3 &rel, double radius) const
	{
		double d = length (rel);
		if (d <= radius) return true;
		VECTOR3 fwd = { camRot.m13, camRot.m23, camRot.m33 };
		VECTOR3 n = rel / d;
		double grow = 1.0;
		if (relOn) {
			double b2 = dotp (relBeta, relBeta), g = 1.0 / sqrt (1.0 - b2), nb = dotp (n, relBeta);
			n = (n + relBeta * (g * g / (g + 1.0) * nb + g)) / (g * (1.0 + nb));
			grow = 1.0 / (g * (1.0 + nb)); // (1/D: angles near n magnified)
		}
		double cosA = dotp (n, fwd) / length (n);
		double half = atan (tanAperture * sqrt (1.0 + (double)viewW * viewW / ((double)viewH * viewH)));
		return acos (std::clamp (cosA, -1.0, 1.0)) < half + std::min (3.2, 1.2 * grow * asin (std::min (1.0, radius / d)));
	}
	/// Relativistic view: the camera's target faster than REL_VIEW_MIN (0.001 c):
	/// the scene seen aberrated and Doppler shifted, objects where their light left
	/// them. ORBITER_VK_RELVIEW=0: off; ORBITER_VK_RELVIEW_BETA=b (along the view)
	/// or bx,by,bz (global frame): a velocity [c] forced (tests, pictures)
	bool RelView () const { return relOn; }
	VkImageView PrepassView () const; ///< linear view depth in w (the relativistic view's vessel pixels)
	/// Relativistic view: a radiance (linear RGB) of something at rest at
	/// gpos (global), as seen: Doppler shifted (unchanged when off). For
	/// what is not in the prepass (the Sun): the rest is shifted after the scene
	void RelShiftColour (const VECTOR3 &gpos, float rgb[3]) const;
	/// the camera's own vessel in view space: centre, radius (0: none)
	void RelSelfView (float out[4]) const;
	const VECTOR3 &RelBeta () const { return relBeta; } ///< the camera's velocity [c], global frame (0: off)
	VECTOR3 RelBetaView () const { return relOn ? view.TransformPoint (relBeta) : _V(0, 0, 0); } ///< in view space
	/// where an object is seen: where it was when its light left it
	/// (oapiGetObservedPos: in the relativistic view the global frame's,
	/// aberrated when drawn; slower, only objects fast relative to the camera)
	void ObservedPos (OBJHANDLE h, VECTOR3 &p) const;
	/// object-to-view transform for an object at global position gpos with rotation grot
	Mat4 ModelView (const VECTOR3 &gpos, const MATRIX3 &grot) const;
	void DrawMesh (const VkMesh *mesh, const Mat4 &modelView, RenderPass pass,
		const std::vector<Mat4> *groupTransforms = nullptr, const std::vector<uint8_t> *groupVisible = nullptr,
		const std::vector<GroupTexOverride> *texOverride = nullptr, const std::vector<Mat4> *prevGroupTransforms = nullptr);
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
		const Mat4 *frameToView = nullptr, bool impostor = false, float range = 0.0f, const float *tint = nullptr);
	/// Buildings of the procedural cities (CityKit; building.vert): indices
	/// [firstIndex, +nIndex) of a shape in ib (vertices in vb,
	/// BUILDING_VERTEX bytes each), count instances (BUILDING_INSTANCE bytes
	/// each) in inst from byte instOfs, in the frame frameToView maps to the
	/// view; kit: the set of the kit's material array; hour: the local
	/// solar time [h] (lit windows at night). Lit and moving as set for the
	/// current object.
	static constexpr uint32_t BUILDING_VERTEX = 52, BUILDING_INSTANCE = 80;
	void DrawBuildings (VkBuffer vb, VkBuffer ib, uint32_t firstIndex, uint32_t nIndex, VkBuffer inst, VkDeviceSize instOfs,
		uint32_t count, RenderPass pass, const Mat4 &frameToView, VkDescriptorSet kit, float hour = 12.0f);
	/// Street lamps of the cities (lamp.vert): count instances in buf (four
	/// floats each: position, LED or not) in the frame frameToView maps to
	/// the view, with origin centre (planet frame); sun: direction to the sun
	/// (planet frame); drawn from fadeFrom fading out to range [m], halfSize
	/// [m], brightness bright
	void DrawLamps (VkBuffer buf, uint32_t count, const Mat4 &frameToView, const VECTOR3 &centre, const VECTOR3 &sun,
		float fadeFrom, float range, float halfSize, float bright);
	/// The road map of the cities around the camera (CityField; tile
	/// shading): textures of its fine and coarse level (r: asphalt, g:
	/// pavement; null: none), on the tangent plane at centre (planet frame)
	/// with axes X, Y (planet frame, unit), their sizes [m]
	void SetCityMap (VkTexture *fine, VkTexture *coarse, VkTexture *far, OBJHANDLE hPlanet, const VECTOR3 &centre,
		const VECTOR3 &X, const VECTOR3 &Y, double fineSize, double coarseSize, double farSize);
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
	/// planetary rings (shadow: their shadow over the planet's sphere,
	/// multiplied); ring: inner and outer radius [planet radii], sunlight
	/// factor, texture layout (0 radial, 1 legacy); legacy: its fo, fi, nrad
	void DrawRing (VkBuffer vb, VkBuffer ib, uint32_t nIdx, const Mat4 &modelView, VkTexture *tex,
		const float ring[4], const float legacy[4], bool shadow);
	void DrawGeometry (VkBuffer vb, VkBuffer ib, uint32_t nIdx, const Mat4 &modelView,
		VkTexture *tex, const float colour[4], bool unlit, bool blend);
	double PixelSize (double size, double dist) const;

	/// Fraction of the sun's disc visible from a global position (planets
	/// may eclipse it); also sets the sunlight used by subsequent DrawMesh calls
	double Sunlight (const VECTOR3 &gpos);
	/// the same, without setting the light of the objects drawn next
	/// (exclude: a body not counted, e.g. the one the point is on)
	double SunVisibility (const VECTOR3 &gpos, OBJHANDLE exclude = nullptr) const;
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
	bool CloudsActive () const { return cloudsActive; }
	/// sunlight reaching a point (global) through a gas giant's cloud decks
	/// and gas (the planet near the camera; 1 elsewhere): the direct beam
	/// and the diffuse light, or (direct) the beam only
	double DeckTransmittance (const VECTOR3 &gpos, bool direct = false) const;
	/// light of a point source at a (global) reaching b through a gas
	/// giant's decks and gas (green): the unscattered share (direct) and the
	/// share diffused by the clouds on the way (diffuse); false if no decks
	bool DeckPath (const VECTOR3 &a, const VECTOR3 &b, double &direct, double &diffuse) const;
	const class VolumetricClouds *VClouds () const { return vclouds.get(); }
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
	/// (seen: the vertices are where they are seen already: not aberrated
	/// again in the relativistic view)
	void DrawBillboards (VkTexture *tex, bool additive, const struct PVtx *v, size_t n, bool seen = false);
	/// Relativistic view: where a point at rest at view position p is seen
	/// (aberrated, at the distance of its light's emission; unchanged when off)
	VECTOR3 RelApparentView (const VECTOR3 &p) const;
	VkTexture *DefaultExhaustTex () const { return exhaustTex; }
	/// GPU particles of the particle streams
	class ParticleSystem *Particles () { return particles.get(); }
	/// engine plume volumes (null: billboards)
	class PlumeRenderer *Plumes () { return plumes && plumes->enabled ? plumes.get() : nullptr; }
	class GroundTracks *Tracks () { return tracks.get(); }
	/// reentry plasma sheaths (null: off)
	class PlasmaRenderer *Plasma () { return plasma && plasma->enabled ? plasma.get() : nullptr; }
	/// particle textures of fires and sparks (made when first needed)
	class VkTexture *FireTexture ();
	class VkTexture *SparkTexture ();
	class VkTexture *FragmentTexture (); ///< small bits of debris: a shard in each quadrant (white, hard edged)
	/// a fireball's flames by their temperature (0: white-yellow core, 1: orange, 2: dark red), ragged;
	/// 3: a soft white puff without billows (vapour)
	class VkTexture *FireballTexture (int stage);
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
	/// the sky seen through a black hole with an accretion disc (the exposure adapts to it)
	bool LensDisc () const { return lensOn && lensDisc; }

	/// A star's disc (Shaders/star.frag): its photosphere's radiance at the
	/// disc's centre rgb (scene, relativistic shift applied), modelView with
	/// its rotation and radius; its temperature [K] (activity: spots,
	/// flares), seed (its pattern)
	/// a star's photosphere (Shaders/star.frag); modelView: its frame (an
	/// ellipsoid: drawn out by a tide); tide: towards the companion in that
	/// frame (null: none)
	/// (roche: filling its Roche lobe about the hole, the tide's direction; the mesh scaled by star.frag's ROCHE_BOX)
	void DrawStar (const Mat4 &modelView, const float rgb[3], double teff, double radius, uint32_t seed, const VECTOR3 *tide = nullptr, bool roche = false);
	/// a star's flares now (cool dwarfs: Proxima Centauri's ~ a few an hour,
	/// rare large ones), deterministic in time: the two brightest (direction
	/// in its frame, brightness relative to its disc's centre), the light's
	/// rise (1: none)
	struct StarFlare { VECTOR3 dir{}; double bright = 0.0; };
	static void StarFlares (uint32_t seed, double teff, double t, StarFlare out[2], double &boost);

	/// scene radiance of a star's surface as bright as the Sun's (a disc far
	/// brighter than any lit surface; the scale of the stars and accretion
	/// discs drawn as bodies)
	static constexpr float STAR_BRIGHTNESS = 20.0f;

	/// Interstellar travel: the stars drawn as bodies (discs and glare) and
	/// the light: a star is a body while its light at the camera is at least
	/// the Sun's at 100 AU (NEAR_FLUX of the Sun's at 1 AU), else a point of
	/// the star field. The light is the brightest of them (the Sun, else a
	/// named star near the camera: Orbiter's core, oapiGetNamedStar), in its
	/// colour, fading as its flux falls below NEAR_FLUX.
	static constexpr double NEAR_FLUX = 1e-4;
	/// a star's glare sprite's half size at view depth z: glowSize radii, at
	/// least minAngle; near the star (its disc large) at most reaching
	/// STAR_GLARE_REACH beyond its limb (the disc's own light blooms)
	static double StarGlowSize (double radius, double z, double glowSize, double minAngle)
	{
		return std::min (std::max (radius * glowSize, z * minAngle), radius + z * STAR_GLARE_REACH);
	}
	static constexpr double STAR_GLARE_REACH = 0.12;
	/// a star's glare sprite's strength by its disc's radius in pixels: whole
	/// for a small disc, gone for a large one (its own light blooms)
	static double StarGlowFade (double pixelRadius) { return 1.0 - std::clamp ((pixelRadius - 20.0) / 60.0, 0.0, 1.0); }
	/// the angular radius [rad] of the largest star disc (the Sun's or a near
	/// star's) in or about the view (0: none); the exposure adapts to a large one
	double StarDiscAngle (double *teff = nullptr) const;
	struct StarLight {
		VECTOR3 pos{};          ///< global position
		double radius = 0.0;    ///< [m]
		double teff = 5772.0;   ///< [K]
		double flux = 1.0;      ///< V band, relative to the Sun's at 1 AU
		bool sun = true;        ///< the Sun (Orbiter's star body)
		bool disc = false;      ///< a black hole's accretion disc
		double flare = 1.0;     ///< the light's rise in a flare (Scene::StarFlares)
		OBJHANDLE body = nullptr; ///< its star body, if any
	};
	const StarLight &Light () const { return light; }
	/// further lights (another star of a binary, a disc), brightest first:
	/// diffuse light, ray traced shadows (Shaders/lights.glsl, type 3)
	const std::vector<StarLight> &ExtraLights () const { return extraLights; }
	static constexpr size_t MAX_EXTRA_LIGHTS = 3;
	/// (an extra light's least flux, of the light's: its light 1/1000 of the
	/// light's after the compression, EXTRA_LIGHT_GAMMA)
	static constexpr double EXTRA_LIGHT_MIN = 1e-4;
	/// extra lights' brightness relative to the light: their flux ratio to
	/// this power (the eye's compression, as the light's own)
	static constexpr double EXTRA_LIGHT_GAMMA = 0.75;
	bool SunAsBody () const { return sunAsBody; }
	/// a black body's disc radiance (RGB) relative to the Sun's (5772 K)
	static void StarDiscRGB (double teff, float rgb[3]);
	/// the sun's position on the screen (texture coordinates); false if
	/// behind the camera or far off the screen
	bool SunOnScreen (float uv[2]) const;
	/// fraction of the sun's disc visible from the camera
	double CameraSunlight () { return Sunlight (camPos); }
	/// The eye's adaptation for the sky: the scene's metered luminance
	/// (log2, of the last frames) dims the Milky Way seen against it
	void SkyAdapt (float avgLogLum, double dt);
	/// Orbiter's celestial sphere options have changed
	void SkyOptionsChanged ();
	/// The graphics settings that apply at once (GfxSettings) changed
	void ApplySettings ();
	VkDescriptorSet FrameSet () const { return inProbe ? probeFrameSet[ctx->FrameIndex()] : frameSet[ctx->FrameIndex()]; }
	/// rendering the environment probe (not the main view)
	bool InProbe () const { return inProbe; }
	void GpuMark (const char *name) { if (!inProbe) ctx->GpuMark (name); }
	/// Render one face of the environment probe around the focus vessel
	/// (after the prepass, outside rendering scopes)
	void RenderProbe ();
	/// screen pixels per radian at the view centre
	double PixPerRad () const { return 0.5 * viewH / tanAperture; }
	uint32_t ViewHeight () const { return viewH; } ///< [pixels]

private:
	void CreatePipelines ();
	enum class MeshShader { Mesh, Ring, RingShadow, Building, Star };
	VkPipeline CreateMeshPipeline (bool blend, bool additive, bool depthWrite, bool prepass = false, bool instanced = false, bool impostor = false,
		MeshShader shader = MeshShader::Mesh);
	VkPipeline CreateParticlePipeline (bool additive, bool lamps = false);
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
	VkPipeline starPipe = VK_NULL_HANDLE; // (star discs: star.frag)
	VkPipeline meshOpaque = VK_NULL_HANDLE;
	VkPipeline meshBlend = VK_NULL_HANDLE;
	VkPipeline meshAdditive = VK_NULL_HANDLE;
	VkPipeline ringPipe = VK_NULL_HANDLE, ringShadowPipe = VK_NULL_HANDLE;
	VkPipeline meshPrepass = VK_NULL_HANDLE;
	VkPipeline meshOpaqueInst = VK_NULL_HANDLE;  // instanced (ground scatter)
	VkPipeline meshPrepassInst = VK_NULL_HANDLE;
	VkPipeline meshOpaqueImp = VK_NULL_HANDLE;   // instanced impostors (far plants)
	VkPipeline meshPrepassImp = VK_NULL_HANDLE;
	VkPipeline buildOpaque = VK_NULL_HANDLE;     // buildings of the cities
	VkPipeline buildPrepass = VK_NULL_HANDLE;
	VkPipeline lampPipe = VK_NULL_HANDLE;         // street lamps
	VkPipelineLayout lampLayout = VK_NULL_HANDLE;
	std::unique_ptr<class AmbientOcclusion> ao;
	std::unique_ptr<class ScreenReflections> ssr;
	std::unique_ptr<class RTScene> rt;
	std::unique_ptr<class RTLighting> rtl;
	std::vector<RTScene::Part> rtParts; // static groups of the mesh being drawn into the prepass
	void WriteRTLightDescriptors ();
	static void PlanetAlbedo (OBJHANDLE hp, float alb[3]);
	bool cameraCut = true; // this frame does not continue the previous one
	void WriteSSRDescriptors ();
	void WriteCityDescriptors ();
	VkTexture *cityMap[3] = {}, *cityNone = nullptr;
	VkTexture *cityBound[vkc::FRAMES_IN_FLIGHT][3];
	VkSampler citySampler = VK_NULL_HANDLE;
	OBJHANDLE cityPlanet = nullptr;
	VECTOR3 cityCentre{}, cityX{}, cityY{};
	double citySize[3] = { 1.0, 1.0, 1.0 };
	std::unique_ptr<class TemporalAA> taa;
	std::unique_ptr<class FsrUpscaler> fsr;
	std::unique_ptr<class VolumetricClouds> vclouds;
	std::unique_ptr<class GroundTracks> tracks;
	std::unique_ptr<PlumeRenderer> plumes;
	std::unique_ptr<class PlasmaRenderer> plasma;
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
	// SunVisibility of points on a body: the bodies that can eclipse the
	// light somewhere near it, this frame (each tile asked about all the
	// bodies, in every pass)
	struct EclipseCands { uint64_t frame = ~0ull; VECTOR3 lightPos{}; std::vector<OBJHANDLE> bodies; };
	mutable std::map<OBJHANDLE, EclipseCands> eclipseCands;
	void WriteAODescriptors ();
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
	std::unique_ptr<GalaxySky> galaxy;
	std::unique_ptr<StarField> starField; // (the catalogue's stars)
	std::unique_ptr<class BlackHoleLens> lens; // (the sky seen near a black hole)
	bool lensOn = false;   // this frame's sky through a black hole's spacetime
	bool lensDisc = false; // (with its accretion disc)
	VECTOR3 lensPos{};     // (its position, global)
	double lensM = 0.0;    // (its mass [m])
	OBJHANDLE lensDonor = nullptr; // (its disc's donor star: drawn in the lensed sky too)
	void UpdateLens ();
	SkySettings skySettings;
	double skyDim = 0.0;      // the sky's dimming by light adaptation [stops]
	double skyDarkGain = 0.0; // scene radiance of 1 S10 to the dark-adapted eye
	galactic::Observer SkyObserver () const;

	// interstellar travel: named stars drawn as bodies, the light
	struct NearStar { int index; uint32_t seed; VECTOR3 pos; double radius, teff, flux, dist; OBJHANDLE body; }; // (index: the star field's, -1: none)
	std::vector<NearStar> nearStars;
	StarLight light;
	std::vector<StarLight> extraLights;
	bool sunAsBody = true;
	void UpdateNearStars ();
	void RenderNearStars (bool transparent);
	/// a star feeding a black hole's disc (its donor): its frame, x towards the hole, y the orbit's axis
	bool DonorFrame (OBJHANDLE body, const VECTOR3 &pos, VECTOR3 &x, VECTOR3 &y) const;

	// camera
	VECTOR3 camPos{};
	MATRIX3 camRot{};     // camera frame -> global
	void UpdateRelativity ();
	bool relOn = false;   // relativistic view
	VECTOR3 relBeta{};    // the camera's velocity [c], global frame
	VECTOR3 camVel{};     // the camera's velocity [m/s] the light-time is taken for (ObservedPos)
	OBJHANDLE relSelf = nullptr; // the camera's own vessel (moving with it)
	double relSelfRad = 0.0;
	VECTOR3 shakeLP{}, shakeV{}; // the cockpit view's jolts: the vessel's aerodynamic acceleration, low-passed; the jolt, smoothed
	Mat4 view;            // global direction -> view (rotation only)
	float proj[16] = {};
	double tanAperture = 1.0;
	double nearPlane = 0.1;
	uint32_t viewW = 1, viewH = 1;

	// visuals
	std::map<OBJHANDLE, std::unique_ptr<VObject>> visuals;
	class VkTexture *fireTex = nullptr, *sparkTex = nullptr, *fragTex = nullptr, *fireballTex[6] = {};
	OBJHANDLE hSun = nullptr;
	float objLight = 1.0f;
	float objGlassGlow[3] = { 0, 0, 0 };
	float objHeat[4] = { 0, 0, 0, 0 };
	uint32_t objOwner = 0;  // VObject::Id of the object being drawn into the prepass
	float objVel[3] = { 0, 0, 0 }; // and its velocity (global frame)
};

#endif // !__SCENE_H

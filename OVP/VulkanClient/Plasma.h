// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Reentry plasma: the shock layer's gas glowing around a vessel entering
// an atmosphere, and the sheets of it that peel off the windward hull's
// outline and stream downstream.
//
// Drawn as a volume in the flow's frame (x, y across the flow, s along it,
// in units of the vessel's size). Each frame, for each heated vessel:
// - its meshes are drawn seen along the flow into a tile of an atlas (the
//   flow map: the windward and the leeward surface's s at each point of
//   its outline seen from upstream)
// - jump flooding gives each point of the tile its distance to the
//   outline and the surfaces' s where the flow leaves it (the field)
// - the ray through each pixel is marched through the box around the
//   vessel and its wake, up to the opaque scene (the prepass depth), the
//   gas's emission by the field: the shock layer ahead of the windward
//   surface (white-yellow at the hull, pink, magenta, violet at its outer
//   edge), the sheets streaming off the outline (widening, cooling and
//   fading downstream), the dim lee wake behind it; streaks along the flow
//   (plasma_vol.frag). Additive, with the reactive mask.
// More heated hulls than there are tiles: the rest as a shell (the mesh
// pushed out ahead of its windward side, plasma.frag).
// The hull's own glow (mesh.frag, material heat) is a dull red at its
// stagnation regions; the gas lights the vessel (VVessel::UpdateLights).
// =======================================================================

#ifndef __PLASMA_H
#define __PLASMA_H

#include "VkContext.h"
#include "VkMath.h"
#include <vector>

class VkMesh;

/// The look of a vessel's sheath this frame
struct SheathState {
	double flux = 0.0;      // stagnation-point heat flux [W/m^2]
	float level = 0.0f;     // heating: 0 (onset, 5e4 W/m^2) .. 1 (peak, 8e5 W/m^2 and more)
	float glow = 0.0f;      // the gas's glow: 0 (onset, 3e4 W/m^2) .. 1 (5e5 W/m^2), by the log of the flux
	VECTOR3 dirGlobal{};    // direction of motion through the air (global frame)
	VECTOR3 dirView{};      // ... (view space)
	/// heating level and glow of a flux [W/m^2]
	static float Level (double flux);
	static float Glow (double flux);
	/// colour of the hull's heat and the shell's glow (radiance per unit of intensity) at heating level t
	static void Colour (float t, float rgb[3]);
	/// colour of the gas's light (on the vessel and what is near it)
	static void GasColour (float glow, float rgb[3]);
	/// thickness of the shell [m] for a vessel of radius size
	double Thickness (double size) const;
	/// mean radiance of the windward glow (for its light)
	float Radiance () const;
};

class PlasmaRenderer {
public:
	bool Init (vkc::Context *ctx, VkDescriptorSetLayout frameLayout, VkImageView detailNoise);
	void Shutdown ();
	/// the opaque scene's view depth (Scene::BeginFrame)
	void SetPrepass (VkImageView prepass);
	/// start of the frame's sheaths (before the transparent pass resumes)
	void Begin (VkDescriptorSet frameSet, double simt);
	/// a heated vessel: its sheath, radius size [m], its centre (view);
	/// returns its index for AddMesh
	int AddSheath (const SheathState &s, double size, const VECTOR3 &centreView);
	/// a mesh of sheath i: model-view, group transforms and visibility as
	/// Scene::DrawMesh
	void AddMesh (int i, const VkMesh *mesh, const Mat4 &modelView, const std::vector<Mat4> *groupTransforms,
		const std::vector<uint8_t> *groupVisible);
	/// sheaths to be marched: Prepare first (no rendering scope bound)
	bool Pending () const { return enabled && !sheaths.empty (); }
	/// the flow maps and their fields (outside rendering scopes)
	void Prepare ();
	/// the volumes and the shells (scene with the reactive mask bound)
	void Draw ();
	/// sheaths drawn this frame
	uint32_t Count () const { return count; }
	bool enabled = true;

private:
	struct MeshRef {
		const VkMesh *mesh;
		Mat4 mv;
		const std::vector<Mat4> *grpT;
		const std::vector<uint8_t> *grpVis;
	};
	struct Sheath {
		SheathState s;
		double size;
		VECTOR3 centre;
		double px;               // its size on the screen (priority)
		int tile = -1;           // in the atlas; -1: a shell
		std::vector<MeshRef> meshes;
		Mat4 toFlow;             // view -> flow frame (units of size)
	};
	void DrawShell (const Sheath &sh);
	bool AllocAtlas ();
	void FreeAtlas ();
	void WriteSets ();

	vkc::Context *ctx = nullptr;
	// the shell
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	// the flow maps (raster), the field (compute), the march
	VkPipelineLayout flowLayout = VK_NULL_HANDLE, jfaLayout = VK_NULL_HANDLE, volLayout = VK_NULL_HANDLE;
	VkPipeline flowPipe = VK_NULL_HANDLE, jfaPipe = VK_NULL_HANDLE, volPipe = VK_NULL_HANDLE;
	VkDescriptorSetLayout jfaSetLayout = VK_NULL_HANDLE, volSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet jfaSet = VK_NULL_HANDLE, volSet = VK_NULL_HANDLE;
	VkSampler linearClamp = VK_NULL_HANDLE, nearestClamp = VK_NULL_HANDLE, repeatSampler = VK_NULL_HANDLE;
	vkc::Image flowMap, seedA, seedB, field;
	bool atlasAlloc = false;
	double idleSince = 0.0;
	VkImageView prepassView = VK_NULL_HANDLE, detailView = VK_NULL_HANDLE;

	VkDescriptorSet frameSet = VK_NULL_HANDLE;
	float time = 0.0f;
	uint32_t frameNo = 0;
	double lastSimT = -1e30;
	uint32_t count = 0;
	bool prepared = false;
	std::vector<Sheath> sheaths;
	// (ORBITER_VK_PLASMA_DUMP=<file>: the field's atlas written once, raw half floats)
	vkc::Buffer dumpBuf;
	int dumpState = 0; // 0: none asked, 1: to copy, 2: copied (written at the next Begin), 3: done
};

#endif // !__PLASMA_H

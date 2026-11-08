// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Reentry plasma: the shock layer ahead of a vessel's windward hull
// glowing as it enters an atmosphere. Drawn as a shell: the vessel's
// meshes again, their windward side pushed out along the normals by a few
// per cent of its size, emitting by the stagnation-point heat flux (the
// Sutton-Graves estimate of VVessel::SetHeating), additive, in the
// transparent pass after the engine plumes (with the reactive mask).
// The hull's own glow (mesh.frag, material heat) stays as it is.
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
	float level = 0.0f;     // 0 (onset, 5e4 W/m^2) .. 1 (peak, 8e5 W/m^2 and more)
	VECTOR3 dirGlobal{};    // direction of motion through the air (global frame)
	VECTOR3 dirView{};      // ... (view space)
	/// colour of the glow (radiance per unit of intensity) at heating level t
	static void Colour (float t, float rgb[3]);
	/// thickness of the layer [m] for a vessel of radius size
	double Thickness (double size) const;
	/// mean radiance of the windward glow (for its light)
	float Radiance () const;
};

class PlasmaRenderer {
public:
	bool Init (vkc::Context *ctx, VkDescriptorSetLayout frameLayout);
	void Shutdown ();
	/// start of the frame's sheaths (scene with the reactive mask bound)
	void Begin (VkDescriptorSet frameSet, double simt);
	/// a mesh of a heated vessel: model-view, group transforms and visibility
	/// as Scene::DrawMesh; size: the vessel's radius [m]
	void DrawMesh (const VkMesh *mesh, const Mat4 &modelView, const std::vector<Mat4> *groupTransforms,
		const std::vector<uint8_t> *groupVisible, const SheathState &s, double size);
	/// sheaths drawn this frame
	uint32_t Count () const { return count; }
	bool enabled = true;

private:
	vkc::Context *ctx = nullptr;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	VkDescriptorSet frameSet = VK_NULL_HANDLE;
	bool bound = false;
	float time = 0.0f;
	uint32_t count = 0;
	const void *lastState = nullptr;
};

#endif // !__PLASMA_H

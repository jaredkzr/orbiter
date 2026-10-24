// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Sun shadow map: an orthographic depth map from the sun around the
// camera's target, rendered from vessels and base structures and
// sampled by all lit surfaces
// =======================================================================

#ifndef __SHADOWS_H
#define __SHADOWS_H

#include "VkContext.h"
#include "VkMath.h"
#include <vector>

class VkMesh;

class ShadowMap {
public:
	static constexpr uint32_t SIZE = 4096;

	bool Init (vkc::Context *ctx);
	void Shutdown ();
	VkImageView View () const { return depth.view; }
	VkSampler Sampler () const { return sampler; }

	/// Start the shadow pass for the region of the given radius around a
	/// global position, lit from the direction sunDir (unit vector to the sun)
	void Begin (const VECTOR3 &centre, double radius, const VECTOR3 &sunDir);
	void End ();
	bool Active () const { return active; }

	/// Draw a caster. model: object -> global frame relative to the shadow
	/// centre (see Centre)
	void DrawMesh (const VkMesh *mesh, const Mat4 &model,
		const std::vector<Mat4> *groupTransforms = nullptr, const std::vector<uint8_t> *groupVisible = nullptr);
	const VECTOR3 &Centre () const { return centre; }
	double Radius () const { return radius; }

	/// Receiver transform: view space -> shadow map texture coordinates
	/// and depth, for a camera at camPos with rotation camRot (camera -> global)
	Mat4 ReceiverMatrix (const VECTOR3 &camPos, const MATRIX3 &camRot) const;
	double TexelSize () const { return 2.0 * radius / SIZE; } ///< [m]

private:
	vkc::Context *ctx = nullptr;
	vkc::Image depth;
	VkSampler sampler = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	bool active = false;
	VECTOR3 centre{};
	double radius = 1.0;
	Mat4 lightVP;  // global (relative to centre) -> shadow clip space
};

#endif // !__SHADOWS_H

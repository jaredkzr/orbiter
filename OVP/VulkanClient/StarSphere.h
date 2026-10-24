// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Celestial sphere: background stars
// =======================================================================

#ifndef __STARSPHERE_H
#define __STARSPHERE_H

#include "VkContext.h"
#include "CelSphereAPI.h"

class VulkanClient;

class StarSphere : public oapi::CelestialSphere {
public:
	explicit StarSphere (VulkanClient *gc);

	void Init (vkc::Context *ctx);
	void Shutdown ();
	void Render (VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, const float *viewProj);

protected:
	bool EclDir2WindowPos (const VECTOR3 &dir, int &x, int &y) const override;

private:
	vkc::Context *ctx = nullptr;
	vkc::Buffer vbuf;
	uint32_t nStar = 0;
	float viewProj[16] = {};
	uint32_t viewW = 1, viewH = 1;
};

#endif // !__STARSPHERE_H

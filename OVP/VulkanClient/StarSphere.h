// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Celestial sphere: Orbiter's markers and labels (the stars and the
// Milky Way are drawn by StarField and GalaxySky)
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
	/// the frame's view and projection, for the markers
	void SetView (const float *viewProj);

protected:
	bool EclDir2WindowPos (const VECTOR3 &dir, int &x, int &y) const override;

private:
	vkc::Context *ctx = nullptr;
	float viewProj[16] = {};
	uint32_t viewW = 1, viewH = 1;
};

#endif // !__STARSPHERE_H

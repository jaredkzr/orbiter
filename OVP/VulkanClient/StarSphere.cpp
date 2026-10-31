// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "StarSphere.h"
#include "VulkanClient.h"
#include <cstring>

StarSphere::StarSphere (VulkanClient *gc)
	: oapi::CelestialSphere (gc)
{
}

void StarSphere::Init (vkc::Context *c)
{
	ctx = c;
}

void StarSphere::Shutdown ()
{
	ctx = nullptr;
}

void StarSphere::SetView (const float *vp)
{
	memcpy (viewProj, vp, sizeof(viewProj));
	VkExtent2D ext = ctx->RenderExtent();
	viewW = ext.width, viewH = ext.height;
}

bool StarSphere::EclDir2WindowPos (const VECTOR3 &dir, int &x, int &y) const
{
	// clip = viewProj * (dir, 0)
	const float *m = viewProj;
	double cx = m[0]*dir.x + m[4]*dir.y + m[8]*dir.z;
	double cy = m[1]*dir.x + m[5]*dir.y + m[9]*dir.z;
	double cw = m[3]*dir.x + m[7]*dir.y + m[11]*dir.z;
	if (cw <= 0.0) return false; // behind the camera
	x = (int)((cx / cw * 0.5 + 0.5) * viewW);
	y = (int)((cy / cw * 0.5 + 0.5) * viewH);
	return x >= 0 && x < (int)viewW && y >= 0 && y < (int)viewH;
}

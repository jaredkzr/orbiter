// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "StarSphere.h"
#include "VulkanClient.h"
#include <cstring>
#include <vector>

namespace {
	struct StarPush {
		float viewProj[16];
		float pointSize;
	};
}

StarSphere::StarSphere (VulkanClient *gc)
	: oapi::CelestialSphere (gc)
{
}

void StarSphere::Init (vkc::Context *c)
{
	ctx = c;
	const std::vector<StarRenderRec> rec = LoadStars ();
	std::vector<float> vtx;
	vtx.reserve (rec.size() * 6);
	for (auto &s : rec) {
		vtx.push_back ((float)s.pos.x); vtx.push_back ((float)s.pos.y); vtx.push_back ((float)s.pos.z);
		vtx.push_back ((float)s.col.x); vtx.push_back ((float)s.col.y); vtx.push_back ((float)s.col.z);
	}
	nStar = (uint32_t)rec.size();
	if (nStar)
		vbuf = ctx->CreateDeviceBuffer (vtx.data(), vtx.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	oapiWriteLogV ("VulkanClient: %u stars loaded", nStar);
}

void StarSphere::Shutdown ()
{
	if (ctx) ctx->DestroyBuffer (vbuf);
	ctx = nullptr;
}

void StarSphere::Render (VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, const float *vp)
{
	memcpy (viewProj, vp, sizeof(viewProj));
	VkExtent2D ext = ctx->RenderExtent();
	viewW = ext.width, viewH = ext.height;
	if (!nStar) return;

	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	StarPush pc;
	memcpy (pc.viewProj, vp, sizeof(pc.viewProj));
	pc.pointSize = 1.0f;
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
	VkDeviceSize ofs = 0;
	vkCmdBindVertexBuffers (cmd, 0, 1, &vbuf.buffer, &ofs);
	vkCmdDraw (cmd, nStar, 1, 0, 0);
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

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Orbiter graphics client using Vulkan for rendering and SDL3 for the
// render window and user input.
// =======================================================================

#ifndef __VULKANCLIENT_H
#define __VULKANCLIENT_H

#include "GraphicsAPI.h"
#include "VkContext.h"
#include "VkTexture.h"
#include <memory>
#include <string>

struct SDL_Window;
class Scene;

class VulkanClient : public oapi::GraphicsClient {
public:
	explicit VulkanClient (HINSTANCE hInstance);
	~VulkanClient ();

	vkc::Context *Ctx () { return &ctx; }
	TextureManager *TexMgr () { return &texmgr; }

	// --- initialisation and session ---
	bool clbkInitialise () override;
	HWND clbkCreateRenderWindow () override;
	void clbkPostCreation () override;
	void clbkCloseSession (bool fastclose) override;
	void clbkDestroyRenderWindow (bool fastclose) override;
	bool clbkUseLaunchpadVideoTab () const override { return false; }

	// --- per-frame ---
	void clbkUpdate (bool running) override;
	void clbkRenderScene () override;
	bool clbkDisplayFrame () override;

	// --- render parameters ---
	bool clbkFullscreenMode () const override { return false; }
	void clbkGetViewportSize (DWORD *width, DWORD *height) const override;
	bool clbkGetRenderParam (DWORD prm, DWORD *value) const override;

	// --- visuals ---
	int clbkVisEvent (OBJHANDLE hObj, VISHANDLE vis, DWORD msg, DWORD_PTR context) override;
	MESHHANDLE clbkGetMesh (VISHANDLE vis, UINT idx) override;
	void clbkNewVessel (OBJHANDLE hVessel) override {}
	void clbkDeleteVessel (OBJHANDLE hVessel) override;

	// --- screenshots ---
	bool clbkSaveSurfaceToImage (SURFHANDLE surf, const char *fname,
		oapi::ImageFileFormat fmt, float quality = 0.7f) override;

	// --- textures and meshes ---
	SURFHANDLE clbkLoadTexture (const char *fname, DWORD flags = 0) override;
	void clbkReleaseTexture (SURFHANDLE hTex) override;
	bool clbkReleaseSurface (SURFHANDLE surf) override;
	void clbkIncrSurfaceRef (SURFHANDLE surf) override;
	bool clbkGetSurfaceSize (SURFHANDLE surf, DWORD *w, DWORD *h) override;
	bool clbkSetMeshTexture (DEVMESHHANDLE hMesh, DWORD texidx, SURFHANDLE tex) override;
	int clbkSetMeshMaterial (DEVMESHHANDLE hMesh, DWORD matidx, const MATERIAL *mat) override;
	int clbkMeshMaterial (DEVMESHHANDLE hMesh, DWORD matidx, MATERIAL *mat) override;

	// --- ImGui ---
	void clbkImGuiInit () override;
	void clbkImGuiShutdown () override;
	void clbkImGuiNewFrame () override;
	void clbkImGuiRenderDrawData () override;
	uint64_t clbkImGuiSurfaceTexture (SURFHANDLE surf) override;

private:
	SDL_Window *window = nullptr;
	vkc::Context ctx;
	TextureManager texmgr;
	std::unique_ptr<Scene> scene;
	bool ready = false;      // device initialised
	bool imguiReady = false;
	bool frameActive = false; // a frame is being recorded
	void CaptureBackbuffer (const std::string &path);
	std::string autoScreenshot;   // ORBITER_VK_SCREENSHOT: file written at frame autoScreenshotFrame
	unsigned autoScreenshotFrame = 60;
	unsigned frameCount = 0;
};

#endif // !__VULKANCLIENT_H

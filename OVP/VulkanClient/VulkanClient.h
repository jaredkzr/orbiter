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
class Renderer2D;
class FontManager;

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

	// --- 2D panels ---
	void clbkRender2DPanel (SURFHANDLE *hSurf, MESHHANDLE hMesh, MATRIX3 *T, bool additive = false) override;
	void clbkRender2DPanel (SURFHANDLE *hSurf, MESHHANDLE hMesh, MATRIX3 *T, float alpha, bool additive = false) override;

	// --- surfaces ---
	SURFHANDLE clbkLoadSurface (const char *fname, DWORD attrib, bool bPath = false) override;
	SURFHANDLE clbkCreateSurfaceEx (DWORD w, DWORD h, DWORD attrib) override;
	SURFHANDLE clbkCreateSurface (DWORD w, DWORD h, SURFHANDLE hTemplate = NULL) override;
	SURFHANDLE clbkCreateTexture (DWORD w, DWORD h) override;
	bool clbkSetSurfaceColourKey (SURFHANDLE surf, DWORD ckey) override;
	bool clbkBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, SURFHANDLE src, DWORD flag = 0) const override;
	bool clbkBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, SURFHANDLE src, DWORD srcx, DWORD srcy, DWORD w, DWORD h, DWORD flag = 0) const override;
	bool clbkScaleBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, DWORD tgtw, DWORD tgth,
		SURFHANDLE src, DWORD srcx, DWORD srcy, DWORD srcw, DWORD srch, DWORD flag = 0) const override;
	bool clbkFillSurface (SURFHANDLE surf, DWORD col) const override;
	bool clbkFillSurface (SURFHANDLE surf, DWORD tgtx, DWORD tgty, DWORD w, DWORD h, DWORD col) const override;

	// --- sketchpad and drawing tools ---
	oapi::Sketchpad *clbkGetSketchpad (SURFHANDLE surf) override;
	void clbkReleaseSketchpad (oapi::Sketchpad *sp) override;
	oapi::Font *clbkCreateFont (int height, bool prop, const char *face, FontStyle style = FontStyle::FONT_NORMAL, int orientation = 0) const override;
	oapi::Font *clbkCreateFontEx (int height, char *face, int width = 0, int weight = 400, FontStyle style = FontStyle::FONT_NORMAL, float spacing = 0.0f) const override;
	void clbkReleaseFont (oapi::Font *font) const override;
	oapi::Pen *clbkCreatePen (int style, int width, DWORD col) const override;
	void clbkReleasePen (oapi::Pen *pen) const override;
	oapi::Brush *clbkCreateBrush (DWORD col) const override;
	void clbkReleaseBrush (oapi::Brush *brush) const override;

	// --- textures and meshes ---
	SURFHANDLE clbkLoadTexture (const char *fname, DWORD flags = 0) override;
	void clbkReleaseTexture (SURFHANDLE hTex) override;
	bool clbkReleaseSurface (SURFHANDLE surf) override;
	void clbkIncrSurfaceRef (SURFHANDLE surf) override;
	bool clbkGetSurfaceSize (SURFHANDLE surf, DWORD *w, DWORD *h) override;
	bool clbkSetMeshTexture (DEVMESHHANDLE hMesh, DWORD texidx, SURFHANDLE tex) override;
	int clbkSetMeshMaterial (DEVMESHHANDLE hMesh, DWORD matidx, const MATERIAL *mat) override;
	int clbkMeshMaterial (DEVMESHHANDLE hMesh, DWORD matidx, MATERIAL *mat) override;
	int clbkGetMeshGroup (DEVMESHHANDLE hMesh, DWORD grpidx, GROUPREQUESTSPEC *grs) override;
	int clbkEditMeshGroup (DEVMESHHANDLE hMesh, DWORD grpidx, GROUPEDITSPEC *ges) override;

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
	std::unique_ptr<Renderer2D> r2d;
	std::unique_ptr<FontManager> fonts;
	bool ready = false;      // device initialised
	bool imguiReady = false;
	bool frameActive = false; // a frame is being recorded
	void CaptureBackbuffer (const std::string &path);
	std::string autoScreenshot;   // ORBITER_VK_SCREENSHOT: file written at frame autoScreenshotFrame
	unsigned autoScreenshotFrame = 60;
	unsigned frameCount = 0;
};

#endif // !__VULKANCLIENT_H

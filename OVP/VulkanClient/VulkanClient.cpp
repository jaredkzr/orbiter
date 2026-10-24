// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#define ORBITER_MODULE
#include "VulkanClient.h"
#include "Scene.h"
#include "VObjects.h"
#include "VkMesh.h"
#include "Input.h"
#include "posix/OrbiterInput.h"
#include "ImageIO.h"
#include "Draw2D.h"
#include "Sketchpad.h"
#include "Fonts.h"
#include "Particles.h"
#include "PostFX.h"
#include <cmath>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdlib>

static VulkanClient *g_client = nullptr;

// ======================================================================
// Module entry points

DLLCLBK void InitModule (HINSTANCE hDLL)
{
	g_client = new VulkanClient (hDLL);
	if (!oapiRegisterGraphicsClient (g_client)) {
		delete g_client;
		g_client = nullptr;
	}
}

DLLCLBK void ExitModule (HINSTANCE hDLL)
{
	if (g_client) {
		oapiUnregisterGraphicsClient (g_client);
		delete g_client;
		g_client = nullptr;
	}
}

// ======================================================================

VulkanClient::VulkanClient (HINSTANCE hInstance)
	: GraphicsClient (hInstance)
{
}

VulkanClient::~VulkanClient ()
{
	Teardown ();
	if (window) SDL_DestroyWindow (window);
	SDL_Quit();
}

void VulkanClient::Teardown ()
{
	// Must run before process exit: once the driver library has been
	// finalised, Vulkan objects can no longer be destroyed.
	scene.reset();
	if (!ready) return;
	DeleteStreams (true);
	ctx.Drain();       // deferred releases still refer to texture manager resources
	r2d->Shutdown();
	r2d.reset();
	postfx->Shutdown();
	postfx.reset();
	fonts.reset();
	ctx.Drain();
	texmgr.Shutdown();
	ctx.Shutdown();
	ready = false;
}

bool VulkanClient::clbkInitialise ()
{
	if (!GraphicsClient::clbkInitialise ()) return false;

	// Offscreen mode (ORBITER_VK_OFFSCREEN=1): nothing is shown and no display
	// is required; frames are rendered into images (e.g. for screenshots)
	const char *off = getenv ("ORBITER_VK_OFFSCREEN");
	offscreen = off && *off && *off != '0';
	if (offscreen) SDL_SetHint (SDL_HINT_VIDEO_DRIVER, "offscreen,dummy");

	if (!SDL_Init (SDL_INIT_VIDEO)) {
		oapiWriteLogError ("VulkanClient: SDL initialisation failed: %s", SDL_GetError());
		return false;
	}
	const VIDEODATA *vd = GetVideoData();
	int w = vd->winw > 0 ? vd->winw : 1280;
	int h = vd->winh > 0 ? vd->winh : 800;
	// The window and device exist for the lifetime of the client, so that
	// modules can load textures before the simulation session starts.
	window = SDL_CreateWindow ("Orbiter", w, h, offscreen ? SDL_WINDOW_HIDDEN : (SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN));
	if (!window) {
		oapiWriteLogError ("VulkanClient: could not create window: %s", SDL_GetError());
		return false;
	}
	const char *val = getenv ("ORBITER_VK_VALIDATION");
	ctx.SetOffscreenExtent ((uint32_t)w, (uint32_t)h);
	if (!ctx.Init (window, val && *val && *val != '0', offscreen)) return false;
	if (offscreen) oapiWriteLogV ("VulkanClient: rendering offscreen (%dx%d)", w, h);
	if (const char *shot = getenv ("ORBITER_VK_SCREENSHOT")) {
		autoScreenshot = shot;
		if (const char *n = getenv ("ORBITER_VK_SCREENSHOT_FRAME")) autoScreenshotFrame = (unsigned)atoi (n);
	}
	if (!texmgr.Init (&ctx)) return false;
	r2d = std::make_unique<Renderer2D>();
	if (!r2d->Init (&ctx, &texmgr)) return false;
	fonts = std::make_unique<FontManager> (&texmgr);
	materials.Load ("Config/GC");
	oapiWriteLogV ("VulkanClient: material configuration for %zu meshes", materials.MeshCount());
	postfx = std::make_unique<PostFX>();
	if (!postfx->Init (&ctx)) return false;
	ready = true;
	return true;
}

HWND VulkanClient::clbkCreateRenderWindow ()
{
	if (!ready) return NULL;
	SDL_SetWindowTitle (window, "Orbiter (Vulkan)");
	if (!offscreen) {
		SDL_ShowWindow (window);
		SDL_RaiseWindow (window);
	}
	return (HWND)window;
}

void VulkanClient::clbkPostCreation ()
{
	// the planetary system exists now
	scene = std::make_unique<Scene> (this);
	scene->Init ();
}

void VulkanClient::clbkCloseSession (bool fastclose)
{
	GraphicsClient::clbkCloseSession (fastclose);
	DeleteStreams (true);
	if (ready) {
		r2d->Flush ();
		ctx.SubmitPending ();
		vkDeviceWaitIdle (ctx.device);
	}
	scene.reset();
}

void VulkanClient::clbkDestroyRenderWindow (bool fastclose)
{
	GraphicsClient::clbkDestroyRenderWindow (fastclose);
	if (window) SDL_HideWindow (window);
	if (fastclose) Teardown (); // the application exits right after this
}

// ======================================================================
// Per-frame

void VulkanClient::clbkUpdate (bool running)
{
	if (window) input::ProcessEvents (window);
	if (running) {
		double dt = oapiGetSimStep ();
		for (auto *ps : streams) ps->Update (dt);
		DeleteStreams (false);
	}
	if (scene) scene->Update ();
}

void VulkanClient::clbkRenderScene ()
{
	frameActive = false;
	if (!ready || !scene) return;
	if (!ctx.BeginFrame ()) return;
	frameActive = true;
	r2d->Flush ();
	scene->RenderShadows ();
	scene->BeginFrame ();
	scene->RenderPrepass ();
	ctx.BindScene (true);
	scene->Render ();
	VkExtent2D sceneSize;
	VkImageView sceneImage = scene->ResolveTemporal (sceneSize);
	postfx->Apply (sceneImage, sceneSize);   // HDR scene -> backbuffer
	Render2DOverlay (); // panels, MFDs and HUD
}

void VulkanClient::clbkImGuiRenderDrawData ()
{
	if (!imguiReady) return;
	ImGui::Render ();
	if (frameActive) {
		r2d->Flush ();
		ctx.BindBackbuffer (false);
		ImGui_ImplVulkan_RenderDrawData (ImGui::GetDrawData(), ctx.Cmd());
	}
}

bool VulkanClient::clbkDisplayFrame ()
{
	if (!ready) return false;
	r2d->Flush ();
	if (frameActive) {
		if (!autoScreenshot.empty() && ++frameCount == autoScreenshotFrame) {
			CaptureBackbuffer (autoScreenshot);
			oapiWriteLogV ("VulkanClient: screenshot saved to %s", autoScreenshot.c_str());
			float le, ll;
			postfx->ExposureState (le, ll);
			oapiWriteLogV ("VulkanClient: exposure %.3f (metered luminance %.4f)", std::exp2 (le), std::exp2 (ll));
			// ORBITER_VK_SCREENSHOT_EXIT=1: end the session once the screenshot is taken
			const char *ex = getenv ("ORBITER_VK_SCREENSHOT_EXIT");
			if (ex && *ex && *ex != '0') oapiInputCloseRequest ();
		}
		ctx.EndFrame ();
	} else {
		ctx.SubmitPending (); // offscreen work recorded without a frame
	}
	frameActive = false;
	return true;
}

// ======================================================================
// Screenshots

void VulkanClient::CaptureBackbuffer (const std::string &path)
{
	ctx.RequestCapture ([path](const uint8_t *px, uint32_t w, uint32_t h, VkFormat fmt) {
		bool bgr = (fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_UNORM);
		std::vector<uint8_t> rgb ((size_t)w * h * 3);
		for (size_t i = 0; i < (size_t)w * h; i++) {
			rgb[i*3+0] = px[i*4 + (bgr ? 2 : 0)];
			rgb[i*3+1] = px[i*4 + 1];
			rgb[i*3+2] = px[i*4 + (bgr ? 0 : 2)];
		}
		if (!image::WritePNG (path, w, h, rgb.data()))
			oapiWriteLogError ("VulkanClient: could not write %s", path.c_str());
	});
}

bool VulkanClient::clbkSaveSurfaceToImage (SURFHANDLE surf, const char *fname,
	oapi::ImageFileFormat fmt, float quality)
{
	// only the render surface, saved as PNG regardless of the requested format
	if (surf || !fname || !ready) return false;
	CaptureBackbuffer (std::string (fname) + ".png");
	return true;
}

// ======================================================================
// Render parameters

void VulkanClient::clbkGetViewportSize (DWORD *width, DWORD *height) const
{
	int w = 0, h = 0;
	if (offscreen) {
		VkExtent2D e = ctx.Extent();
		w = (int)e.width, h = (int)e.height;
	} else if (window) SDL_GetWindowSizeInPixels (window, &w, &h);
	*width = (DWORD)w, *height = (DWORD)h;
}

bool VulkanClient::clbkGetRenderParam (DWORD prm, DWORD *value) const
{
	switch (prm) {
	case RP_COLOURDEPTH:    *value = 32; return true;
	case RP_ZBUFFERDEPTH:   *value = 32; return true;
	case RP_STENCILDEPTH:   *value = 0;  return true;
	case RP_MAXLIGHTS:      *value = 8;  return true;
	case RP_ISTLDEVICE:     *value = 1;  return true;
	case RP_REQUIRETEXPOW2: *value = 0;  return true;
	}
	return false;
}

// ======================================================================
// Visuals

int VulkanClient::clbkVisEvent (OBJHANDLE hObj, VISHANDLE vis, DWORD msg, DWORD_PTR context)
{
	return vis ? ((VObject*)vis)->VisEvent (msg, context) : 0;
}

MESHHANDLE VulkanClient::clbkGetMesh (VISHANDLE vis, UINT idx)
{
	return vis ? ((VObject*)vis)->GetMesh (idx) : NULL;
}

void VulkanClient::clbkDeleteVessel (OBJHANDLE hVessel)
{
	if (scene) scene->DeleteVisual (hVessel);
}

// ======================================================================
// Textures and meshes

SURFHANDLE VulkanClient::clbkLoadTexture (const char *fname, DWORD flags)
{
	if (!ready) return NULL;
	return (SURFHANDLE)texmgr.Load (fname);
}

void VulkanClient::clbkReleaseTexture (SURFHANDLE hTex)
{
	texmgr.Release ((VkTexture*)hTex);
}

bool VulkanClient::clbkReleaseSurface (SURFHANDLE surf)
{
	if (!surf) return false;
	texmgr.Release ((VkTexture*)surf);
	return true;
}

void VulkanClient::clbkIncrSurfaceRef (SURFHANDLE surf)
{
	texmgr.AddRef ((VkTexture*)surf);
}

bool VulkanClient::clbkGetSurfaceSize (SURFHANDLE surf, DWORD *w, DWORD *h)
{
	if (!surf) return false;
	*w = ((VkTexture*)surf)->width;
	*h = ((VkTexture*)surf)->height;
	return true;
}

bool VulkanClient::clbkSetMeshTexture (DEVMESHHANDLE hMesh, DWORD texidx, SURFHANDLE tex)
{
	return hMesh ? ((VkMesh*)hMesh)->SetTexture (texidx, (VkTexture*)tex) : false;
}

int VulkanClient::clbkSetMeshMaterial (DEVMESHHANDLE hMesh, DWORD matidx, const MATERIAL *mat)
{
	return hMesh ? ((VkMesh*)hMesh)->SetMaterial (matidx, mat) : 2;
}

int VulkanClient::clbkMeshMaterial (DEVMESHHANDLE hMesh, DWORD matidx, MATERIAL *mat)
{
	return hMesh ? ((VkMesh*)hMesh)->GetMaterial (matidx, mat) : 2;
}

int VulkanClient::clbkGetMeshGroup (DEVMESHHANDLE hMesh, DWORD grpidx, GROUPREQUESTSPEC *grs)
{
	return hMesh ? ((VkMesh*)hMesh)->GetGroupData (grpidx, grs) : -2;
}

int VulkanClient::clbkEditMeshGroup (DEVMESHHANDLE hMesh, DWORD grpidx, GROUPEDITSPEC *ges)
{
	return hMesh ? ((VkMesh*)hMesh)->EditGroup (grpidx, ges) : -2;
}

// ======================================================================
// Meshes

void VulkanClient::clbkStoreMeshPersistent (MESHHANDLE hMesh, const char *fname)
{
	if (hMesh && fname) meshNames[hMesh] = fname;
}

std::string VulkanClient::MeshName (MESHHANDLE hMesh) const
{
	auto it = meshNames.find (hMesh);
	return it != meshNames.end() ? it->second : std::string();
}

// ======================================================================
// Particle streams

oapi::ParticleStream *VulkanClient::clbkCreateParticleStream (PARTICLESTREAMSPEC *pss)
{
	if (!ready) return NULL;
	auto *ps = new VkParticleStream (this, &texmgr, pss, VkParticleStream::Kind::Generic);
	streams.push_back (ps);
	return ps;
}

oapi::ParticleStream *VulkanClient::clbkCreateExhaustStream (PARTICLESTREAMSPEC *pss,
	OBJHANDLE hVessel, const double *lvl, const VECTOR3 *ref, const VECTOR3 *dir)
{
	if (!ready) return NULL;
	auto *ps = new VkParticleStream (this, &texmgr, pss, VkParticleStream::Kind::Exhaust);
	ps->Attach (hVessel, ref, dir, lvl);
	streams.push_back (ps);
	return ps;
}

oapi::ParticleStream *VulkanClient::clbkCreateExhaustStream (PARTICLESTREAMSPEC *pss,
	OBJHANDLE hVessel, const double *lvl, const VECTOR3 &ref, const VECTOR3 &dir)
{
	if (!ready) return NULL;
	auto *ps = new VkParticleStream (this, &texmgr, pss, VkParticleStream::Kind::Exhaust);
	ps->Attach (hVessel, ref, dir, lvl);
	streams.push_back (ps);
	return ps;
}

oapi::ParticleStream *VulkanClient::clbkCreateReentryStream (PARTICLESTREAMSPEC *pss, OBJHANDLE hVessel)
{
	if (!ready) return NULL;
	auto *ps = new VkParticleStream (this, &texmgr, pss, VkParticleStream::Kind::Reentry);
	ps->Attach (hVessel, _V(0, 0, 0), _V(0, 0, 0), nullptr);
	streams.push_back (ps);
	return ps;
}

void VulkanClient::DeleteStreams (bool all)
{
	// Deleting a stream that is still attached notifies its vessel
	// (ParticleStream destructor), so this is safe while vessels exist.
	size_t n = 0;
	for (auto *ps : streams) {
		if (all || ps->Expired()) delete ps;
		else streams[n++] = ps;
	}
	streams.resize (n);
}

// ======================================================================
// 2D panels

void VulkanClient::clbkRender2DPanel (SURFHANDLE *hSurf, MESHHANDLE hMesh, MATRIX3 *T, bool additive)
{
	clbkRender2DPanel (hSurf, hMesh, T, 1.0f, additive);
}

void VulkanClient::clbkRender2DPanel (SURFHANDLE *hSurf, MESHHANDLE hMesh, MATRIX3 *T, float alpha, bool additive)
{
	if (!r2d || !hMesh || !r2d->Begin (nullptr)) return;
	// Vertices are in panel pixels, mapped by scale and offset (the core adds
	// a half-pixel offset for D3D pixel centres, which is removed here)
	float sx = (float)T->m11, sy = (float)T->m22;
	float tx = (float)T->m13 + 0.5f, ty = (float)T->m23 + 0.5f;
	DWORD ngrp = oapiMeshGroupCount (hMesh);
	for (DWORD g = 0; g < ngrp; g++) {
		MESHGROUP *grp = oapiMeshGroup (hMesh, g);
		if (!grp || (grp->UsrFlag & 2) || !grp->nIdx || !grp->Vtx) continue;
		VkTexture *tex = nullptr;
		if (grp->TexIdx >= TEXIDX_MFD0) {
			DWORD mfd = grp->TexIdx - TEXIDX_MFD0;
			if (mfd < MAXMFD && !(tex = (VkTexture*)GetMFDSurface ((int)mfd))) continue;
		} else if (hSurf) {
			tex = (VkTexture*)hSurf[grp->TexIdx];
		} else {
			tex = (VkTexture*)oapiGetTextureHandle (hMesh, grp->TexIdx + 1); // the mesh's own textures
		}
		DrawState2D st;
		st.tex = tex;
		st.blend = additive ? Blend2D::Additive : Blend2D::Alpha;
		Vtx2D *v = r2d->AddTris (st, grp->nIdx);
		if (!v) return;
		for (DWORD i = 0; i < grp->nIdx; i++) {
			const NTVERTEX &p = grp->Vtx[grp->Idx[i] < grp->nVtx ? grp->Idx[i] : 0];
			v[i] = { sx * p.x + tx, sy * p.y + ty, p.tu, p.tv, 1.0f, 1.0f, 1.0f, alpha };
		}
	}
}

// ======================================================================
// Surfaces

SURFHANDLE VulkanClient::clbkLoadSurface (const char *fname, DWORD attrib, bool bPath)
{
	if (!ready) return NULL;
	// surfaces that are drawn to get a private copy
	bool writable = (attrib & (OAPISURFACE_RENDERTARGET | OAPISURFACE_GDI | OAPISURFACE_SKETCHPAD | OAPISURFACE_UNCOMPRESS)) != 0;
	return (SURFHANDLE)texmgr.Load (fname, !writable, bPath);
}

SURFHANDLE VulkanClient::clbkCreateSurfaceEx (DWORD w, DWORD h, DWORD attrib)
{
	return ready ? (SURFHANDLE)texmgr.CreateSurface (w, h) : NULL;
}

SURFHANDLE VulkanClient::clbkCreateSurface (DWORD w, DWORD h, SURFHANDLE hTemplate)
{
	return ready ? (SURFHANDLE)texmgr.CreateSurface (w, h) : NULL;
}

SURFHANDLE VulkanClient::clbkCreateTexture (DWORD w, DWORD h)
{
	return ready ? (SURFHANDLE)texmgr.CreateSurface (w, h) : NULL;
}

bool VulkanClient::clbkSetSurfaceColourKey (SURFHANDLE surf, DWORD ckey)
{
	if (!surf) return false;
	((VkTexture*)surf)->ckey = (ckey == SURF_NO_CK ? VkTexture::NO_CKEY : (ckey & 0xFFFFFF));
	return true;
}

bool VulkanClient::clbkBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, SURFHANDLE src, DWORD flag) const
{
	VkTexture *s = (VkTexture*)src;
	if (!r2d || !s) return false;
	return r2d->Blit ((VkTexture*)tgt, tgtx, tgty, s->width, s->height, s, 0, 0, s->width, s->height, flag & BLT_SRCCOLORKEY);
}

bool VulkanClient::clbkBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, SURFHANDLE src, DWORD srcx, DWORD srcy, DWORD w, DWORD h, DWORD flag) const
{
	if (!r2d || !src) return false;
	return r2d->Blit ((VkTexture*)tgt, tgtx, tgty, w, h, (VkTexture*)src, srcx, srcy, w, h, flag & BLT_SRCCOLORKEY);
}

bool VulkanClient::clbkScaleBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, DWORD tgtw, DWORD tgth,
	SURFHANDLE src, DWORD srcx, DWORD srcy, DWORD srcw, DWORD srch, DWORD flag) const
{
	if (!r2d || !src) return false;
	return r2d->Blit ((VkTexture*)tgt, tgtx, tgty, tgtw, tgth, (VkTexture*)src, srcx, srcy, srcw, srch, flag & BLT_SRCCOLORKEY);
}

bool VulkanClient::clbkFillSurface (SURFHANDLE surf, DWORD col) const
{
	if (!r2d) return false;
	VkTexture *t = (VkTexture*)surf;
	VkExtent2D ext = t ? VkExtent2D{ t->width, t->height } : ctx.Extent();
	return r2d->Fill (t, 0, 0, ext.width, ext.height, col);
}

bool VulkanClient::clbkFillSurface (SURFHANDLE surf, DWORD tgtx, DWORD tgty, DWORD w, DWORD h, DWORD col) const
{
	return r2d ? r2d->Fill ((VkTexture*)surf, tgtx, tgty, w, h, col) : false;
}

// ======================================================================
// Sketchpad and drawing tools

oapi::Sketchpad *VulkanClient::clbkGetSketchpad (SURFHANDLE surf)
{
	if (!r2d) return NULL;
	VkSketchpad *skp = new VkSketchpad (r2d.get(), fonts.get(), (VkTexture*)surf);
	if (!skp->Valid()) {
		delete skp;
		return NULL;
	}
	return skp;
}

void VulkanClient::clbkReleaseSketchpad (oapi::Sketchpad *sp)
{
	delete (VkSketchpad*)sp; // flushes the drawing batch
}

oapi::Font *VulkanClient::clbkCreateFont (int height, bool prop, const char *face, FontStyle style, int orientation) const
{
	return fonts ? fonts->Create (height, prop, face, 0, 400, style, orientation, 0.0f) : NULL;
}

oapi::Font *VulkanClient::clbkCreateFontEx (int height, char *face, int width, int weight, FontStyle style, float spacing) const
{
	return fonts ? fonts->Create (height, true, face, width, weight, style, 0, spacing) : NULL;
}

void VulkanClient::clbkReleaseFont (oapi::Font *font) const
{
	delete font;
}

oapi::Pen *VulkanClient::clbkCreatePen (int style, int width, DWORD col) const
{
	return new VkPen (style, width, col);
}

void VulkanClient::clbkReleasePen (oapi::Pen *pen) const
{
	delete pen;
}

oapi::Brush *VulkanClient::clbkCreateBrush (DWORD col) const
{
	return new VkBrush (col);
}

void VulkanClient::clbkReleaseBrush (oapi::Brush *brush) const
{
	delete brush;
}

// ======================================================================
// ImGui

static void ImGuiCheckResult (VkResult res)
{
	if (res != VK_SUCCESS) vkc::Check (res, "ImGui Vulkan backend");
}

void VulkanClient::clbkImGuiInit ()
{
	if (!ready) return;
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable; // dialogs stay inside the render window

	ImGui_ImplSDL3_InitForVulkan (window);

	ImGui_ImplVulkan_InitInfo ii{};
	ii.ApiVersion = VK_API_VERSION_1_3;
	ii.Instance = ctx.instance;
	ii.PhysicalDevice = ctx.physicalDevice;
	ii.Device = ctx.device;
	ii.QueueFamily = ctx.queueFamily;
	ii.Queue = ctx.queue;
	ii.DescriptorPoolSize = 64;
	ii.MinImageCount = 2;
	ii.ImageCount = ctx.swapchainImageCount;
	ii.UseDynamicRendering = true;
	ii.PipelineInfoMain.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &ctx.colorFormat;
	ii.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED; // drawn after the scene, without depth
	ii.CheckVkResultFn = ImGuiCheckResult;
	ImGui_ImplVulkan_Init (&ii);
	imguiReady = true;
}

void VulkanClient::clbkImGuiShutdown ()
{
	if (!imguiReady) return;
	vkDeviceWaitIdle (ctx.device);
	ImGui_ImplVulkan_Shutdown ();
	ImGui_ImplSDL3_Shutdown ();
	imguiReady = false;
}

void VulkanClient::clbkImGuiNewFrame ()
{
	if (!imguiReady) return;
	ImGui_ImplVulkan_NewFrame ();
	ImGui_ImplSDL3_NewFrame ();
}

uint64_t VulkanClient::clbkImGuiSurfaceTexture (SURFHANDLE surf)
{
	// texture descriptor sets use a layout identical to the ImGui backend's
	return surf ? (uint64_t)((VkTexture*)surf)->set : 0;
}

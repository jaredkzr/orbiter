// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#define ORBITER_MODULE
#include "VulkanClient.h"
#include "Scene.h"
#include "VObjects.h"
#include "VkMesh.h"
#include "Input.h"
#include "ImageIO.h"
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
	scene.reset();
	if (ready) {
		texmgr.Shutdown();
		ctx.Shutdown();
	}
	if (window) SDL_DestroyWindow (window);
	SDL_Quit();
}

bool VulkanClient::clbkInitialise ()
{
	if (!GraphicsClient::clbkInitialise ()) return false;

	if (!SDL_Init (SDL_INIT_VIDEO)) {
		oapiWriteLogError ("VulkanClient: SDL initialisation failed: %s", SDL_GetError());
		return false;
	}
	const VIDEODATA *vd = GetVideoData();
	int w = vd->winw > 0 ? vd->winw : 1280;
	int h = vd->winh > 0 ? vd->winh : 800;
	// The window and device exist for the lifetime of the client, so that
	// modules can load textures before the simulation session starts.
	window = SDL_CreateWindow ("Orbiter", w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
	if (!window) {
		oapiWriteLogError ("VulkanClient: could not create window: %s", SDL_GetError());
		return false;
	}
	const char *val = getenv ("ORBITER_VK_VALIDATION");
	if (!ctx.Init (window, val && *val && *val != '0')) return false;
	if (const char *shot = getenv ("ORBITER_VK_SCREENSHOT")) {
		autoScreenshot = shot;
		if (const char *n = getenv ("ORBITER_VK_SCREENSHOT_FRAME")) autoScreenshotFrame = (unsigned)atoi (n);
	}
	if (!texmgr.Init (&ctx)) return false;
	ready = true;
	return true;
}

HWND VulkanClient::clbkCreateRenderWindow ()
{
	if (!ready) return NULL;
	SDL_SetWindowTitle (window, "Orbiter (Vulkan)");
	SDL_ShowWindow (window);
	SDL_RaiseWindow (window);
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
	if (ready) vkDeviceWaitIdle (ctx.device);
	scene.reset();
}

void VulkanClient::clbkDestroyRenderWindow (bool fastclose)
{
	GraphicsClient::clbkDestroyRenderWindow (fastclose);
	if (window) SDL_HideWindow (window);
}

// ======================================================================
// Per-frame

void VulkanClient::clbkUpdate (bool running)
{
	if (window) input::ProcessEvents (window);
	if (scene) scene->Update ();
}

void VulkanClient::clbkRenderScene ()
{
	frameActive = false;
	if (!ready || !scene) return;
	if (!ctx.BeginFrame ()) return;
	frameActive = true;
	ctx.BeginRendering ();
	scene->Render ();
}

void VulkanClient::clbkImGuiRenderDrawData ()
{
	if (!imguiReady) return;
	ImGui::Render ();
	if (frameActive)
		ImGui_ImplVulkan_RenderDrawData (ImGui::GetDrawData(), ctx.Cmd());
}

bool VulkanClient::clbkDisplayFrame ()
{
	if (frameActive) {
		if (!autoScreenshot.empty() && ++frameCount == autoScreenshotFrame) {
			CaptureBackbuffer (autoScreenshot);
			oapiWriteLogV ("VulkanClient: screenshot saved to %s", autoScreenshot.c_str());
		}
		ctx.EndFrame ();
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
	if (window) SDL_GetWindowSizeInPixels (window, &w, &h);
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
	ii.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = ctx.depthFormat;
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

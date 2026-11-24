// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#define ORBITER_MODULE
#include "VulkanClient.h"
#include "Fsr.h"
#include "GfxSettings.h"
#include "Fsr4.h"
#include "SkySettings.h"
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
#include "ParticleStream.h"
#include "PostFX.h"
#include "Plumes.h"
#include "Fireball.h"
#include <cmath>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdlib>
#include <future>
#include <atomic>
#include <cstring>
#include <thread>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <unistd.h>

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
	for (auto &w : writers) w.wait ();
	writers.clear ();
	CloseVideo ();
	if (frameTimes) { fclose (frameTimes); frameTimes = nullptr; }
	// Must run before process exit: once the driver library has been
	// finalised, Vulkan objects can no longer be destroyed.
	if (ready) DeleteStreams (true); // (their particles live in the scene's pool)
	scene.reset();
	if (!ready) return;
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
	vkc::Gfx ().Load (); // (the graphics settings)
	// the sky's data are required (ORBITER_VK_SKY=0: no sky, for tests)
	const char *noSky = getenv ("ORBITER_VK_SKY");
	if (!noSky || atoi (noSky)) {
		std::vector<std::string> missing = SkySettings::MissingData ();
		for (auto &m : missing)
			oapiWriteLogError ("VulkanClient: missing sky data: %s (built by Utils/SkyKit)", m.c_str ());
		if (missing.size ()) return false;
	}
	oapiSurfaceDetailSupported (); // (PlanetSurface: meshes and shading)
	oapiScatterSupported ();       // (ScatterField: the ground scatter)
	oapiCitySupported ();          // (CityField: the cities)

	// Offscreen mode (ORBITER_VK_OFFSCREEN=1): nothing is shown and no display
	// is required; frames are rendered into images (e.g. for screenshots)
	const char *off = getenv ("ORBITER_VK_OFFSCREEN");
	offscreen = off && *off && *off != '0';
	if (offscreen) SDL_SetHint (SDL_HINT_VIDEO_DRIVER, "offscreen,dummy");

	// Game controllers only with a visible window (offscreen runs are tests),
	// or with a virtual test gamepad (ORBITER_VK_TEST_GAMEPAD, see input::AttachTestGamepad)
	const char *testPad = getenv ("ORBITER_VK_TEST_GAMEPAD");
	if (testPad) // (the hidden window never has the input focus)
		SDL_SetHint (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if (!SDL_Init ((offscreen && !testPad) ? SDL_INIT_VIDEO : SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		oapiWriteLogError ("VulkanClient: SDL initialisation failed: %s", SDL_GetError());
		return false;
	}
	if (testPad) input::AttachTestGamepad (testPad);
	if (const char *rc = getenv ("ORBITER_VK_REMOTE")) {
		// remote control (playtesting): the test input's commands from a socket
		std::string p (rc);
		size_t sl = p.find_last_of ('/');
		std::string dir = (sl == std::string::npos ? std::string (".") : p.substr (0, sl));
		const char *pace = getenv ("ORBITER_VK_REMOTE_PACE");
		bool lockstep = !(pace && !strcmp (pace, "realtime"));
		testInput = std::make_unique<TestInput> ();
		remote = std::make_unique<RemoteControl> ();
		RemoteControl *r = remote.get();
		testInput->SetInteractive ([r](int id, bool ok, const std::string &t) { r->Answer (id, ok, t); }, dir);
		if (!remote->Start (p, lockstep, testInput.get())) remote.reset(), testInput.reset();
	} else if (const char *ti = getenv ("ORBITER_VK_TEST_INPUT")) {
		testInput = std::make_unique<TestInput> ();
		if (!testInput->Load (ti)) testInput.reset();
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
	// ORBITER_VK_FRAMETIMES=<file>: a line per frame (Tests/Bench/analyze.py
	// reads it; format in clbkDisplayFrame)
	if (const char *ft = getenv ("ORBITER_VK_FRAMETIMES")) {
		frameTimes = fopen (ft, "w");
		if (!frameTimes) oapiWriteLogError ("VulkanClient: could not write %s", ft);
		ctx.cpuTiming = frameTimes != nullptr;
	}
	if (const char *h = getenv ("ORBITER_VK_HIDE_UI")) hideUi = *h && *h != '0';
	// ORBITER_VK_VIDEO_FILE=<file.mp4>: every frame streamed to an ffmpeg
	// process that encodes it (no frames on disk; offscreen too, so with the
	// screen locked); ORBITER_VK_VIDEO_FPS its frame rate (30: with
	// --fixedstep=1/30, one frame a step - the sound rendered alongside fits)
	if (const char *v = getenv ("ORBITER_VK_VIDEO_FILE")) videoFile = v;
	if (const char *r = getenv ("ORBITER_VK_VIDEO_FPS")) videoFps = std::max (1.0, atof (r));
	if (const char *shot = getenv ("ORBITER_VK_SCREENSHOT")) {
		autoScreenshot = shot;
		if (const char *n = getenv ("ORBITER_VK_SCREENSHOT_FRAME")) autoScreenshotFrame = (unsigned)atoi (n);
		if (const char *n = getenv ("ORBITER_VK_SCREENSHOT_NOUI")) shotNoUi = *n && *n != '0';
		// ORBITER_VK_VIDEO=<dir>: also write the ORBITER_VK_VIDEO_FRAMES frames
		// before the screenshot (frame_NNNNN.png; sim times in times.txt)
		if (const char *v = getenv ("ORBITER_VK_VIDEO")) {
			videoDir = v;
			videoFrames = 300;
			if (const char *n = getenv ("ORBITER_VK_VIDEO_FRAMES")) videoFrames = (unsigned)atoi (n);
		}
	}
	if (!texmgr.Init (&ctx)) return false;
	r2d = std::make_unique<Renderer2D>();
	if (!r2d->Init (&ctx, &texmgr)) return false;
	fonts = std::make_unique<FontManager> (&texmgr);
	materials.Load ("Config/GC");
	materials.Load ("Config/VK"); // (Vulkan client additions)
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
	ApplyVideoMode ();
	ShowWindow ();
	return (HWND)window;
}

void VulkanClient::ShowWindow ()
{
	if (offscreen || windowShown) return;
	SDL_ShowWindow (window);
	SDL_RaiseWindow (window);
	windowShown = true;
}

void VulkanClient::ApplyVideoMode ()
{
	// window size and fullscreen (desktop resolution) from the video
	// parameters, which the launchpad may have changed since the last session
	if (offscreen) return;
	const VIDEODATA *vd = GetVideoData();
	SDL_SetWindowFullscreen (window, vd->fullscreen);
	if (!vd->fullscreen && vd->winw > 0 && vd->winh > 0) {
		int w, h;
		SDL_GetWindowSize (window, &w, &h);
		if (w != vd->winw || h != vd->winh) SDL_SetWindowSize (window, vd->winw, vd->winh);
	}
	SDL_SyncWindow (window);
}

void VulkanClient::clbkPostCreation ()
{
	// the planetary system exists now
	scene = std::make_unique<Scene> (this);
	scene->Init ();
}

void VulkanClient::clbkOptionChanged (DWORD cat, DWORD item)
{
	GraphicsClient::clbkOptionChanged (cat, item);
	if (cat == OPTCAT_CELSPHERE && scene) scene->SkyOptionsChanged ();
}

int VulkanClient::clbkGetSettings (const oapi::GraphicsSetting **list)
{
	const auto &l = vkc::Gfx ().List (ctx.rayTracing, FsrUpscaler::Available (), Fsr4Backend::Available ());
	*list = l.data ();
	return (int)l.size ();
}

void VulkanClient::clbkSetSetting (int index, float value)
{
	vkc::GfxSettings &g = vkc::Gfx ();
	g.Set (index, value);
	// (the settings that apply at once)
	if (ready && scene) ctx.SetRenderScale (g.renderScale); // (else when the session starts)
	if (postfx) postfx->bloomStrength = g.bloom, postfx->autoExposure = g.autoExposure;
	if (scene) scene->ApplySettings ();
}

void VulkanClient::clbkSaveSettings ()
{
	vkc::Gfx ().Save ();
}

void VulkanClient::clbkCloseSession (bool fastclose)
{
	CloseVideo (); // (the session's video ends with it)
	GraphicsClient::clbkCloseSession (fastclose);
	DeleteStreams (true);
	if (testInput) testInput->SessionEnd ();
	if (ready) {
		r2d->Flush ();
		ctx.SubmitPending ();
		ctx.WaitIdle ();
	}
	scene.reset();
}

void VulkanClient::clbkDestroyRenderWindow (bool fastclose)
{
	GraphicsClient::clbkDestroyRenderWindow (fastclose);
	// (the window stays open for the launchpad; it is destroyed on exit)
	if (fastclose) Teardown (); // the application exits right after this
}

// ======================================================================
// Per-frame

static double WallNow () // [s]
{
	return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace {
	// The GPU's clocks, power and temperature for the frame-time log (Linux
	// amdgpu: its hwmon files), sampled every 100 ms on a thread of its own (a
	// power reading asks the GPU's firmware)
	class GpuSensors {
	public:
		static GpuSensors &Get () { static GpuSensors g; return g; }
		bool Valid () const { return !dir.empty (); }
		double RssMb () const { return v[5].load (); } // [MB] the process's resident memory (negative: not read)
		// [MHz], [MHz], [W], [deg C], [%] (negative: not read)
		void Read (double &sclk, double &mclk, double &watts, double &tj, double &busy) const
		{
			sclk = v[0].load () * 1e-6, mclk = v[1].load () * 1e-6, watts = v[2].load () * 1e-6, tj = v[3].load () * 1e-3, busy = v[4].load ();
		}
		~GpuSensors ()
		{
			quit = true;
			if (thread.joinable ()) thread.join ();
		}
	private:
		std::string dir, busyFile;
		std::atomic<double> v[6];
		std::atomic<bool> quit{ false };
		std::thread thread;
		GpuSensors ()
		{
			for (auto &x : v) x = -1e30;
#ifndef _WIN32
			// (the first card with an amdgpu hwmon)
			for (int c = 0; c < 8 && dir.empty (); c++)
				for (int h = 0; h < 16; h++) {
					char d[128];
					snprintf (d, sizeof (d), "/sys/class/drm/card%d/device/hwmon/hwmon%d/", c, h);
					if (FILE *f = fopen ((std::string (d) + "name").c_str (), "r")) {
						char name[32] = {};
						if (fgets (name, sizeof (name), f) && !strncmp (name, "amdgpu", 6)) {
							dir = d;
							char b[96];
							snprintf (b, sizeof (b), "/sys/class/drm/card%d/device/gpu_busy_percent", c);
							busyFile = b;
						}
						fclose (f);
						if (!dir.empty ()) break;
					}
				}
			thread = std::thread ([this] { Loop (); });
#endif
		}
		static double ReadNum (const std::string &path)
		{
			double x = -1e30;
			if (FILE *f = fopen (path.c_str (), "r")) {
				if (fscanf (f, "%lf", &x) != 1) x = -1e30;
				fclose (f);
			}
			return x;
		}
		void Loop ()
		{
			const double page = (double)sysconf (_SC_PAGESIZE) / (1 << 20);
			while (!quit) {
				if (!dir.empty ()) {
					v[0] = ReadNum (dir + "freq1_input");
					v[1] = ReadNum (dir + "freq2_input");
					double w = ReadNum (dir + "power1_average");
					v[2] = w > -1e29 ? w : ReadNum (dir + "power1_input");
					v[3] = ReadNum (dir + "temp2_input"); // (junction)
					v[4] = ReadNum (busyFile);
				}
				if (FILE *f = fopen ("/proc/self/statm", "r")) {
					unsigned long long size, rss;
					if (fscanf (f, "%llu %llu", &size, &rss) == 2) v[5] = rss * page;
					fclose (f);
				}
				for (int i = 0; i < 10 && !quit; i++) std::this_thread::sleep_for (std::chrono::milliseconds (10));
			}
		}
	};
}

void VulkanClient::clbkUpdate (bool running)
{
	double t0 = 0.0;
	if (frameTimes) {
		// the core's part of the frame since the last one was displayed: its
		// state update's parts, and the rest (time step, camera)
		t0 = WallNow ();
		double c[3] = { 0.0, 0.0, 0.0 };
		if (running) oapiGetCoreStepTimes (c);
		ctx.CpuAdd ("core pre", c[0]), ctx.CpuAdd ("core step", c[1]), ctx.CpuAdd ("core post", c[2]);
		if (displayEnd > 0.0) ctx.CpuAdd ("core other", std::max (0.0, (t0 - displayEnd) * 1e3 - c[0] - c[1] - c[2]));
	}
	double w0 = ctx.WaitTotal ();
	auto section = [&](const char *name) { // (less any wait on the GPU in it)
		if (!frameTimes) return;
		double t = WallNow (), w = ctx.WaitTotal ();
		ctx.CpuAdd (name, std::max (0.0, (t - t0) * 1e3 - (w - w0)));
		t0 = t, w0 = w;
	};
	if (window) {
		updateCount++;
		if (remote) {
			remote->SetFrame (updateCount, scene != nullptr);
			remote->Poll (window);
		}
		if (testInput && (scene || remote)) testInput->Step (window); // (remote: the launchpad too)
		input::ProcessEvents (window);
	}
	section ("input");
	if (running) {
		double dt = oapiGetSimStep ();
		if (scene) scene->Particles ()->AddTime (dt);
		for (auto *ps : streams) ps->Update (dt);
		DeleteStreams (false);
	}
	section ("streams");
	if (scene) scene->Update ();
	section ("scene update");
	updateEnd = t0;
}

void VulkanClient::clbkTimeJump (double simt, double simdt, double mjd)
{
	GraphicsClient::clbkTimeJump (simt, simdt, mjd);
	if (scene) scene->OnTimeJump ();
}

namespace {
	constexpr double DAYLIGHT_MAX_EXPOSURE = 1.3; // with the camera in sunlight
	constexpr double NIGHT_MIN_EXPOSURE = 0.15;   // lowest exposure with the camera in darkness (flames lighting the view)
	constexpr double NIGHT_METER_LOW = 0.4, NIGHT_METER_HIGH = 0.9; // metering window with the camera in darkness (PostFX meterLow/High)
	constexpr double FLAME_DISPLAY = 3.0; // a flame's drawn core at night, exposed: this many times white (blown out)
	constexpr double STAR_DISC_DISPLAY = 0.9; // a large star disc's centre, displayed
}

void VulkanClient::clbkRenderScene ()
{
	frameActive = false;
	if (!ready) return;
	if (!scene) {
		// no session (launchpad): a cleared frame for the user interface
		ShowWindow ();
		if (!ctx.BeginFrame ()) return;
		frameActive = true;
		r2d->Flush ();
		ctx.BindBackbuffer (false);
		return;
	}
	if (!warmedUp) WarmUp ();
	if (frameTimes) {
		renderStart = WallNow ();
		if (updateEnd > 0.0) ctx.CpuAdd ("core input", (renderStart - updateEnd) * 1e3); // (user input, the dialogs' frame)
		ctx.BeginCpuFrame ();
	}
	if (!ctx.BeginFrame ()) return;
	frameActive = true;
	r2d->Flush ();
	ctx.GpuMark ("start");
	scene->RenderShadows ();
	ctx.GpuMark ("shadows");
	scene->BeginFrame ();
	scene->RenderPrepass ();
	scene->RenderProbe ();
	ctx.GpuMark ("probe");
	ctx.BindScene (true);
	float le = 0.0f;
	if (postfx->autoExposure) {
		float ll;
		postfx->ExposureState (le, ll);
		scene->SkyAdapt (ll, vkc::FrameDt (oapiGetSysStep ()));
	}
	if (PlumeRenderer *pr = scene->Plumes ()) pr->exposure = postfx->exposure * (postfx->autoExposure ? std::exp2 (le) : 1.0f);
	scene->Render ();
	ctx.GpuMark ("effects");
	VkExtent2D sceneSize;
	VkImageView sceneImage = scene->ResolveTemporal (sceneSize);
	ctx.GpuMark ("temporal");
	postfx->sunOnScreen = scene->SunOnScreen (postfx->sunUV);
	{
		double sl = scene->CameraSunlight ();
		postfx->maxExposure = (float)std::exp2 (std::log2 (4.0) + (std::log2 (DAYLIGHT_MAX_EXPOSURE) - std::log2 (4.0)) * sl);
		// deep in a gas giant's clouds the eye adapts further, but not
		// fully: the light fading to darkness with depth stays seen
		double dt = scene->DeckTransmittance (scene->CameraPos ());
		if (dt < 1.0) postfx->maxExposure = std::max (postfx->maxExposure, (float)(4.0 + 4.0 * (1.0 - dt)));
		// (a black hole's accretion disc in the lens: the eye adapts to it)
		bool disc = scene->LensDisc ();
		// (a star's disc large in view, many times brighter than lit
		// surfaces: the eye adapts to it, its photosphere's detail seen)
		// (the exposure then brings the disc's centre to about full
		// brightness: the photosphere's granulation, spots and limb seen;
		// lit surfaces near it, their light compressed, dimmer)
		double teff = 5772.0;
		double sa = scene->StarDiscAngle (&teff);
		double fill = std::clamp ((sa - 0.02) / 0.08, 0.0, 1.0);
		fill = fill * fill * (3.0 - 2.0 * fill);
		// (at night the eye, a camera, adapts further down when a lit scene
		// fills the view: a launch's flames lighting the smoke and the ground
		// at many times daylight. The floor binds only then: a dark night
		// scene meters far below it)
		postfx->minExposure = disc ? 1e-4f : (float)std::exp2 (std::log2 (NIGHT_MIN_EXPOSURE) + (std::log2 (0.6) - std::log2 (NIGHT_MIN_EXPOSURE)) * std::clamp (sl, 0.0, 1.0));
		postfx->adaptStrength = disc ? 0.9f : 0.6f;
		// (at night the brightest lit samples are the flames themselves, drawn
		// far brighter than anything they light: metered on them, a launch's
		// vessel lit from behind by its flames stayed dark. In darkness the
		// meter leaves out as many of the brightest samples as the flames
		// cover (their solid angle, PlumeRenderer) and its window moves down
		// somewhat: exposed for the scene the flames light, as a photograph of
		// a night launch, the flames blown out. Tuned by the night launch
		// shots (N2-night.md): the hull from below ~80/255, the pad cloud at
		// 250 m within ~10 % of the plain meter. In sunlight: the plain meter)
		{
			double night = 1.0 - std::clamp (sl, 0.0, 1.0);
			double lo = NIGHT_METER_LOW, hi = NIGHT_METER_HIGH;
			postfx->meterLow = (float)(0.5 + (lo - 0.5) * night);
			postfx->meterHigh = (float)(0.98 + (hi - 0.98) * night);
			// the flames' share of the image (their solid angle over the view's)
			double fl = 0.0;
			if (PlumeRenderer *pr = scene->Plumes ())
				fl = std::min (pr->SolidAngle () * scene->ProjX () * scene->ProjY () / 4.0, 0.9);
			// (a crash fireball counts partly: the brightest 60 % of it left
			// out, the rest metered - a mild darkening around it at night, not
			// blinding; its hot core blown out)
			if (FireballRenderer *fb = scene->Fireballs ())
				fl = std::min (fl + 0.6 * fb->ScreenShare (), 0.9);
			postfx->meterFlames = (float)(fl * night);
			// (and exposed for them, as a photograph of a night launch: the
			// flame's core blown out at FLAME_DISPLAY times white, the smoke
			// it lights bright beside it and falling off, the sky black - the
			// meter alone, leaving the flames out, exposed for the dark sky
			// and the stars; smoke and pad then all evenly bright)
			if (PlumeRenderer *pr = scene->Plumes ())
				if (pr->FlameRadiance () > 0.0 && fl > 0.0) {
					double w = night * std::clamp ((fl - 2e-4) / 2e-3, 0.0, 1.0);
					double cap = FLAME_DISPLAY / pr->FlameRadiance ();
					if (w > 0.0 && cap < postfx->maxExposure)
						postfx->maxExposure = (float)std::exp2 (std::log2 (postfx->maxExposure) + (std::log2 (std::max (cap, (double)postfx->minExposure)) - std::log2 (postfx->maxExposure)) * w);
				}
		}
		if (fill > 0.0 && !disc) {
			float c[3];
			Scene::StarDiscRGB (teff, c);
			// (its brightest channel: a red dwarf's red unsaturated, its colour kept)
			double lum = Scene::STAR_BRIGHTNESS * std::max ({ (double)c[0], (double)c[1], (double)c[2], 1e-3 });
			double target = std::log2 (STAR_DISC_DISPLAY / lum);
			// (a red dwarf's disc, dimmer than the Sun's, as bright: the eye adapts up)
			auto blend = [&](float e) { return (float)std::exp2 (std::log2 (e) + (target - std::log2 (e)) * fill); };
			postfx->maxExposure = blend (postfx->maxExposure);
			postfx->minExposure = blend (postfx->minExposure);
		}
	}
	// relativistic view: the Doppler shift, before the exposure
	postfx->rel.on = scene->RelView ();
	if (postfx->rel.on) {
		VECTOR3 bv = scene->RelBetaView ();
		for (int k = 0; k < 3; k++) postfx->rel.beta[k] = (float)bv.data[k];
		postfx->rel.fx = (float)scene->ProjX (), postfx->rel.fy = (float)scene->ProjY ();
		scene->RelSelfView (postfx->rel.self);
		postfx->rel.prepass = scene->PrepassView ();
	}
	postfx->Apply (sceneImage, sceneSize);   // HDR scene -> backbuffer
	ctx.GpuMark ("post");
	if (!CleanShotFrame ()) Render2DOverlay (); // panels, MFDs and HUD
	ctx.CpuMark ("overlay");
}

void VulkanClient::WarmUp ()
{
	// (the first frames had shown the terrain made from scratch, 1.5-3 s of
	// 7-15 fps: the meshes, the first city set's 100 ms a frame, the
	// scatter's 50 ms. Instead the loading goes on, with cleared frames,
	// until the tiles, cities and scatter of the first view are in)
	warmedUp = true;
	static const double cap = getenv ("ORBITER_VK_WARMUP_S") ? atof (getenv ("ORBITER_VK_WARMUP_S")) : 3.0;
	if (cap <= 0.0 || !scene) return;
	auto t0 = std::chrono::steady_clock::now ();
	int frames = 0, idle = 0;
	bool busy = true;
	while (std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count () < cap) {
		if (!ctx.BeginFrame ()) break;
		bool last = std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count () > cap - 0.01;
		busy = scene->WarmUpStep (last);
		r2d->Flush ();
		ctx.BindBackbuffer (false);
		ctx.EndFrame ();
		frames++;
		if (busy) idle = 0;
		else if (++idle >= 3) break;
	}
	oapiWriteLogV ("VulkanClient: loading held %.2f s, %d frames, for the first view's tiles, cities and scatter%s",
		std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count (), frames, busy ? " (not all in)" : "");
	vkc::Stats () = vkc::FrameStats (); // (not the first frame's)
	ctx.ClearCpuTimes ();
	ctx.TakeFenceWait ();
}

void VulkanClient::clbkImGuiRenderDrawData ()
{
	if (!imguiReady) return;
	ImGui::Render ();
	if (frameActive && !CleanShotFrame ()) {
		r2d->Flush ();
		ctx.BindBackbuffer (false);
		ImGui_ImplVulkan_RenderDrawData (ImGui::GetDrawData(), ctx.Cmd());
	}
}

bool VulkanClient::clbkDisplayFrame ()
{
	if (!ready) return false;
	r2d->Flush ();
	// (ORBITER_VK_MEMLOG: the heaps and the live buffers by origin, every 20 s)
	static const bool memLog = [] { const char *e = getenv ("ORBITER_VK_MEMLOG"); return e && *e && *e != '0'; } ();
	if (memLog) {
		static auto last = std::chrono::steady_clock::now ();
		auto t = std::chrono::steady_clock::now ();
		if (t - last > std::chrono::seconds (20)) { last = t; vkc::LogHeaps (); }
	}
	{ // (the log's time of the session's first frame: the load time, Tests/Bench ColdStart)
		static bool firstLogged = false;
		if (frameActive && scene && !firstLogged) firstLogged = true, oapiWriteLog ("VulkanClient: first frame");
	}
	if (frameTimes && frameActive && scene) {
		ctx.CpuMark ("2d ui"); // (the core's 2D output, the dialogs)
		double now = WallNow ();
		if (!frameTimesCount) {
			// a header: what the frames were measured on (ORBITER_BENCH_STAMP:
			// the benchmark's own, e.g. the build and the run's settings)
			const auto &g = vkc::Gfx ();
			VkExtent2D e = ctx.Extent (), r = ctx.RenderExtent ();
			const char *stamp = getenv ("ORBITER_BENCH_STAMP");
			fprintf (frameTimes, "# orbiter-frametimes 3 %s size=%ux%u render=%ux%u offscreen=%d frames_in_flight=%u aa=%s rt=%d rt_lighting=%d rt_reflections=%d"
				" clouds=%d plumes=%d particles=%d particle_density=%g smoke=%d fireballs=%d %s\n",
				ctx.DeviceStamp ().c_str (), e.width, e.height, r.width, r.height, offscreen ? 1 : 0, vkc::FRAMES_IN_FLIGHT,
				scene->AntiAliasingName ().c_str (), scene->RT () ? 1 : 0, g.rtLighting ? 1 : 0, g.rtReflections ? 1 : 0,
				g.clouds ? 1 : 0, g.plumes ? 1 : 0, g.particles ? 1 : 0, g.particleDensity, g.smokeRendering, g.fireballs, stamp ? stamp : "");
		}
		// a frame: its number, the sim time [s], the wall-clock time since the
		// previous frame [ms], the GPU's total [ms] and its passes (with
		// ORBITER_VK_PROFILE; a frame or two behind), "CPU render=" the
		// client's time from clbkRenderScene to here and "CPU wait=" its time
		// waiting on the GPU (frames in flight, the swapchain) [ms]; after
		// "@cpu" the CPU's sections [ms] (the core's parts, clbkUpdate's, then
		// per GPU pass the CPU time up to its mark, less any wait; "submit":
		// the previous frame's); "@bound=" G when the CPU waited on the GPU,
		// else C
		// Then (version 3): "@glag=" how many frames before this one the GPU
		// times are from (frames in flight: the GPU's frame N-2 on line N);
		// "@n" the frame's workload counters (vkc::FrameStats, those not 0);
		// "@mem" the device-local heap's use and budget [MB] (the driver's,
		// VK_EXT_memory_budget); "@hw" the GPU's clocks [MHz], power [W],
		// junction temperature [C] and busy share [%] (amdgpu, sampled every 100 ms);
		// "@mem rss=" the process's resident memory [MB]
		const auto &gpu = ctx.LastGpuTimes ();
		double total = 0.0;
		for (const auto &m : gpu) total += m.second;
		double wall = lastFrameWall < 0.0 ? 0.0 : (now - lastFrameWall) * 1e3, wait = ctx.TakeFenceWait ();
		fprintf (frameTimes, "%u %.6f %.3f %.3f", ++frameTimesCount, oapiGetSimTime (), wall, total);
		for (const auto &m : gpu) fprintf (frameTimes, " %s=%.3f", m.first, m.second);
		fprintf (frameTimes, " CPU render=%.3f CPU wait=%.3f @cpu", (now - renderStart) * 1e3, wait);
		for (const auto &m : ctx.CpuTimes ()) fprintf (frameTimes, " %s=%.3f", m.first, m.second);
		fprintf (frameTimes, " @bound=%c", wait > std::max (0.5, 0.05 * wall) ? 'G' : 'C');
		// (this frame is submitted as FrameSerial + 1)
		if (!gpu.empty ()) fprintf (frameTimes, " @glag=%llu", (unsigned long long)(ctx.FrameSerial () + 1 - ctx.LastGpuSerial ()));
		{
			vkc::FrameStats &st = vkc::Stats ();
			fprintf (frameTimes, " @n");
			auto cnt = [&](const char *name, uint64_t x) { if (x) fprintf (frameTimes, " %s=%llu", name, (unsigned long long)x); };
			cnt ("idle", st.idleWaits);
			if (st.idleMs > 0.0) fprintf (frameTimes, " idlems=%.3f", st.idleMs);
			cnt ("imm", st.immediate); cnt ("bufnew", st.bufNew); cnt ("buffree", st.bufFree); cnt ("imgnew", st.imgNew);
			cnt ("memnew", st.memNew); cnt ("memfree", st.memFree); cnt ("memnewmb", st.memNewBytes >> 20);
			cnt ("deferred", st.deferred); cnt ("tex", st.texUploads); cnt ("blas", st.blas); cnt ("blastri", st.blasTris);
			cnt ("tdrawn", st.tilesDrawn); cnt ("tnew", st.tilesNew); cnt ("tfreed", st.tilesFreed);
			cnt ("mplain", st.meshPlain); cnt ("mgpu", st.meshGpu); cnt ("mapplied", st.meshApplied); cnt ("mpending", st.meshPending);
			cnt ("tmeshes", st.tileMeshes); cnt ("tmb", st.tileBytes >> 20); cnt ("tdraws", st.tileDraws);
			st = vkc::FrameStats ();
		}
		uint64_t use, bud;
		if (ctx.HeapBudget (use, bud)) {
			fprintf (frameTimes, " @mem used=%llu budget=%llu", (unsigned long long)(use >> 20), (unsigned long long)(bud >> 20));
			double rss = GpuSensors::Get ().RssMb ();
			if (rss > 0) fprintf (frameTimes, " rss=%.0f", rss);
		}
		if (GpuSensors::Get ().Valid ()) {
			double sclk, mclk, watts, tj, busy;
			GpuSensors::Get ().Read (sclk, mclk, watts, tj, busy);
			fprintf (frameTimes, " @hw");
			if (sclk > 0) fprintf (frameTimes, " sclk=%.0f", sclk);
			if (mclk > 0) fprintf (frameTimes, " mclk=%.0f", mclk);
			if (watts > 0) fprintf (frameTimes, " watts=%.1f", watts);
			if (tj > -1e3) fprintf (frameTimes, " tj=%.0f", tj);
			if (busy >= 0) fprintf (frameTimes, " busy=%.0f", busy);
		}
		fputc ('\n', frameTimes);
		ctx.ClearCpuTimes ();
		lastFrameWall = now;
	} else if (frameTimes) ctx.ClearCpuTimes (), vkc::Stats () = vkc::FrameStats ();
	if (frameActive) {
		if (!videoFile.empty ()) StreamFrame ();
		if (!autoScreenshot.empty() && !videoDir.empty() && frameCount + 1 + videoFrames >= autoScreenshotFrame && frameCount + 1 < autoScreenshotFrame) {
			char name[64];
			snprintf (name, sizeof(name), "/frame_%05u.png", videoIndex++);
			CaptureBackbuffer (videoDir + name, false); // (written before the next frame: none lost at exit)
			if (FILE *f = fopen ((videoDir + "/times.txt").c_str(), videoIndex == 1 ? "w" : "a")) {
				fprintf (f, "%.6f\n", oapiGetSimTime ());
				fclose (f);
			}
		}
		if (testInput) {
			std::string shot = testInput->TakeScreenshotRequest ();
			if (!shot.empty()) {
				CaptureBackbuffer (shot);
				if (postfx) {
					float le, ll;
					postfx->ExposureState (le, ll);
					oapiWriteLogV ("VulkanClient: exposure %.3f (metered luminance %.4f)", std::exp2 (le), std::exp2 (ll));
				}
			}
		}
		if (!autoScreenshot.empty() && ++frameCount == autoScreenshotFrame) {
			vkc::LogHeaps ();
			CaptureBackbuffer (autoScreenshot);
			oapiWriteLogV ("VulkanClient: screenshot saved to %s", autoScreenshot.c_str());
			float le, ll;
			postfx->ExposureState (le, ll);
			oapiWriteLogV ("VulkanClient: exposure %.3f (metered luminance %.4f)", std::exp2 (le), std::exp2 (ll));
			// ORBITER_VK_SCREENSHOT_EXIT=1: end the session once the screenshot is taken
			const char *ex = getenv ("ORBITER_VK_SCREENSHOT_EXIT");
			if (ex && *ex && *ex != '0') oapiInputCloseRequest ();
		}
		double ts = frameTimes ? WallNow () : 0.0;
		ctx.EndFrame ();
		if (frameTimes) {
			displayEnd = WallNow ();
			ctx.CpuAdd ("submit", (displayEnd - ts) * 1e3); // (the next frame's line)
		}
	} else {
		ctx.SubmitPending (); // offscreen work recorded without a frame
	}
	frameActive = false;
	return true;
}

// ======================================================================
// Screenshots

void VulkanClient::StreamFrame ()
{
	ctx.RequestCapture ([this](const uint8_t *px, uint32_t w, uint32_t h, VkFormat fmt) {
		if (!videoPipe) {
			const bool bgr = (fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_UNORM);
			// on the GPU (VAAPI) where there is one, else x264 (ORBITER_VK_VIDEO_CODEC=x264: always)
			const char *codec = getenv ("ORBITER_VK_VIDEO_CODEC");
			const bool vaapi = !(codec && !strcmp (codec, "x264")) && access ("/dev/dri/renderD128", R_OK | W_OK) == 0;
			char cmd[1024];
			snprintf (cmd, sizeof (cmd), "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt %s -s %ux%u -framerate %g -i - %s '%s'",
				bgr ? "bgra" : "rgba", w, h, videoFps,
				vaapi ? "-vaapi_device /dev/dri/renderD128 -vf format=nv12,hwupload -c:v h264_vaapi -qp 18 -g 30"
				      : "-c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -g 30", // (a keyframe a second: seekable)
				videoFile.c_str ());
			signal (SIGPIPE, SIG_IGN); // (ffmpeg gone: a failed write, not the end of Orbiter)
			videoPipe = popen (cmd, "w");
			if (!videoPipe) { oapiWriteLogError ("VulkanClient: could not start ffmpeg for %s", videoFile.c_str ()); videoFile.clear (); return; }
			oapiWriteLogV ("VulkanClient: video to %s (%ux%u, %g fps, %s)", videoFile.c_str (), w, h, videoFps, vaapi ? "VAAPI H.264" : "x264");
		}
		if (fwrite (px, 4, (size_t)w * h, videoPipe) != (size_t)w * h) {
			oapiWriteLogError ("VulkanClient: video: ffmpeg stopped taking frames (%u written)", videoStreamed);
			CloseVideo ();
			videoFile.clear ();
			return;
		}
		videoStreamed++;
	});
}

void VulkanClient::CloseVideo ()
{
	if (!videoPipe) return;
	const int rc = pclose (videoPipe); // (ffmpeg finishes the file)
	videoPipe = nullptr;
	oapiWriteLogV ("VulkanClient: video: %u frames to %s (ffmpeg %d)", videoStreamed, videoFile.c_str (), rc);
}

void VulkanClient::CaptureBackbuffer (const std::string &path, bool async)
{
	ctx.RequestCapture ([this, path, async](const uint8_t *px, uint32_t w, uint32_t h, VkFormat fmt) {
		bool bgr = (fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_UNORM);
		auto rgb = std::make_shared<std::vector<uint8_t>> ((size_t)w * h * 3);
		uint8_t *d = rgb->data();
		for (size_t i = 0; i < (size_t)w * h; i++) {
			d[i*3+0] = px[i*4 + (bgr ? 2 : 0)];
			d[i*3+1] = px[i*4 + 1];
			d[i*3+2] = px[i*4 + (bgr ? 0 : 2)];
		}
		auto write = [path, w, h, rgb, async]() {
			if (!image::WritePNG (path, w, h, rgb->data(), async))
				oapiWriteLogError ("VulkanClient: could not write %s", path.c_str());
		};
		if (!async) { write (); return; }
		// image sequences: encoded on worker threads (a few at a time)
		writers.erase (std::remove_if (writers.begin(), writers.end(), [](std::future<void> &f) {
			return f.wait_for (std::chrono::seconds (0)) == std::future_status::ready; }), writers.end());
		while (writers.size() >= std::max (2u, std::thread::hardware_concurrency() / 2)) {
			writers.front().wait ();
			writers.erase (writers.begin());
		}
		writers.push_back (std::async (std::launch::async, write));
	});
}

bool VulkanClient::clbkSaveSurfaceToImage (SURFHANDLE surf, const char *fname,
	oapi::ImageFileFormat fmt, float quality)
{
	// only the render surface, saved as PNG regardless of the requested format
	if (surf || !fname || !ready) return false;
	std::string path (fname);
	std::replace (path.begin(), path.end(), '\\', '/'); // (core paths use Windows separators)
	CaptureBackbuffer (path + ".png", true);
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
	return (SURFHANDLE)texmgr.Load (fname, true, false, (flags & 0x10) != 0); // (bit 4: optional, no error if missing)
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

ParticleSystem *VulkanClient::Particles () const
{
	return scene ? scene->Particles () : nullptr;
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
	ctx.WaitIdle ();
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

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ========================================================================
// To be linked into all Orbiter addon modules.
// Contains standard module entry point and version information.
// ========================================================================

#ifdef _WIN32
#include <windows.h>
#else
#include "posix/win32compat.h"
#endif
#include <fstream>
#include <stdio.h>

#ifdef _WIN32
#define DLLCLBK extern "C" __declspec(dllexport)
#define OAPIFUNC __declspec(dllimport)

BOOL WINAPI DllMain (HINSTANCE hModule,
					 DWORD ul_reason_for_call,
					 LPVOID lpReserved)
{
	OAPIFUNC void InitLib (HINSTANCE hModule);
	typedef void (*DLLEXIT)(HINSTANCE);
	static DLLEXIT DLLExit;

	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
		InitLib (hModule);
		DLLExit = (DLLEXIT)GetProcAddress (hModule, "ExitModule");
		if (!DLLExit) DLLExit = (DLLEXIT)GetProcAddress (hModule, "opcDLLExit");
		break;
	case DLL_PROCESS_DETACH:
		if (DLLExit) (*DLLExit)(hModule);
		break;
	}
	return TRUE;
}
#else // ELF shared objects: constructor/destructor take the role of DllMain
#include <dlfcn.h>

#define DLLCLBK extern "C" __attribute__((visibility("default")))
#define OAPIFUNC

typedef void (*DLLEXIT)(HINSTANCE);
static HINSTANCE s_hModule = NULL;
static DLLEXIT s_DLLExit = NULL;

__attribute__((constructor)) static void OrbiterModuleAttach ()
{
	OAPIFUNC void InitLib (HINSTANCE hModule);

	// Find the handle of the module this code is linked into
	Dl_info info;
	if (!dladdr ((void*)&OrbiterModuleAttach, &info) || !info.dli_fname) return;
	void *h = dlopen (info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
	if (!h) return;
	dlclose (h); // RTLD_NOLOAD added a reference; the handle stays valid while the module is loaded
	void *hMain = dlopen (NULL, RTLD_NOW);
	if (hMain) dlclose (hMain);
	if (h == hMain) return; // linked into the executable, not a module (DllMain isn't called for executables either)
	s_hModule = (HINSTANCE)h;

	InitLib (s_hModule);
	s_DLLExit = (DLLEXIT)dlsym (h, "ExitModule");
	if (!s_DLLExit) s_DLLExit = (DLLEXIT)dlsym (h, "opcDLLExit");
}

__attribute__((destructor)) static void OrbiterModuleDetach ()
{
	if (s_DLLExit) (*s_DLLExit)(s_hModule);
}
#endif

int oapiGetModuleVersion ()
{
	static int v = 0;
	if (!v) {
		OAPIFUNC int Date2Int (char *date);
		v = Date2Int ((char*)__DATE__);
	}
	return v;
}

DLLCLBK int GetModuleVersion (void)
{
	return oapiGetModuleVersion();
}

void dummy () {}


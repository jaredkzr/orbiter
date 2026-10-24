// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Non-Windows stand-in for <windows.h>, used only by Orbiter core sources.
// Provides the Win32 types (via win32compat.h) and the few Win32 calls with
// direct POSIX equivalents (module loading and timing), implemented in
// PosixPlatform.cpp. Any other Win32 API use must be excluded from
// non-Windows builds.
#pragma once
#include "posix/win32compat.h"

#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR  0x00000100
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS  0x00001000

HMODULE LoadLibrary(LPCSTR name);
HMODULE LoadLibraryEx(LPCSTR name, HANDLE hFile, DWORD flags);
FARPROC GetProcAddress(HMODULE hModule, LPCSTR procName);
BOOL FreeLibrary(HMODULE hModule);
DWORD GetLastError();
DWORD GetModuleFileName(HMODULE hModule, LPSTR fileName, DWORD size);

/// Description of the last module loading error (POSIX only)
const char *GetLastModuleErrorString();

// Timing (implemented on top of clock_gettime / nanosleep)
DWORD timeGetTime();
DWORD GetTickCount();
BOOL QueryPerformanceCounter(LARGE_INTEGER *count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *freq);
void Sleep(DWORD ms);

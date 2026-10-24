// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// POSIX implementation of the Orbiter platform layer, plus the small set of
// Win32 module-loading calls used by the core (LoadLibrary etc.), which map
// directly onto dlopen/dlsym/dlclose.

#include "Platform.h"
#include "windows.h"
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <strings.h>
#include <sys/stat.h>
#include <string>
#include <vector>

namespace {

	bool Exists(const std::string &path)
	{
		struct stat st;
		return stat(path.c_str(), &st) == 0;
	}

	// Find an entry in directory 'dir' whose name matches 'name' ignoring case.
	bool FindNoCase(const std::string &dir, const std::string &name, std::string &match)
	{
		DIR *d = opendir(dir.empty() ? "." : dir.c_str());
		if (!d) return false;
		bool found = false;
		while (dirent *e = readdir(d)) {
			if (!strcasecmp(e->d_name, name.c_str())) {
				match = e->d_name;
				found = true;
				break;
			}
		}
		closedir(d);
		return found;
	}

	std::string g_lastError; // last module loading error message

} // namespace

std::string orbiter::platform::ResolvePath(const char *path)
{
	if (!path) return std::string();
	std::string p(path);
	for (auto &c : p)
		if (c == '\\') c = '/';
	if (p.empty() || Exists(p)) return p;

	// Resolve case mismatches component by component
	bool absolute = (p[0] == '/');
	std::string result = absolute ? "/" : "";
	size_t pos = absolute ? 1 : 0;
	bool resolving = true;
	while (pos <= p.size()) {
		size_t next = p.find('/', pos);
		if (next == std::string::npos) next = p.size();
		std::string comp = p.substr(pos, next - pos);
		if (!comp.empty()) {
			std::string base = result;
			if (!base.empty() && base.back() != '/') base += '/';
			std::string match;
			if (resolving && comp != "." && comp != ".." && !Exists(base + comp) && FindNoCase(result, comp, match))
				comp = match;
			else if (!Exists(base + comp))
				resolving = false; // component doesn't exist: keep the remainder verbatim
			result = base + comp;
		}
		pos = next + 1;
	}
	return result;
}

char *orbiter::platform::ResolvePathInPlace(char *path)
{
	if (path) {
		std::string res = ResolvePath(path);
		if (res.size() == strlen(path)) // always true, but don't risk an overflow
			memcpy(path, res.c_str(), res.size());
	}
	return path;
}

std::string orbiter::platform::ModulePath(const char *name)
{
	std::string path(name ? name : "");
	for (auto &c : path)
		if (c == '\\') c = '/';
	// Orbiter refers to modules by their Windows file names
	if (path.size() > 4 && !strcasecmp(path.c_str() + path.size() - 4, ".dll"))
		path.replace(path.size() - 4, 4, ModuleExt);
	return ResolvePath(path);
}

// ======================================================================
// Win32 module loading on top of dlopen

HMODULE LoadLibrary(LPCSTR name)
{
	std::string path = orbiter::platform::ModulePath(name);
	// dlopen only searches the library path for names without a slash
	if (path.find('/') == std::string::npos)
		path = "./" + path;

	void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (h) {
		g_lastError.clear();
	} else {
		const char *err = dlerror();
		g_lastError = err ? err : "unknown error";
	}
	return (HMODULE)h;
}

HMODULE LoadLibraryEx(LPCSTR name, HANDLE, DWORD)
{
	return LoadLibrary(name);
}

FARPROC GetProcAddress(HMODULE hModule, LPCSTR procName)
{
	return (FARPROC)dlsym((void*)hModule, procName);
}

BOOL FreeLibrary(HMODULE hModule)
{
	return hModule && dlclose((void*)hModule) == 0;
}

DWORD GetModuleFileName(HMODULE hModule, LPSTR fileName, DWORD size)
{
	if (!size) return 0;
	const char *name = "";
	link_map *lm = nullptr;
	if (hModule && dlinfo((void*)hModule, RTLD_DI_LINKMAP, &lm) == 0 && lm && lm->l_name)
		name = lm->l_name;
	strncpy(fileName, name, size - 1);
	fileName[size - 1] = '\0';
	return (DWORD)strlen(fileName);
}

DWORD GetLastError()
{
	return g_lastError.empty() ? 0 : 1;
}

const char *GetLastModuleErrorString()
{
	return g_lastError.c_str();
}

// ======================================================================
// Win32 timing functions on top of clock_gettime

#include <errno.h>
#include <time.h>

static uint64_t MonotonicNanoseconds()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

DWORD timeGetTime()
{
	return (DWORD)(MonotonicNanoseconds() / 1000000ull);
}

DWORD GetTickCount()
{
	return timeGetTime();
}

BOOL QueryPerformanceCounter(LARGE_INTEGER *count)
{
	count->QuadPart = (LONGLONG)MonotonicNanoseconds();
	return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *freq)
{
	freq->QuadPart = 1000000000ll;
	return TRUE;
}

void Sleep(DWORD ms)
{
	timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000l };
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

// ======================================================================
// File name compatibility for Windows-style paths
//
// Orbiter plugin modules (including third-party add-ons) and the core open
// files with Windows-style relative paths such as "Config\Earth\Data\x.dat",
// relying on '\' separators and a case-insensitive file system. The Orbiter
// executable exports these definitions of the libc file open functions, so
// calls from the core, from modules and from the C++ runtime (std::ifstream)
// bind to them. A path is only translated if it contains backslashes or does
// not exist as given; the call is then forwarded to the C library.
// Set ORBITER_NO_PATH_COMPAT=1 to disable the translation.

#include <fcntl.h>
#include <stdarg.h>

namespace {

	bool PathCompatEnabled()
	{
		static const bool enabled = !getenv("ORBITER_NO_PATH_COMPAT");
		return enabled;
	}

	// Returns true and sets 'mapped' if path needs translation
	bool MapPath(const char *path, std::string &mapped)
	{
		if (!path || !path[0] || !PathCompatEnabled()) return false;
		if (!strchr(path, '\\')) {
			struct stat st;
			if (stat(path, &st) == 0) return false; // exists as given
		}
		mapped = orbiter::platform::ResolvePath(path);
		return mapped != path;
	}

	template<typename F> F RealFunc(const char *name)
	{
		return (F)dlsym(RTLD_NEXT, name);
	}

} // namespace

extern "C" {

__attribute__((visibility("default"))) FILE *fopen(const char *path, const char *mode)
{
	static auto real = RealFunc<FILE*(*)(const char*, const char*)>("fopen");
	std::string mapped;
	return real(MapPath(path, mapped) ? mapped.c_str() : path, mode);
}

__attribute__((visibility("default"))) FILE *fopen64(const char *path, const char *mode)
{
	static auto real = RealFunc<FILE*(*)(const char*, const char*)>("fopen64");
	std::string mapped;
	return real(MapPath(path, mapped) ? mapped.c_str() : path, mode);
}

__attribute__((visibility("default"))) int open(const char *path, int flags, ...)
{
	static auto real = RealFunc<int(*)(const char*, int, ...)>("open");
	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	std::string mapped;
	return real(MapPath(path, mapped) ? mapped.c_str() : path, flags, mode);
}

__attribute__((visibility("default"))) int open64(const char *path, int flags, ...)
{
	static auto real = RealFunc<int(*)(const char*, int, ...)>("open64");
	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	std::string mapped;
	return real(MapPath(path, mapped) ? mapped.c_str() : path, flags, mode);
}

} // extern "C"

// ======================================================================
// Mouse state reported by the graphics client

namespace {
	int g_mouseX = 0, g_mouseY = 0;   // current position in the render window
	int g_mouseDx = 0, g_mouseDy = 0; // accumulated relative motion
}

void orbiter::platform::SetMousePos(int x, int y)
{
	g_mouseX = x, g_mouseY = y;
}

void orbiter::platform::GetMousePos(int &x, int &y)
{
	x = g_mouseX, y = g_mouseY;
}

void orbiter::platform::AddMouseMotion(int dx, int dy)
{
	g_mouseDx += dx, g_mouseDy += dy;
}

void orbiter::platform::TakeMouseMotion(int &dx, int &dy)
{
	dx = g_mouseDx, dy = g_mouseDy;
	g_mouseDx = g_mouseDy = 0;
}

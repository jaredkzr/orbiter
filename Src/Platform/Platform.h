// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Small platform abstraction layer for the Orbiter core.
// On Windows these functions are thin pass-throughs, so Windows behaviour is
// unchanged. On other platforms they translate Windows-style paths and file
// names (as found in Orbiter's code and data files) to the host conventions.

#ifndef __ORBITER_PLATFORM_H
#define __ORBITER_PLATFORM_H

#include <string>

namespace orbiter {
namespace platform {

#ifdef _WIN32
	/// File name extension of loadable plugin modules
	constexpr const char *ModuleExt = ".dll";

	/// Map a path from Orbiter's Windows conventions to a host path.
	inline std::string ResolvePath(const char *path) { return path ? path : ""; }
	inline std::string ResolvePath(const std::string &path) { return path; }

	/// Resolve a path in its own buffer (the resolved path has the same length).
	inline char *ResolvePathInPlace(char *path) { return path; }

	/// Host file name of a plugin module given by its Windows file name (*.dll)
	inline std::string ModulePath(const char *path) { return path ? path : ""; }
#else
	constexpr const char *ModuleExt = ".so";

	/// Map a path from Orbiter's Windows conventions to a host path:
	/// backslashes become forward slashes, and each path component is
	/// matched case-insensitively against existing directory entries, since
	/// Orbiter's code and data files assume a case-insensitive file system.
	/// Components that don't exist (e.g. a file about to be created) are kept as given.
	std::string ResolvePath(const char *path);
	inline std::string ResolvePath(const std::string &path) { return ResolvePath(path.c_str()); }

	/// Resolve a path in its own buffer. Resolution only changes separators and
	/// letter case, so the resolved path always has the same length.
	char *ResolvePathInPlace(char *path);

	/// Host file name of a plugin module given by its Windows file name:
	/// a ".dll" extension is replaced by ModuleExt, and the path is resolved.
	std::string ModulePath(const char *path);

	/// Mouse state reported by the graphics client (Windows queries the cursor directly)
	void SetMousePos(int x, int y);
	void GetMousePos(int &x, int &y);
	void AddMouseMotion(int dx, int dy);
	/// Returns and resets the relative mouse motion accumulated since the last call
	void TakeMouseMotion(int &dx, int &dy);
#endif

} // namespace platform
} // namespace orbiter

#endif // !__ORBITER_PLATFORM_H

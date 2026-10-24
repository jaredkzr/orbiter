// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Definitions the FidelityFX SDK sources expect from the Windows CRT,
// force-included when building the SDK subset with GCC/Clang

#pragma once

#include <cstring>
#include <cwchar>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <codecvt>
#include <locale>
#include <string>

#ifndef _countof
#define _countof(a) (sizeof (a) / sizeof ((a)[0]))
#endif

#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

inline int wcscpy_s (wchar_t *dst, size_t n, const wchar_t *src)
{
	if (!dst || !n) return 1;
	wcsncpy (dst, src, n - 1);
	dst[n - 1] = L'\0';
	return 0;
}
template <size_t N> inline int wcscpy_s (wchar_t (&dst)[N], const wchar_t *src) { return wcscpy_s (dst, N, src); }

inline int strcpy_s (char *dst, size_t n, const char *src)
{
	if (!dst || !n) return 1;
	strncpy (dst, src, n - 1);
	dst[n - 1] = '\0';
	return 0;
}
template <size_t N> inline int strcpy_s (char (&dst)[N], const char *src) { return strcpy_s (dst, N, src); }

#include <cmath>
#include <cstdarg>
using std::floor;
using std::log2;

inline int sprintf_s (char *dst, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	int r = vsnprintf (dst, n, fmt, ap);
	va_end (ap);
	return r;
}
template <size_t N> inline int sprintf_s (char (&dst)[N], const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	int r = vsnprintf (dst, N, fmt, ap);
	va_end (ap);
	return r;
}

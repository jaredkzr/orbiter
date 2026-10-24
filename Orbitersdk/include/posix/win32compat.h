// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Minimal Win32 type definitions for non-Windows builds.
// This header only provides the types, constants and trivial macros that
// appear in shared (platform-neutral) signatures. It deliberately does not
// emulate Win32 behaviour: code that calls into the Win32 API must be
// excluded from non-Windows builds instead.

#ifndef __ORBITER_WIN32COMPAT_H
#define __ORBITER_WIN32COMPAT_H

#ifdef _WIN32
#error "win32compat.h must not be used on Windows"
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Calling conventions and storage classes

#define WINAPI
#define PASCAL
#define FAR
#define NEAR
#define CALLBACK
#define APIENTRY
#define __cdecl
#define __stdcall
#define __forceinline inline __attribute__((always_inline))

// ---------------------------------------------------------------------------
// Basic types (sizes match the Win32 data model)

typedef void VOID;
typedef char CHAR;
typedef char TCHAR;
typedef unsigned char BYTE;
typedef unsigned char UCHAR;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t DWORD64;
typedef uint64_t DWORDLONG;
typedef int INT;
typedef unsigned int UINT;
typedef int BOOL;
typedef float FLOAT;
typedef short SHORT;
typedef int8_t INT8;
typedef uint8_t UINT8;
typedef int16_t INT16;
typedef uint16_t UINT16;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef int64_t INT64;
typedef uint64_t UINT64;
typedef unsigned short USHORT;
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef size_t SIZE_T;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef int32_t HRESULT;
typedef DWORD COLORREF;
typedef wchar_t WCHAR;
typedef wchar_t *LPWSTR;
typedef const wchar_t *LPCWSTR;
typedef char *PSTR;
typedef const char *PCSTR;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef char *LPTSTR;
typedef const char *LPCTSTR;
typedef void *LPVOID;
typedef BYTE *LPBYTE;
typedef const void *LPCVOID;
typedef DWORD *LPDWORD;
typedef BOOL *LPBOOL;
typedef long long __int64;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define MAX_PATH 260
#define INFINITE 0xFFFFFFFF // infinite timeout
#define TEXT(s) s

// ---------------------------------------------------------------------------
// Opaque handle types. Each handle is a distinct pointer type so that
// overloads on handle types keep working.

#define ORBITER_DECLARE_HANDLE(name) struct name##__ { int unused; }; typedef struct name##__ *name
typedef void *HANDLE;
ORBITER_DECLARE_HANDLE(HWND);
ORBITER_DECLARE_HANDLE(HDC);
ORBITER_DECLARE_HANDLE(HINSTANCE);
ORBITER_DECLARE_HANDLE(HFONT);
ORBITER_DECLARE_HANDLE(HPEN);
ORBITER_DECLARE_HANDLE(HBRUSH);
ORBITER_DECLARE_HANDLE(HBITMAP);
ORBITER_DECLARE_HANDLE(HICON);
ORBITER_DECLARE_HANDLE(HMENU);
ORBITER_DECLARE_HANDLE(HKEY);
ORBITER_DECLARE_HANDLE(HRGN);
ORBITER_DECLARE_HANDLE(HGLRC);
ORBITER_DECLARE_HANDLE(HMONITOR);
ORBITER_DECLARE_HANDLE(HTREEITEM);
typedef HINSTANCE HMODULE;
typedef HICON HCURSOR;
typedef HANDLE HGDIOBJ;
#undef ORBITER_DECLARE_HANDLE

// ---------------------------------------------------------------------------
// Structures

typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef const RECT *LPCRECT;
typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT;
typedef struct tagSIZE { LONG cx, cy; } SIZE, *LPSIZE;
typedef struct tagPOINTS { SHORT x, y; } POINTS;
typedef struct tagNMHDR { HWND hwndFrom; UINT_PTR idFrom; UINT code; } NMHDR, *LPNMHDR;
typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG, *LPMSG;

typedef union _LARGE_INTEGER {
	struct { DWORD LowPart; LONG HighPart; };
	LONGLONG QuadPart;
} LARGE_INTEGER;

typedef INT_PTR (CALLBACK *DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef INT_PTR (*FARPROC)();

// ---------------------------------------------------------------------------
// Result codes

#define S_OK    ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL  ((HRESULT)0x80004005L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr)    (((HRESULT)(hr)) < 0)

// ---------------------------------------------------------------------------
// Window message identifiers. Orbiter also uses these values as its own
// event codes (e.g. mouse events passed to panels and modules).

#define WM_CREATE        0x0001
#define WM_DESTROY       0x0002
#define WM_CLOSE         0x0010
#define WM_KEYDOWN       0x0100
#define WM_KEYUP         0x0101
#define WM_CHAR          0x0102
#define WM_COMMAND       0x0111
#define WM_TIMER         0x0113
#define WM_MOUSEMOVE     0x0200
#define WM_LBUTTONDOWN   0x0201
#define WM_LBUTTONUP     0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN   0x0204
#define WM_RBUTTONUP     0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN   0x0207
#define WM_MBUTTONUP     0x0208
#define WM_MOUSEWHEEL    0x020A
#define WM_USER          0x0400

// System error codes reported by module loading
#define ERROR_SUCCESS        0L
#define ERROR_MOD_NOT_FOUND  126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_BAD_EXE_FORMAT 193L

// ---------------------------------------------------------------------------
// Word and colour macros

#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)MAKELONG(l, h))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(rgb) (LOBYTE(rgb))
#define GetGValue(rgb) (LOBYTE(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) (LOBYTE((rgb) >> 16))

// GDI pen styles (also used by the Orbiter API, e.g. oapiCreatePen)
#define PS_SOLID 0
#define PS_DASH  1
#define PS_DOT   2
#define PS_NULL  5

#define ZeroMemory(p, n) memset((p), 0, (n))
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

// ---------------------------------------------------------------------------
// MSVC C runtime names

#define _stricmp strcasecmp
#define stricmp strcasecmp
#define _strnicmp strncasecmp
#define strnicmp strncasecmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _strdup strdup
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _fileno fileno
#define _getcwd getcwd
#define _chdir chdir
#define _putenv putenv
#define _unlink unlink
#define lstrlen strlen

#ifdef __cplusplus
#include <errno.h>
#include <stdarg.h>

// Bounds-checked MSVC CRT variants, implemented on top of the standard C library
inline int fopen_s(FILE **pf, const char *fname, const char *mode)
{
	*pf = fopen(fname, mode);
	return *pf ? 0 : errno;
}

inline int strcpy_s(char *dst, size_t size, const char *src)
{
	if (!dst || !size) return EINVAL;
	if (!src) { dst[0] = '\0'; return EINVAL; }
	size_t n = strlen(src);
	if (n >= size) { dst[0] = '\0'; return ERANGE; }
	memcpy(dst, src, n + 1);
	return 0;
}

template <size_t N> inline int strcpy_s(char (&dst)[N], const char *src) { return strcpy_s(dst, N, src); }

inline int strncpy_s(char *dst, size_t size, const char *src, size_t count)
{
	if (!dst || !size) return EINVAL;
	size_t n = strnlen(src, count);
	if (n >= size) { dst[0] = '\0'; return ERANGE; }
	memcpy(dst, src, n);
	dst[n] = '\0';
	return 0;
}

template <size_t N> inline int strncpy_s(char (&dst)[N], const char *src, size_t count) { return strncpy_s(dst, N, src, count); }

inline int sprintf_s(char *dst, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, size, fmt, ap);
	va_end(ap);
	return n;
}

template <size_t N> inline int sprintf_s(char (&dst)[N], const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, N, fmt, ap);
	va_end(ap);
	return n;
}
#endif // __cplusplus

#endif // !__ORBITER_WIN32COMPAT_H

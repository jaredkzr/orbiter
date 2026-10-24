// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Data-type subset of the legacy Direct3D 7 / DirectDraw headers for
// non-Windows builds. The Orbiter core still uses a few D3D7 structures
// (D3DMATRIX, D3DVECTOR, D3DMATERIAL7, ...) as plain math and mesh data
// containers. Device and surface interfaces are opaque here: code that
// talks to a Direct3D device must be excluded from non-Windows builds.

#pragma once
#include "posix/win32compat.h"

typedef float D3DVALUE, *LPD3DVALUE;
typedef DWORD D3DCOLOR;
#define D3DVAL(val) ((float)(val))
#define D3DRGBA(r, g, b, a) \
	((((long)((a) * 255)) << 24) | (((long)((r) * 255)) << 16) | \
	 (((long)((g) * 255)) << 8) | (long)((b) * 255))
#define D3DRGB(r, g, b) \
	(0xff000000L | (((long)((r) * 255)) << 16) | (((long)((g) * 255)) << 8) | (long)((b) * 255))

typedef struct _D3DVECTOR {
	union { D3DVALUE x; D3DVALUE dvX; };
	union { D3DVALUE y; D3DVALUE dvY; };
	union { D3DVALUE z; D3DVALUE dvZ; };
} D3DVECTOR, *LPD3DVECTOR;

typedef struct _D3DMATRIX {
	union {
		struct {
			D3DVALUE _11, _12, _13, _14;
			D3DVALUE _21, _22, _23, _24;
			D3DVALUE _31, _32, _33, _34;
			D3DVALUE _41, _42, _43, _44;
		};
		D3DVALUE m[4][4];
	};
} D3DMATRIX, *LPD3DMATRIX;

typedef struct _D3DCOLORVALUE {
	union { D3DVALUE r; D3DVALUE dvR; };
	union { D3DVALUE g; D3DVALUE dvG; };
	union { D3DVALUE b; D3DVALUE dvB; };
	union { D3DVALUE a; D3DVALUE dvA; };
} D3DCOLORVALUE, *LPD3DCOLORVALUE;

typedef struct _D3DMATERIAL7 {
	union { D3DCOLORVALUE diffuse;  D3DCOLORVALUE dcvDiffuse; };
	union { D3DCOLORVALUE ambient;  D3DCOLORVALUE dcvAmbient; };
	union { D3DCOLORVALUE specular; D3DCOLORVALUE dcvSpecular; };
	union { D3DCOLORVALUE emissive; D3DCOLORVALUE dcvEmissive; };
	union { D3DVALUE power; D3DVALUE dvPower; };
} D3DMATERIAL7, *LPD3DMATERIAL7;

typedef struct _D3DVERTEX {
	union { D3DVALUE x;  D3DVALUE dvX; };
	union { D3DVALUE y;  D3DVALUE dvY; };
	union { D3DVALUE z;  D3DVALUE dvZ; };
	union { D3DVALUE nx; D3DVALUE dvNX; };
	union { D3DVALUE ny; D3DVALUE dvNY; };
	union { D3DVALUE nz; D3DVALUE dvNZ; };
	union { D3DVALUE tu; D3DVALUE dvTU; };
	union { D3DVALUE tv; D3DVALUE dvTV; };
} D3DVERTEX, *LPD3DVERTEX;

// Flexible vertex format flags (used to describe vertex layouts)
#define D3DFVF_XYZ          0x002
#define D3DFVF_XYZRHW       0x004
#define D3DFVF_NORMAL       0x010
#define D3DFVF_DIFFUSE      0x040
#define D3DFVF_SPECULAR     0x080
#define D3DFVF_TEX1         0x100
#define D3DFVF_TEX2         0x200
#define D3DFVF_TEXTUREFORMAT2 0
#define D3DFVF_TEXCOORDSIZE2(CoordIndex) (D3DFVF_TEXTUREFORMAT2)
#define D3DFVF_VERTEX (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

// Opaque DirectDraw / Direct3D 7 interfaces
struct IDirectDraw7;
struct IDirectDrawSurface7;
struct IDirect3D7;
struct IDirect3DDevice7;
struct IDirect3DVertexBuffer7 { // COM interface; only reference counting is declared here
	virtual ULONG Release() = 0;
protected:
	virtual ~IDirect3DVertexBuffer7() {}
};
typedef struct IDirectDraw7 *LPDIRECTDRAW7;
typedef struct IDirectDrawSurface7 *LPDIRECTDRAWSURFACE7;
typedef struct IDirect3D7 *LPDIRECT3D7;
typedef struct IDirect3DDevice7 *LPDIRECT3DDEVICE7;
typedef struct IDirect3DVertexBuffer7 *LPDIRECT3DVERTEXBUFFER7;

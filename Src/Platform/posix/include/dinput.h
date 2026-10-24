// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Non-Windows stand-in for <dinput.h>: data types only.
// The DirectInput device code (Input.cpp, Di7frame.cpp) is not built on
// non-Windows platforms; these definitions only let shared headers compile.
#pragma once
#include "posix/win32compat.h"

typedef struct _GUID {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t  Data4[8];
} GUID;

struct IDirectInput8;
struct IDirectInputDevice8;
typedef struct IDirectInput8 *LPDIRECTINPUT8;
typedef struct IDirectInputDevice8 *LPDIRECTINPUTDEVICE8;

typedef struct DIDEVICEINSTANCE {
	DWORD dwSize;
	GUID  guidInstance;
	GUID  guidProduct;
	DWORD dwDevType;
	CHAR  tszInstanceName[MAX_PATH];
	CHAR  tszProductName[MAX_PATH];
} DIDEVICEINSTANCE, *LPDIDEVICEINSTANCE;
typedef const DIDEVICEINSTANCE *LPCDIDEVICEINSTANCE;

typedef struct _DIDATAFORMAT DIDATAFORMAT;

typedef struct DIDEVICEOBJECTDATA {
	DWORD    dwOfs;
	DWORD    dwData;
	DWORD    dwTimeStamp;
	DWORD    dwSequence;
	UINT_PTR uAppData;
} DIDEVICEOBJECTDATA, *LPDIDEVICEOBJECTDATA;

// Keyboard scan codes (identical to Orbiter's OAPI_KEY_* codes)
#define DIK_LSHIFT 0x2A
#define DIK_F3     0x3D

typedef struct DIJOYSTATE2 {
	LONG  lX, lY, lZ;
	LONG  lRx, lRy, lRz;
	LONG  rglSlider[2];
	DWORD rgdwPOV[4];
	BYTE  rgbButtons[128];
	LONG  lVX, lVY, lVZ;
	LONG  lVRx, lVRy, lVRz;
	LONG  rglVSlider[2];
	LONG  lAX, lAY, lAZ;
	LONG  lARx, lARy, lARz;
	LONG  rglASlider[2];
	LONG  lFX, lFY, lFZ;
	LONG  lFRx, lFRy, lFRz;
	LONG  rglFSlider[2];
} DIJOYSTATE2, *LPDIJOYSTATE2;

// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#ifndef __MEMSTAT_H
#define __MEMSTAT_H

#include <windows.h>
#ifdef _WIN32
#include <psapi.h>

typedef BOOL (CALLBACK *Proc_GetProcessMemoryInfo)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
#endif

class MemStat {
public:
    MemStat ();
    ~MemStat ();

    long HeapUsage ();

#ifdef _WIN32
private:
    static HMODULE hLib;
	static bool bLib;
    HANDLE hProc;
	Proc_GetProcessMemoryInfo pGetProcessMemoryInfo;
    bool active;
#endif
};

#endif // !__MEMSTAT_H
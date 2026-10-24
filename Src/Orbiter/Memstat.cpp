// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Memstat.h"

#ifdef _WIN32

bool MemStat::bLib = false;
HMODULE MemStat::hLib = 0;

MemStat::MemStat ()
{
	if (!bLib) {
		hLib = LoadLibrary ("Psapi.dll");
		bLib = true;
	}
    hProc = GetCurrentProcess();
    active = (hLib != NULL && hProc != NULL);
	if (active) {
		pGetProcessMemoryInfo = (Proc_GetProcessMemoryInfo)GetProcAddress (hLib, "GetProcessMemoryInfo");
	} else {
		pGetProcessMemoryInfo = 0;
	}
}

MemStat::~MemStat ()
{
    if (hProc) CloseHandle (hProc);
}

long MemStat::HeapUsage ()
{
	if (pGetProcessMemoryInfo) {
	    PROCESS_MEMORY_COUNTERS pmc;
		pGetProcessMemoryInfo (hProc, &pmc, sizeof(pmc));
		return (long)pmc.WorkingSetSize;
	} else return 0;
}

#else // !_WIN32

#include <stdio.h>
#include <unistd.h>

MemStat::MemStat ()
{
}

MemStat::~MemStat ()
{
}

long MemStat::HeapUsage ()
{
	// resident set size, the closest equivalent of the Windows working set
	long pages = 0, resident = 0;
	FILE *f = fopen ("/proc/self/statm", "r");
	if (!f) return 0;
	if (fscanf (f, "%ld %ld", &pages, &resident) != 2) resident = 0;
	fclose (f);
	return resident * sysconf (_SC_PAGESIZE);
}

#endif // _WIN32

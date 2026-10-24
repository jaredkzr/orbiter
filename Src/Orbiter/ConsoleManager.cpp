#include "ConsoleManager.h"

#include <windows.h>

#ifdef _WIN32

bool ConsoleManager::IsConsoleExclusive(void) {
    DWORD pids[2];
    DWORD num_pids = GetConsoleProcessList(pids, 2);
    return num_pids <= 1;
}

void ConsoleManager::ShowConsole(bool show)
{
    HWND wnd = GetConsoleWindow();
    if (wnd)
        ShowWindow(wnd, show ? SW_SHOW : SW_HIDE);
}

#else // !_WIN32: the process runs in the terminal it was started from

bool ConsoleManager::IsConsoleExclusive(void)
{
    return false;
}

void ConsoleManager::ShowConsole(bool show)
{
}

#endif // _WIN32

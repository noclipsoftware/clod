// clod: a terminal tab manager for Claude Code sessions.
//
//   clod             open a window attached to the background server (starting it if needed)
//   clod --tab N     same, switching to tab N (0 = sessions page)
//   clod --test      a throwaway instance for trying a new build next to the running clod: its own
//                    server with no tray and no saved tabs, which ends when this window closes
//   clod --server    the background server itself; started by the first client
//
// Unity build: this file includes everything else.

#define _CRT_RAND_S
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

#include "clod_base.h"
#include "clod_vt.cpp"
#include "clod_sessions.cpp"
#include "clod_app.cpp"
#include "clod_pty.cpp"
#include "clod_ipc.cpp"
#include "clod_tray.cpp"
#include "clod_server.cpp"
#include "clod_client.cpp"

int wmain(int Argc, wchar_t** Argv)
{
    bool32 Server = false;
    s32 JumpToTab = 0;
    for(s32 I = 1; I < Argc; ++I)
    {
        if(wcscmp(Argv[I], L"--server") == 0) { Server = true; }
        else if(wcscmp(Argv[I], L"--tab") == 0 && I + 1 < Argc) { JumpToTab = _wtoi(Argv[++I]) + 1; }
        else if(wcscmp(Argv[I], L"--test") == 0)
        {
            // The server this client spawns inherits these: a private pipe and throwaway behaviour.
            std::wstring Instance = L"test-" + std::to_wstring(GetCurrentProcessId());
            SetEnvironmentVariableW(L"CLOD_INSTANCE", Instance.c_str());
            SetEnvironmentVariableW(L"CLOD_EPHEMERAL", L"1");
        }
    }
    return Server ? ServerMain() : ClientMain(JumpToTab);
}

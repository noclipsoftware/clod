// clod: a terminal tab manager for Claude Code sessions.
//
//   clod             open a window attached to the background server (starting it if needed)
//   clod --tab N     same, switching to tab N (0 = home screen)
//   clod --test      a throwaway instance for trying a new build next to the running clod: its own
//                    server with no tray and no saved tabs, which ends when this window closes
//   clod --server    the background server itself; started by the first client
//   clod view FILE   in a clod tab: show an image over the tab (any key closes it)
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
#include <winternl.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

#include "clod_base.h"
#include "clod_vt.cpp"
#include "clod_sessions.cpp"
#include "clod_app.cpp"
#include "clod_ipc.cpp"
#include "clod_pty.cpp"
#include "clod_settings.cpp"
#include "clod_image.cpp"
#include "clod_tray.cpp"
#include "clod_server.cpp"
#include "clod_client.cpp"

int wmain(int Argc, wchar_t** Argv)
{
    // --- clod view <file>: run in a clod tab, shows an image over it. Writes OSC 7777 to its console,
    // which ConPTY hands to the server like any output; the server reads the file ---
    if(Argc >= 2 && wcscmp(Argv[1], L"view") == 0)
    {
        if(Argc < 3) { fwprintf(stderr, L"usage: clod view <image file>\n"); return 1; }
        wchar_t Full[4096] = {};
        DWORD Attr = GetFullPathNameW(Argv[2], ArrayCount(Full), Full, 0) ? GetFileAttributesW(Full) : INVALID_FILE_ATTRIBUTES;
        if(Attr == INVALID_FILE_ATTRIBUTES || (Attr & FILE_ATTRIBUTE_DIRECTORY)) { fwprintf(stderr, L"clod view: no such file: %s\n", Argv[2]); return 1; }
        if(!GetEnvironmentVariableW(L"CLOD_DIR", 0, 0)) { fwprintf(stderr, L"clod view: shows images only in a clod tab\n"); return 1; }
        std::wstring Seq = std::wstring(L"\x1b]7777;view;") + Full + L"\x07";
        DWORD Written = 0;
        if(!WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), Seq.c_str(), (DWORD)Seq.size(), &Written, 0))
        {
            fwprintf(stderr, L"clod view: needs its output on the tab's console, not redirected\n");
            return 1;
        }
        return 0;
    }

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

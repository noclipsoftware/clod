// clodw: clod's own window. A GUI client of the clod server that draws the server's cell grid with
// Direct3D 11 (glyphs from DirectWrite), instead of handing VT output to someone else's terminal.
//
//   clodw             open a window attached to the background server (starting it if needed)
//   clodw --tab N     same, switching to tab N (0 = sessions page)
//   clodw --test      a throwaway private server for trying a new build; it ends with the window
//
// Unity build: this file includes everything else.

#define _CRT_RAND_S
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <d2d1.h>
#include <dwrite_2.h>
#include <commdlg.h>
#include <imm.h>

#include "clod_base.h"
#include "clod_ipc.cpp"
#include "clod_settings.cpp"
#include "clodw_render.cpp"
#include "clodw_glyphs.cpp"
#include "clodw_main.cpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    s32 JumpToTab = 0;
    int Argc = 0;
    wchar_t** Argv = CommandLineToArgvW(GetCommandLineW(), &Argc);
    for(s32 I = 1; I < Argc; ++I)
    {
        if(wcscmp(Argv[I], L"--tab") == 0 && I + 1 < Argc) { JumpToTab = _wtoi(Argv[++I]) + 1; }
        else if(wcscmp(Argv[I], L"--test") == 0)
        {
            // The server this window spawns inherits these: a private pipe and throwaway behaviour.
            std::wstring Instance = L"test-" + std::to_wstring(GetCurrentProcessId());
            SetEnvironmentVariableW(L"CLOD_INSTANCE", Instance.c_str());
            SetEnvironmentVariableW(L"CLOD_EPHEMERAL", L"1");
        }
    }
    LocalFree(Argv);
    return WindowMain(JumpToTab);
}

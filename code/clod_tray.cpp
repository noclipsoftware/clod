// Notification-area icon for the server: tab menu, open/quit, and balloon notifications.
// The window procedure only records what the user asked for; the server loop acts on it.

#define WM_CLOD_TRAY (WM_APP + 1)
#define TRAY_ICON_ID 1

enum tray_action
{
    TRAY_NONE = 0,
    TRAY_OPEN, // open or focus a client, at ActionTab
    TRAY_QUIT, // stop clod; the tabs reopen with --resume on the next launch
};

enum
{
    TRAY_MENU_OPEN = 1,
    TRAY_MENU_QUIT = 2,
    TRAY_MENU_TAB = 100, // + tab index
};

struct tray_state
{
    HWND Window;
    HICON Icon;          // small, for the notification area
    HICON LargeIcon;     // for balloon notifications
    UINT TaskbarCreated; // broadcast when explorer restarts; the icon must be re-added
    s32 Action;
    s32 ActionTab;       // tab index + 1; 0 = stay on the current tab
    s32 BalloonTab;      // tab index + 1 the last balloon was about
    const app_state* App;
};

// Popup menus stay light unless the process opts into dark mode. The switch is undocumented
// uxtheme exports (by ordinal) that Explorer and most tray apps use. Follows the taskbar's theme
// (SystemUsesLightTheme), which is what the other tray icons' menus match.
static void TrayApplyMenuTheme()
{
    typedef int (WINAPI *set_preferred_app_mode)(int Mode); // 0 default, 1 allow dark, 2 force dark, 3 force light
    typedef void (WINAPI *flush_menu_themes)();
    HMODULE UxTheme = LoadLibraryExW(L"uxtheme.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!UxTheme) { return; }
    set_preferred_app_mode SetPreferredAppMode = (set_preferred_app_mode)GetProcAddress(UxTheme, MAKEINTRESOURCEA(135));
    flush_menu_themes FlushMenuThemes = (flush_menu_themes)GetProcAddress(UxTheme, MAKEINTRESOURCEA(136));
    if(!SetPreferredAppMode || !FlushMenuThemes) { return; }

    DWORD Light = 1;
    DWORD Size = sizeof(Light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, 0, &Light, &Size);
    SetPreferredAppMode(Light ? 3 : 2);
    FlushMenuThemes();
}

static NOTIFYICONDATAW TrayIconData(const tray_state& Tray)
{
    NOTIFYICONDATAW N = {};
    N.cbSize = sizeof(N);
    N.hWnd = Tray.Window;
    N.uID = TRAY_ICON_ID;
    return N;
}

static void TrayAddIcon(tray_state& Tray)
{
    NOTIFYICONDATAW N = TrayIconData(Tray);
    N.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    N.uCallbackMessage = WM_CLOD_TRAY;
    N.hIcon = Tray.Icon;
    wcscpy_s(N.szTip, L"clod");
    Shell_NotifyIconW(NIM_ADD, &N);
    N.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &N);
}

static void TraySetTip(tray_state& Tray, const std::string& Tip)
{
    if(!Tray.Window) { return; } // no tray (test instance)
    NOTIFYICONDATAW N = TrayIconData(Tray);
    N.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(N.szTip, Utf8ToWide(Tip).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &N);
}

static void TrayNotify(tray_state& Tray, const std::string& Title, const std::string& Body, s32 TabIndex)
{
    if(!Tray.Window) { return; } // no tray (test instance)
    NOTIFYICONDATAW N = TrayIconData(Tray);
    N.uFlags = NIF_INFO;
    N.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;
    N.hBalloonIcon = Tray.LargeIcon;
    wcsncpy_s(N.szInfoTitle, Utf8ToWide(Title).c_str(), _TRUNCATE);
    wcsncpy_s(N.szInfo, Utf8ToWide(Body).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &N);
    Tray.BalloonTab = TabIndex + 1;
}

static LRESULT CALLBACK TrayWindowProc(HWND Window, UINT Msg, WPARAM WParam, LPARAM LParam)
{
    tray_state* Tray = (tray_state*)GetWindowLongPtrW(Window, GWLP_USERDATA);
    if(!Tray) { return DefWindowProcW(Window, Msg, WParam, LParam); }

    if(Msg == Tray->TaskbarCreated)
    {
        TrayAddIcon(*Tray);
        return 0;
    }
    if(Msg == WM_SETTINGCHANGE && LParam && wcscmp((const wchar_t*)LParam, L"ImmersiveColorSet") == 0)
    {
        TrayApplyMenuTheme(); // light/dark switched in Settings
        return 0;
    }
    if(Msg != WM_CLOD_TRAY) { return DefWindowProcW(Window, Msg, WParam, LParam); }

    switch(LOWORD(LParam))
    {
        // Single click opens. A double click also starts with NIN_SELECT, so it isn't handled separately.
        case NIN_SELECT:
        case NIN_KEYSELECT:
        {
            Tray->Action = TRAY_OPEN;
            Tray->ActionTab = 0;
        } break;
        case NIN_BALLOONUSERCLICK:
        {
            Tray->Action = TRAY_OPEN;
            Tray->ActionTab = Tray->BalloonTab;
        } break;
        case WM_CONTEXTMENU:
        {
            // --- menu: one entry per tab, then open / quit ---
            const app_state& App = *Tray->App;
            HMENU Menu = CreatePopupMenu();
            std::string Header = "clod - " + std::to_string(App.Tabs.size()) + " session" + (App.Tabs.size() == 1 ? "" : "s");
            AppendMenuW(Menu, MF_STRING | MF_GRAYED, 0, Utf8ToWide(Header).c_str());
            AppendMenuW(Menu, MF_SEPARATOR, 0, 0);
            for(s32 I = 0; I < (s32)App.Tabs.size(); ++I)
            {
                std::string Label = std::to_string(I + 1) + "  " + TruncateText(TabName(*App.Tabs[I]), 48);
                if(App.Tabs[I]->Exited) { Label += "  (exited)"; }
                UINT Flags = MF_STRING | (App.Active == I + 1 ? MF_CHECKED : 0);
                AppendMenuW(Menu, Flags, TRAY_MENU_TAB + I, Utf8ToWide(Label).c_str());
            }
            if(!App.Tabs.empty()) { AppendMenuW(Menu, MF_SEPARATOR, 0, 0); }
            AppendMenuW(Menu, MF_STRING | MF_DEFAULT, TRAY_MENU_OPEN, L"Open clod");
            AppendMenuW(Menu, MF_SEPARATOR, 0, 0);
            AppendMenuW(Menu, MF_STRING, TRAY_MENU_QUIT, L"Quit");

            // Without foreground the menu never closes when clicking elsewhere.
            SetForegroundWindow(Window);
            s32 Picked = (s32)TrackPopupMenu(Menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                             GET_X_LPARAM(WParam), GET_Y_LPARAM(WParam), 0, Window, 0);
            PostMessageW(Window, WM_NULL, 0, 0);
            DestroyMenu(Menu);

            if(Picked == TRAY_MENU_OPEN) { Tray->Action = TRAY_OPEN; Tray->ActionTab = 0; }
            else if(Picked >= TRAY_MENU_TAB) { Tray->Action = TRAY_OPEN; Tray->ActionTab = Picked - TRAY_MENU_TAB + 2; }
            else if(Picked == TRAY_MENU_QUIT)
            {
                // Quitting keeps every tab (they reopen next launch, claude tabs resumed). What is lost
                // is work in progress: a reply (claude's title spinner is running) or a program running
                // in a shell tab (cmd at its prompt has no child processes). Ask only when there is some.
                std::vector<PROCESSENTRY32W> Procs = ListProcesses();
                std::wstring Busy;
                std::wstring Shells;
                s32 BusyCount = 0;
                s32 ShellCount = 0;
                for(s32 I = 0; I < (s32)App.Tabs.size(); ++I)
                {
                    const tab* T = App.Tabs[I];
                    if(T->Exited) { continue; }
                    std::wstring Line = L"\n    " + std::to_wstring(I + 1) + L"  " + Utf8ToWide(TruncateText(TabName(*T), 60));
                    if(!T->SessionId.empty())
                    {
                        if(T->TitleBusy) { Busy += Line; BusyCount += 1; }
                    }
                    else if(T->Pty)
                    {
                        DWORD Pid = GetProcessId(T->Pty->Process);
                        std::wstring Running;
                        for(const PROCESSENTRY32W& P : Procs)
                        {
                            if(P.th32ParentProcessID == Pid) { Running += (Running.empty() ? L"" : L", ") + std::wstring(P.szExeFile); }
                        }
                        if(!Running.empty()) { Shells += Line + L"  (" + Running + L")"; ShellCount += 1; }
                    }
                }
                bool32 Quit = true;
                if(BusyCount || ShellCount)
                {
                    std::wstring Ask;
                    if(BusyCount)
                    {
                        Ask += (BusyCount == 1 ? L"This session is" : std::to_wstring(BusyCount) + L" sessions are") +
                               std::wstring(L" in the middle of a reply:\n") + Busy + L"\n\nQuitting now cuts the reply off.\n\n";
                    }
                    if(ShellCount)
                    {
                        Ask += (ShellCount == 1 ? L"This shell is" : std::to_wstring(ShellCount) + L" shells are") +
                               std::wstring(L" running a program:\n") + Shells +
                               L"\n\nQuitting ends those programs. The shells reopen as fresh ones.\n\n";
                    }
                    Ask += L"Every tab still reopens the next time you run clod.\n\nQuit anyway?";
                    Quit = MessageBoxW(Window, Ask.c_str(), L"clod", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) == IDOK;
                }
                if(Quit) { Tray->Action = TRAY_QUIT; }
            }
        } break;
    }
    return 0;
}

static void TrayCreate(tray_state& Tray, const app_state* App)
{
    Tray.App = App;
    TrayApplyMenuTheme();
    // The exe's own icon (resource 1, drawn by tools/make_icon.cpp), at the sizes the shell asks for.
    HINSTANCE Self = GetModuleHandleW(0);
    Tray.Icon = (HICON)LoadImageW(Self, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    Tray.LargeIcon = (HICON)LoadImageW(Self, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    Tray.TaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW Wc = {};
    Wc.lpfnWndProc = TrayWindowProc;
    Wc.hInstance = GetModuleHandleW(0);
    Wc.lpszClassName = L"clod_tray";
    Wc.hIcon = Tray.LargeIcon;
    RegisterClassW(&Wc);
    // A real (hidden) top-level window: message-only windows miss the TaskbarCreated broadcast.
    Tray.Window = CreateWindowExW(0, L"clod_tray", L"clod", 0, 0, 0, 0, 0, 0, 0, Wc.hInstance, 0);
    SetWindowLongPtrW(Tray.Window, GWLP_USERDATA, (LONG_PTR)&Tray);
    TrayAddIcon(Tray);
}

static void TrayDestroy(tray_state& Tray)
{
    NOTIFYICONDATAW N = TrayIconData(Tray);
    Shell_NotifyIconW(NIM_DELETE, &N);
    DestroyWindow(Tray.Window);
    DestroyIcon(Tray.Icon);
    DestroyIcon(Tray.LargeIcon);
}

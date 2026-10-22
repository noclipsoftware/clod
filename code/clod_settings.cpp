// The user's settings file, %USERPROFILE%\.clod\settings.txt: "key = value" lines, '#' comments.
// Read by the server (prefix) and by clodw (font, font_size, opacity, ...). A missing file or key = the default.

static std::wstring SettingsPath()
{
    wchar_t Home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
    return std::wstring(Home) + L"\\.clod\\settings.txt";
}

// The value for Key, trimmed; empty when absent.
static std::string ReadSetting(const char* Key)
{
    std::string Value;
    FILE* File = 0;
    _wfopen_s(&File, SettingsPath().c_str(), L"rb");
    char Line[512];
    while(File && fgets(Line, sizeof(Line), File))
    {
        std::string S = Line;
        size_t Eq = S.find('=');
        if(S[0] == '#' || Eq == std::string::npos) { continue; }
        std::string K = S.substr(0, Eq);
        while(!K.empty() && strchr(" \t", K.back())) { K.pop_back(); }
        while(!K.empty() && strchr(" \t", K[0])) { K.erase(0, 1); }
        if(K != Key) { continue; }
        Value = S.substr(Eq + 1);
        while(!Value.empty() && strchr(" \t\r\n", Value.back())) { Value.pop_back(); }
        while(!Value.empty() && strchr(" \t", Value[0])) { Value.erase(0, 1); }
    }
    if(File) { fclose(File); }
    return Value;
}

// Sets Key to Value in settings.txt, keeping every other line (clodw's Font..., zoom, Opacity).
static void WriteSetting(const char* Key, const std::string& Value)
{
    std::string Kept;
    FILE* File = 0;
    _wfopen_s(&File, SettingsPath().c_str(), L"rb");
    char Line[512];
    while(File && fgets(Line, sizeof(Line), File))
    {
        std::string K = Line;
        K = K.substr(0, K.find('='));
        while(!K.empty() && strchr(" \t\r\n", K.back())) { K.pop_back(); }
        while(!K.empty() && strchr(" \t", K[0])) { K.erase(0, 1); }
        if(K != Key) { Kept += Line; }
    }
    if(File) { fclose(File); }
    if(!Kept.empty() && Kept.back() != '\n') { Kept += "\r\n"; }
    Kept += std::string(Key) + " = " + Value + "\r\n";
    std::wstring Path = SettingsPath();
    CreateDirectoryW(Path.substr(0, Path.find_last_of(L'\\')).c_str(), 0);
    _wfopen_s(&File, Path.c_str(), L"wb");
    if(File) { fwrite(Kept.data(), 1, Kept.size(), File); fclose(File); }
}

// The folder clod.exe runs from, with a trailing backslash: clink and the default shell setup ship there.
static std::wstring ExeDir()
{
    wchar_t Self[MAX_PATH] = {};
    GetModuleFileNameW(0, Self, MAX_PATH);
    std::wstring Dir = Self;
    return Dir.substr(0, Dir.find_last_of(L'\\') + 1);
}

// %USERPROFILE%\.clod\, with a trailing backslash: the user's own shell setup and clink profile.
static std::wstring UserClodDir()
{
    wchar_t Home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
    return std::wstring(Home) + L"\\.clod\\";
}

// First run (no ~\.clod\init.bat): gives the user their own copy of clod's defaults (shell setup,
// settings.txt). Run by the server at start and by clodw before it reads settings.txt.
// Only missing files are copied, so a user's edits are never overwritten; from then on ~\.clod
// is theirs. Deleting init.bat brings back any default they removed.
static void SeedUserSetup()
{
    std::wstring Defaults = ExeDir() + L"shell\\";
    std::wstring ClodDir = UserClodDir();
    if(GetFileAttributesW((ClodDir + L"init.bat").c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::vector<std::wstring> Dirs = {std::wstring()}; // relative to both roots
        for(size_t K = 0; K < Dirs.size(); ++K)
        {
            CreateDirectoryW((ClodDir + Dirs[K]).c_str(), 0);
            WIN32_FIND_DATAW Found = {};
            HANDLE Find = FindFirstFileW((Defaults + Dirs[K] + L"*").c_str(), &Found);
            if(Find == INVALID_HANDLE_VALUE) { continue; }
            do
            {
                std::wstring Name = Found.cFileName;
                if(Name == L"." || Name == L"..") { continue; }
                if(Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { Dirs.push_back(Dirs[K] + Name + L"\\"); }
                else { CopyFileW((Defaults + Dirs[K] + Name).c_str(), (ClodDir + Dirs[K] + Name).c_str(), TRUE); }
            } while(FindNextFileW(Find, &Found));
            FindClose(Find);
        }
    }
}

// clod's font, "Iosevka Clod" (build\fonts\IosevkaClod-<version>-<style>.ttf, from tools\make_font.py),
// installed for this user so every app can use it: per-user fonts need no admin (Windows 10 1809+).
// A style already registered at this version is left alone, and so is one installed for all users.
// Another version goes in under its own file name (the old file may be in use by some app), and old
// files are deleted once nothing holds them, else on a later start. Run by the server and by clodw,
// before it picks its font.
static void InstallFonts()
{
    wchar_t Local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", Local, MAX_PATH);
    std::wstring Dest = std::wstring(Local) + L"\\Microsoft\\Windows\\Fonts\\";
    std::wstring Bundled = ExeDir() + L"fonts\\";
    const wchar_t* KeyPath = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    static const wchar_t* Values[2] = {L"Iosevka Clod (TrueType)", L"Iosevka Clod Bold (TrueType)"};
    HKEY Key = 0;
    if(RegCreateKeyExW(HKEY_CURRENT_USER, KeyPath, 0, 0, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, 0, &Key, 0) != ERROR_SUCCESS) { return; }
    HKEY Machine = 0;
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyPath, 0, KEY_QUERY_VALUE, &Machine);

    // --- each bundled style: install it unless this version is registered already ---
    bool32 Shipped = false;
    bool32 Added = false;
    WIN32_FIND_DATAW Found = {};
    HANDLE Find = FindFirstFileW((Bundled + L"IosevkaClod-*.ttf").c_str(), &Found);
    for(bool32 More = Find != INVALID_HANDLE_VALUE; More; More = FindNextFileW(Find, &Found))
    {
        Shipped = true;
        std::wstring Name = Found.cFileName;
        bool32 Bold = Name.size() > 9 && _wcsicmp(Name.c_str() + Name.size() - 9, L"-Bold.ttf") == 0;
        const wchar_t* Value = Values[Bold ? 1 : 0];
        wchar_t Old[MAX_PATH] = {};
        DWORD Size = sizeof(Old) - sizeof(wchar_t);
        bool32 Registered = RegQueryValueExW(Key, Value, 0, 0, (BYTE*)Old, &Size) == ERROR_SUCCESS;
        bool32 ForEveryone = Machine && RegQueryValueExW(Machine, Value, 0, 0, 0, 0) == ERROR_SUCCESS;
        std::wstring Dst = Dest + Name;
        bool32 Current = Registered && _wcsicmp(Old, Dst.c_str()) == 0 && GetFileAttributesW(Dst.c_str()) != INVALID_FILE_ATTRIBUTES;
        if(ForEveryone || Current) { continue; }
        CreateDirectoryW(Dest.c_str(), 0);
        CopyFileW((Bundled + Name).c_str(), Dst.c_str(), TRUE); // already there (an earlier try) is fine
        if(GetFileAttributesW(Dst.c_str()) == INVALID_FILE_ATTRIBUTES) { continue; }
        if(Registered && Old[0]) { RemoveFontResourceW(Old); }
        RegSetValueExW(Key, Value, 0, REG_SZ, (const BYTE*)Dst.c_str(), (DWORD)((Dst.size() + 1) * sizeof(wchar_t)));
        AddFontResourceW(Dst.c_str());
        Added = true;
    }
    if(Find != INVALID_HANDLE_VALUE) { FindClose(Find); }

    // --- older versions' files: whatever of ours the registry no longer names (only when this clod
    // ships the font, so a missing build\fonts never removes an installed one) ---
    std::wstring Using[2];
    for(u32 I = 0; I < 2; ++I)
    {
        wchar_t Path[MAX_PATH] = {};
        DWORD Size = sizeof(Path) - sizeof(wchar_t);
        if(RegQueryValueExW(Key, Values[I], 0, 0, (BYTE*)Path, &Size) == ERROR_SUCCESS) { Using[I] = Path; }
    }
    Find = Shipped ? FindFirstFileW((Dest + L"IosevkaClod-*.ttf").c_str(), &Found) : INVALID_HANDLE_VALUE;
    for(bool32 More = Find != INVALID_HANDLE_VALUE; More; More = FindNextFileW(Find, &Found))
    {
        std::wstring Path = Dest + Found.cFileName;
        if(_wcsicmp(Path.c_str(), Using[0].c_str()) != 0 && _wcsicmp(Path.c_str(), Using[1].c_str()) != 0) { DeleteFileW(Path.c_str()); }
    }
    if(Find != INVALID_HANDLE_VALUE) { FindClose(Find); }
    if(Machine) { RegCloseKey(Machine); }
    RegCloseKey(Key);
    if(Added) { PostMessageW(HWND_BROADCAST, WM_FONTCHANGE, 0, 0); } // running apps refresh their font lists
}


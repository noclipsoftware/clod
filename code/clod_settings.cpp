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

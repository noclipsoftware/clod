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

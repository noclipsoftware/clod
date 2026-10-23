// Discovers Claude Code sessions on disk: <claude dir>/projects/<mangled cwd>/<session id>.jsonl
// Only the head and tail of each file are read; transcripts can be tens of megabytes.

struct session_info
{
    std::wstring Path;
    std::string Id;
    std::string Cwd;
    std::string Project; // last component of Cwd
    std::string Title;
    u64 ModifiedTime;    // FILETIME, 100ns ticks since 1601
    u64 Size;
};

static std::wstring Utf8ToWide(const std::string& S)
{
    if(S.empty()) { return std::wstring(); }
    s32 N = MultiByteToWideChar(CP_UTF8, 0, S.data(), (int)S.size(), 0, 0);
    std::wstring W(N, 0);
    MultiByteToWideChar(CP_UTF8, 0, S.data(), (int)S.size(), &W[0], N);
    return W;
}

static std::string WideToUtf8(const std::wstring& W)
{
    if(W.empty()) { return std::string(); }
    s32 N = WideCharToMultiByte(CP_UTF8, 0, W.data(), (int)W.size(), 0, 0, 0, 0);
    std::string S(N, 0);
    WideCharToMultiByte(CP_UTF8, 0, W.data(), (int)W.size(), &S[0], N, 0, 0);
    return S;
}

static std::wstring ClaudeConfigDir()
{
    wchar_t Buf[MAX_PATH] = {};
    if(GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", Buf, MAX_PATH) > 0) { return Buf; }
    GetEnvironmentVariableW(L"USERPROFILE", Buf, MAX_PATH);
    return std::wstring(Buf) + L"\\.claude";
}

// Decodes the JSON string starting just after its opening quote. False if it runs off the buffer.
static bool32 JsonDecodeString(const std::string& Buf, size_t At, std::string& Out)
{
    Out.clear();
    for(size_t I = At; I < Buf.size(); ++I)
    {
        char C = Buf[I];
        if(C == '"') { return true; }
        if(C != '\\') { Out += C; continue; }
        if(++I >= Buf.size()) { return false; }
        switch(Buf[I])
        {
            case 'n': Out += '\n'; break;
            case 't': Out += '\t'; break;
            case 'r': Out += '\r'; break;
            case 'b': case 'f': break;
            case 'u':
            {
                if(I + 4 >= Buf.size()) { return false; }
                u32 Cp = (u32)strtoul(Buf.substr(I + 1, 4).c_str(), 0, 16);
                I += 4;
                if(Cp >= 0xD800 && Cp <= 0xDBFF && I + 6 < Buf.size() && Buf[I + 1] == '\\' && Buf[I + 2] == 'u')
                {
                    u32 Lo = (u32)strtoul(Buf.substr(I + 3, 4).c_str(), 0, 16);
                    Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Lo - 0xDC00);
                    I += 6;
                }
                AppendUtf8(Out, Cp);
            } break;
            default: Out += Buf[I]; break;
        }
    }
    return false;
}

// Finds "Key":"value" (first or last occurrence) and decodes the value.
static bool32 JsonFindString(const std::string& Buf, const char* Key, bool32 Last, std::string& Out)
{
    std::string Pattern = std::string("\"") + Key + "\":\"";
    size_t At = Last ? Buf.rfind(Pattern) : Buf.find(Pattern);
    while(At != std::string::npos)
    {
        if(JsonDecodeString(Buf, At + Pattern.size(), Out) && !Out.empty()) { return true; }
        if(Last)
        {
            if(At == 0) { break; }
            At = Buf.rfind(Pattern, At - 1);
        }
        else
        {
            At = Buf.find(Pattern, At + 1);
        }
    }
    return false;
}

static std::string ReadFileRange(HANDLE File, u64 Offset, u32 Size)
{
    std::string Out(Size, 0);
    LARGE_INTEGER Pos = {};
    Pos.QuadPart = (LONGLONG)Offset;
    SetFilePointerEx(File, Pos, 0, FILE_BEGIN);
    DWORD Read = 0;
    ReadFile(File, &Out[0], Size, &Read, 0);
    Out.resize(Read);
    return Out;
}

static std::string OneLine(const std::string& S)
{
    std::string Out;
    bool32 Space = false;
    for(char C : S)
    {
        bool32 IsSpace = (C == '\n' || C == '\r' || C == '\t' || C == ' ');
        if(IsSpace) { Space = !Out.empty(); continue; }
        if(Space) { Out += ' '; Space = false; }
        Out += C;
    }
    return Out;
}

static bool32 ReadSessionInfo(const std::wstring& Path, session_info& Out)
{
    HANDLE File = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return false; }

    LARGE_INTEGER Size = {};
    GetFileSizeEx(File, &Size);
    FILETIME Mod = {};
    GetFileTime(File, 0, 0, &Mod);

    const u32 HeadSize = 64 * 1024;
    const u32 TailSize = 256 * 1024;
    u64 FileSize = (u64)Size.QuadPart;
    std::string Head = ReadFileRange(File, 0, (u32)(FileSize < HeadSize ? FileSize : HeadSize));
    std::string Tail;
    if(FileSize > HeadSize)
    {
        u64 TailStart = FileSize > TailSize ? FileSize - TailSize : 0;
        Tail = ReadFileRange(File, TailStart, (u32)(FileSize - TailStart));
    }
    CloseHandle(File);
    const std::string& Last = Tail.empty() ? Head : Tail;

    Out.Path = Path;
    Out.Size = FileSize;
    Out.ModifiedTime = ((u64)Mod.dwHighDateTime << 32) | Mod.dwLowDateTime;
    size_t Slash = Path.find_last_of(L"\\/");
    std::wstring Name = Path.substr(Slash + 1);
    Out.Id = WideToUtf8(Name.substr(0, Name.size() - 6)); // strip ".jsonl"

    // --- cwd: sessions can wander between directories; resume needs the one the project folder is named after ---
    std::string ProjectDir = WideToUtf8(Path.substr(0, Slash));
    ProjectDir = ProjectDir.substr(ProjectDir.find_last_of("\\/") + 1);
    // Returns true once Buf holds the matching cwd; the first cwd seen at all is the fallback.
    auto FindProjectCwd = [&](const std::string& Buf) -> bool32
    {
        size_t At = Buf.find("\"cwd\":\"");
        while(At != std::string::npos)
        {
            std::string Cwd;
            if(JsonDecodeString(Buf, At + 7, Cwd) && !Cwd.empty())
            {
                if(Out.Cwd.empty()) { Out.Cwd = Cwd; }
                std::string Mangled = Cwd;
                for(char& C : Mangled) { if(!isalnum((u8)C)) { C = '-'; } }
                if(Mangled == ProjectDir) { Out.Cwd = Cwd; return true; }
            }
            At = Buf.find("\"cwd\":\"", At + 1);
        }
        return false;
    };
    if(!FindProjectCwd(Head) && !FindProjectCwd(Tail) && FileSize > HeadSize)
    {
        // Long sessions can have no cwd in the head and only later directories in the tail, so walk
        // the middle until the original one turns up. Chunks overlap so a match can't straddle them.
        HANDLE Again = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
        if(Again != INVALID_HANDLE_VALUE)
        {
            const u32 Chunk = 1024 * 1024;
            for(u64 At = HeadSize - 4096; At < FileSize; At += Chunk - 4096)
            {
                if(FindProjectCwd(ReadFileRange(Again, At, Chunk))) { break; }
            }
            CloseHandle(Again);
        }
    }
    size_t CwdSlash = Out.Cwd.find_last_of("\\/");
    Out.Project = (CwdSlash == std::string::npos) ? Out.Cwd : Out.Cwd.substr(CwdSlash + 1);

    // --- title: custom > ai-generated > last prompt > first real user prompt ---
    std::string Title;
    if(JsonFindString(Last, "customTitle", true, Title) || JsonFindString(Head, "customTitle", true, Title) ||
       JsonFindString(Last, "aiTitle", true, Title) || JsonFindString(Head, "aiTitle", true, Title) ||
       JsonFindString(Last, "lastPrompt", true, Title))
    {
        Out.Title = OneLine(Title);
    }
    else
    {
        size_t LineStart = 0;
        while(LineStart < Head.size() && Out.Title.empty())
        {
            size_t LineEnd = Head.find('\n', LineStart);
            if(LineEnd == std::string::npos) { LineEnd = Head.size(); }
            std::string Line = Head.substr(LineStart, LineEnd - LineStart);
            LineStart = LineEnd + 1;
            if(Line.find("\"type\":\"user\"") == std::string::npos || Line.find("\"isMeta\":true") != std::string::npos) { continue; }
            std::string Text;
            if(JsonFindString(Line, "content", false, Text) || JsonFindString(Line, "text", false, Text))
            {
                if(Text[0] != '<') { Out.Title = OneLine(Text); }
            }
        }
    }
    return true;
}

// Every Claude Code session with a prompt, newest first. A transcript unchanged since Known read it
// (same path, write time and size) is taken from Known instead of being read again.
static std::vector<session_info> ScanSessions(const std::vector<session_info>& Known)
{
    std::map<std::wstring, const session_info*> Cache;
    for(const session_info& S : Known) { Cache[S.Path] = &S; }
    std::vector<session_info> Result;
    std::wstring Projects = ClaudeConfigDir() + L"\\projects";

    WIN32_FIND_DATAW Dir = {};
    HANDLE DirFind = FindFirstFileW((Projects + L"\\*").c_str(), &Dir);
    if(DirFind == INVALID_HANDLE_VALUE) { return Result; }
    do
    {
        if(!(Dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Dir.cFileName[0] == L'.') { continue; }
        std::wstring ProjectDir = Projects + L"\\" + Dir.cFileName;

        WIN32_FIND_DATAW File = {};
        HANDLE FileFind = FindFirstFileW((ProjectDir + L"\\*.jsonl").c_str(), &File);
        if(FileFind == INVALID_HANDLE_VALUE) { continue; }
        do
        {
            std::wstring Path = ProjectDir + L"\\" + File.cFileName;
            u64 Modified = ((u64)File.ftLastWriteTime.dwHighDateTime << 32) | File.ftLastWriteTime.dwLowDateTime;
            u64 Size = ((u64)File.nFileSizeHigh << 32) | File.nFileSizeLow;
            auto Hit = Cache.find(Path);
            if(Hit != Cache.end() && Hit->second->ModifiedTime == Modified && Hit->second->Size == Size)
            {
                Result.push_back(*Hit->second);
                continue;
            }
            session_info Info = {};
            if(ReadSessionInfo(Path, Info))
            {
                // No prompt means nothing to resume (a session opened and quit before typing anything).
                if(!Info.Title.empty()) { Result.push_back(Info); }
            }
        } while(FindNextFileW(FileFind, &File));
        FindClose(FileFind);
    } while(FindNextFileW(DirFind, &Dir));
    FindClose(DirFind);

    std::sort(Result.begin(), Result.end(), [](const session_info& A, const session_info& B) { return A.ModifiedTime > B.ModifiedTime; });
    return Result;
}

static bool32 SessionFileExists(const std::string& Id)
{
    std::wstring Projects = ClaudeConfigDir() + L"\\projects";
    std::wstring Want = Utf8ToWide(Id) + L".jsonl";
    WIN32_FIND_DATAW Dir = {};
    HANDLE DirFind = FindFirstFileW((Projects + L"\\*").c_str(), &Dir);
    if(DirFind == INVALID_HANDLE_VALUE) { return false; }
    bool32 Found = false;
    do
    {
        if(!(Dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Dir.cFileName[0] == L'.') { continue; }
        std::wstring Path = Projects + L"\\" + Dir.cFileName + L"\\" + Want;
        if(GetFileAttributesW(Path.c_str()) != INVALID_FILE_ATTRIBUTES) { Found = true; }
    } while(!Found && FindNextFileW(DirFind, &Dir));
    FindClose(DirFind);
    return Found;
}

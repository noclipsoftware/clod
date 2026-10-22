// Client/server transport: framed messages over a per-user named pipe.
// Frame = u32 Type, u32 Size, Size bytes of payload. Byte-mode pipe, overlapped IO on both ends.

enum msg_type
{
    MSG_NONE = 0,
    MSG_ATTACH, // client -> server: msg_attach, then the client's cwd (UTF-8)
    MSG_INPUT,  // client -> server: VT input bytes
    MSG_RESIZE, // client -> server: s32 Width, s32 Height
    MSG_OUTPUT, // server -> client: VT bytes for the console
    MSG_BYE,    // server -> client: reason text; the client restores its console and exits
    MSG_WIDTHS, // client -> server: sorted u32 codepoints the client's console draws 2 cells wide where CharWidth says 1
};

struct msg_attach
{
    s32 Width;
    s32 Height;
    s32 JumpToTab;      // tab index + 1; 0 = stay on the current tab
    u64 ConsoleWindow;  // client's HWND, so the tray can bring it forward
};

struct ipc_conn
{
    HANDLE Pipe;
    HANDLE ReadEvent;
    HANDLE WriteEvent;
    OVERLAPPED ReadOv;
    bool32 ReadPending;
    std::string Inbox; // received bytes not yet split into messages
    char ReadBuf[64 * 1024];
};

static std::wstring CurrentUserSid()
{
    std::wstring Result;
    HANDLE Token = 0;
    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token)) { return Result; }
    u8 Buf[256] = {};
    DWORD Size = 0;
    if(GetTokenInformation(Token, TokenUser, Buf, sizeof(Buf), &Size))
    {
        wchar_t* Sid = 0;
        if(ConvertSidToStringSidW(((TOKEN_USER*)Buf)->User.Sid, &Sid))
        {
            Result = Sid;
            LocalFree(Sid);
        }
    }
    CloseHandle(Token);
    return Result;
}

// CLOD_INSTANCE=<name> runs a separate server (own pipe, own tabs file), e.g. for tests.
static std::wstring InstanceSuffix()
{
    wchar_t Name[64] = {};
    if(GetEnvironmentVariableW(L"CLOD_INSTANCE", Name, ArrayCount(Name))) { return std::wstring(L"-") + Name; }
    return std::wstring();
}

static std::wstring PipeName()
{
    return L"\\\\.\\pipe\\clod-" + CurrentUserSid() + InstanceSuffix();
}

// Server side: a new listening pipe instance that only the current user can open.
static HANDLE IpcCreatePipe(bool32 First)
{
    // Default pipe ACLs grant Everyone read access, which would expose terminal contents.
    std::wstring Sddl = L"D:P(A;;GA;;;" + CurrentUserSid() + L")";
    PSECURITY_DESCRIPTOR Sd = 0;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl.c_str(), SDDL_REVISION_1, &Sd, 0);
    SECURITY_ATTRIBUTES Sa = {};
    Sa.nLength = sizeof(Sa);
    Sa.lpSecurityDescriptor = Sd;
    DWORD Open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (First ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    HANDLE Pipe = CreateNamedPipeW(PipeName().c_str(), Open, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES, 256 * 1024, 64 * 1024, 0, Sd ? &Sa : 0);
    if(Sd) { LocalFree(Sd); }
    return Pipe;
}

static void IpcInit(ipc_conn& C, HANDLE Pipe)
{
    C.Pipe = Pipe;
    C.ReadEvent = CreateEventW(0, TRUE, FALSE, 0);
    C.WriteEvent = CreateEventW(0, TRUE, FALSE, 0);
    C.ReadPending = false;
    C.Inbox.clear();
}

static void IpcClose(ipc_conn& C)
{
    if(C.Pipe)
    {
        CancelIo(C.Pipe);
        if(C.ReadPending)
        {
            DWORD Ignored = 0;
            GetOverlappedResult(C.Pipe, &C.ReadOv, &Ignored, TRUE);
        }
        CloseHandle(C.Pipe);
    }
    if(C.ReadEvent) { CloseHandle(C.ReadEvent); }
    if(C.WriteEvent) { CloseHandle(C.WriteEvent); }
    C.Pipe = 0;
    C.ReadEvent = 0;
    C.WriteEvent = 0;
    C.ReadPending = false;
    C.Inbox.clear();
}

static bool32 IpcSend(ipc_conn& C, u32 Type, const void* Data, u32 Size)
{
    if(!C.Pipe) { return false; }
    std::string Frame;
    Frame.resize(8 + Size);
    memcpy(&Frame[0], &Type, 4);
    memcpy(&Frame[4], &Size, 4);
    if(Size) { memcpy(&Frame[8], Data, Size); }

    size_t At = 0;
    while(At < Frame.size())
    {
        OVERLAPPED Ov = {};
        Ov.hEvent = C.WriteEvent;
        ResetEvent(C.WriteEvent);
        DWORD Written = 0;
        if(!WriteFile(C.Pipe, Frame.data() + At, (DWORD)(Frame.size() - At), 0, &Ov) && GetLastError() != ERROR_IO_PENDING) { return false; }
        if(!GetOverlappedResult(C.Pipe, &Ov, &Written, TRUE) || Written == 0) { return false; }
        At += Written;
    }
    return true;
}

static bool32 IpcSendString(ipc_conn& C, u32 Type, const std::string& S)
{
    return IpcSend(C, Type, S.data(), (u32)S.size());
}

// Keeps one overlapped read outstanding. Call after IpcInit and whenever ReadEvent fires.
// Returns false once the other end has gone away.
static bool32 IpcPump(ipc_conn& C)
{
    if(!C.Pipe) { return false; }
    if(C.ReadPending)
    {
        if(WaitForSingleObject(C.ReadEvent, 0) != WAIT_OBJECT_0) { return true; }
        DWORD Read = 0;
        C.ReadPending = false;
        if(!GetOverlappedResult(C.Pipe, &C.ReadOv, &Read, FALSE)) { return false; }
        C.Inbox.append(C.ReadBuf, Read);
    }
    for(;;)
    {
        C.ReadOv = OVERLAPPED{};
        C.ReadOv.hEvent = C.ReadEvent;
        ResetEvent(C.ReadEvent);
        DWORD Read = 0;
        if(ReadFile(C.Pipe, C.ReadBuf, sizeof(C.ReadBuf), &Read, &C.ReadOv))
        {
            // Completed synchronously; collect it and keep going.
            GetOverlappedResult(C.Pipe, &C.ReadOv, &Read, FALSE);
            C.Inbox.append(C.ReadBuf, Read);
            continue;
        }
        if(GetLastError() != ERROR_IO_PENDING) { return false; }
        C.ReadPending = true;
        return true;
    }
}

static bool32 IpcNextMessage(ipc_conn& C, u32& Type, std::string& Payload)
{
    if(C.Inbox.size() < 8) { return false; }
    u32 Size = 0;
    memcpy(&Type, C.Inbox.data(), 4);
    memcpy(&Size, C.Inbox.data() + 4, 4);
    if(C.Inbox.size() < 8 + (size_t)Size) { return false; }
    Payload.assign(C.Inbox, 8, Size);
    C.Inbox.erase(0, 8 + (size_t)Size);
    return true;
}

// ConPTY child process with a reader thread that buffers its output.

struct pty
{
    HPCON Console;
    HANDLE InputWrite;
    HANDLE OutputRead;
    HANDLE Process;
    HANDLE ReaderThread;
    HANDLE Wake; // shared with the main loop, not owned
    CRITICAL_SECTION Lock;
    std::string Pending; // read by the reader thread, not yet fed to the emulator
};

static DWORD WINAPI PtyReaderThread(void* Param)
{
    pty* P = (pty*)Param;
    static thread_local char Buf[64 * 1024];
    for(;;)
    {
        DWORD Read = 0;
        if(!ReadFile(P->OutputRead, Buf, sizeof(Buf), &Read, 0) || Read == 0) { break; }
        LatencyMark("pty_read");
        EnterCriticalSection(&P->Lock);
        P->Pending.append(Buf, Read);
        LeaveCriticalSection(&P->Lock);
        SetEvent(P->Wake);
    }
    SetEvent(P->Wake);
    return 0;
}

// Starts a process in a new pseudo console. Returns null and fills Error on failure.
// Env is a CreateProcess environment block (UTF-16, double-null terminated) or null to inherit ours.
static pty* PtyStart(const std::wstring& CommandLine, const std::wstring& Cwd, const wchar_t* Env, s32 Cols, s32 Rows, HANDLE Job, HANDLE Wake, std::string& Error)
{
    HANDLE InRead = 0, InWrite = 0, OutRead = 0, OutWrite = 0;
    if(!CreatePipe(&InRead, &InWrite, 0, 0) || !CreatePipe(&OutRead, &OutWrite, 0, 0))
    {
        Error = "CreatePipe failed";
        return 0;
    }

    HPCON Console = 0;
    COORD Size = {(SHORT)Max(Cols, 1), (SHORT)Max(Rows, 1)};
    HRESULT Hr = CreatePseudoConsole(Size, InRead, OutWrite, 0, &Console);
    CloseHandle(InRead);
    CloseHandle(OutWrite);
    if(FAILED(Hr))
    {
        CloseHandle(InWrite);
        CloseHandle(OutRead);
        Error = "CreatePseudoConsole failed (needs Windows 10 1809+)";
        return 0;
    }

    SIZE_T AttrSize = 0;
    InitializeProcThreadAttributeList(0, 1, 0, &AttrSize);
    std::vector<u8> AttrBuf(AttrSize);
    STARTUPINFOEXW Si = {};
    Si.StartupInfo.cb = sizeof(Si);
    // Keep our own std handles from leaking into the child; it must talk only to the pseudo console.
    Si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    Si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)AttrBuf.data();
    InitializeProcThreadAttributeList(Si.lpAttributeList, 1, 0, &AttrSize);
    UpdateProcThreadAttribute(Si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, Console, sizeof(Console), 0, 0);

    std::wstring Cmd = CommandLine; // CreateProcessW may modify its command line buffer
    PROCESS_INFORMATION Pi = {};
    BOOL Ok = CreateProcessW(0, &Cmd[0], 0, 0, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | ABOVE_NORMAL_PRIORITY_CLASS, (void*)Env,
                             Cwd.empty() ? 0 : Cwd.c_str(), &Si.StartupInfo, &Pi);
    DeleteProcThreadAttributeList(Si.lpAttributeList);
    if(Ok) { RaiseForTyping(Pi.hProcess); }
    if(!Ok)
    {
        char Buf[64];
        snprintf(Buf, sizeof(Buf), "CreateProcess failed (error %lu)", GetLastError());
        Error = Buf;
        ClosePseudoConsole(Console);
        CloseHandle(InWrite);
        CloseHandle(OutRead);
        return 0;
    }
    if(Job) { AssignProcessToJobObject(Job, Pi.hProcess); }
    CloseHandle(Pi.hThread);

    pty* P = new pty();
    P->Console = Console;
    P->InputWrite = InWrite;
    P->OutputRead = OutRead;
    P->Process = Pi.hProcess;
    P->Wake = Wake;
    InitializeCriticalSection(&P->Lock);
    P->ReaderThread = CreateThread(0, 0, PtyReaderThread, P, 0, 0);
    return P;
}

static void PtyClose(pty* P)
{
    if(!P) { return; }
    // Closing the pseudo console hangs up the child; the reader keeps draining so this can't stall.
    if(P->Console) { ClosePseudoConsole(P->Console); }
    if(WaitForSingleObject(P->Process, 1500) == WAIT_TIMEOUT) { TerminateProcess(P->Process, 1); }
    CloseHandle(P->InputWrite);
    WaitForSingleObject(P->ReaderThread, 2000);
    CloseHandle(P->ReaderThread);
    CloseHandle(P->OutputRead);
    CloseHandle(P->Process);
    DeleteCriticalSection(&P->Lock);
    delete P;
}

static void PtyWrite(pty* P, const std::string& Bytes)
{
    if(!P || Bytes.empty()) { return; }
    DWORD Written = 0;
    WriteFile(P->InputWrite, Bytes.data(), (DWORD)Bytes.size(), &Written, 0);
}

// Every process on the machine, with its parent: what a tab's cmd is running lives below it.
static std::vector<PROCESSENTRY32W> ListProcesses()
{
    std::vector<PROCESSENTRY32W> Out;
    HANDLE Snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(Snap == INVALID_HANDLE_VALUE) { return Out; }
    PROCESSENTRY32W E = {};
    E.dwSize = sizeof(E);
    for(BOOL More = Process32FirstW(Snap, &E); More; More = Process32NextW(Snap, &E)) { Out.push_back(E); }
    CloseHandle(Snap);
    return Out;
}

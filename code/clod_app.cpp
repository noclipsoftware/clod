// App core: tabs, the home screen, input handling and frame composition.
// No OS calls here. Input handling emits app_commands for the platform layer to execute;
// composition turns app state into a cell grid; rendering diffs two grids into VT bytes.

struct pty; // platform-owned


struct tab
{
    vt_terminal Term;
    // Every tab runs cmd. It is a claude tab while claude runs inside that cmd: SessionId is then
    // that claude's session (empty = plain shell).
    u32 ClaudePid;          // the claude found running in this tab's cmd; 0 = none seen (yet)
    u64 LaunchedAtMs;       // the server started claude here and hasn't seen it yet; 0 = not waiting
    std::string SessionId;
    std::string Cwd;
    std::string Project;
    std::string Title;      // from the session file; used when the child sets no title
    s32 ScrollOffset;       // lines scrolled back from live output; 0 = following
    u64 SeenPushed;         // Term.ScrollbackPushed as of the last scroll fix-up
    bool32 Activity;        // new lines arrived while no pane showed it
    u64 SeenLinesPushed;    // Term.ScrollbackPushed and CursorY as last seen on screen (or just after a
    s32 SeenCursorY;        // resize): output that moves neither, like a prompt redrawn in place, isn't new
    u64 RepaintUntilMs;     // ConPTY's repaint after a resize is expected until then, and isn't new either
    std::string Notice;     // why it wants a look ("finished", "needs you", ...), set while no pane showed it
    bool32 Exited;          // cmd ended, or never started
    bool32 TitleBusy;       // claude's title shows its spinner (a turn is running)
    bool32 NeedsInput;      // claude asked for the user (bell, OSC 9) and nothing has been typed into it since
    u64 NotifiedAtMs;       // last tray notification about this tab
    std::string HistoryId;  // names the file a shell tab's recent output is saved to (HistoryText)
    std::string HistorySaved; // what that file holds, to skip rewriting it unchanged
    bool32 ReplayHistory;   // restored after a restart: cmd types the saved output before its prompt
    // --- an image `clod view` asked for, shown over the tab until a key or a click closes it ---
    std::string ViewImage;        // its path (UTF-8); empty = none
    std::vector<u32> ViewPixels;  // a small premultiplied BGRA copy for the half-block preview
    u32 ViewW;
    u32 ViewH;
    u32 ViewFullW;                // the file's own size, for the caption
    u32 ViewFullH;
    u32 ViewIndex;                // its place among its folder's images (arrows step through them)
    u32 ViewCount;
    pty* Pty;
};

// Rows a tab's child gets: the window minus the top bar and the command bar (bottom).
static s32 ContentRows(s32 Height) { return Max(1, Height - 2); }

enum app_prompt
{
    PROMPT_NONE = 0,
    PROMPT_CLOSE_TAB,
    PROMPT_NEW_FOLDER, // command bar edits PathInput; Enter starts a new session there
};

// A clickable span of the top bar: a pane's name (focuses it), or Pane -1 (the counts: home).
struct bar_hit
{
    s32 X0;
    s32 X1;
    s32 Pane;
};

// Panes split the area between the bars; each shows one tab (no tab in two panes) or the
// home screen.
enum app_layout
{
    LAYOUT_SINGLE = 0,
    LAYOUT_SIDE,  // two columns
    LAYOUT_GRID,  // 2x2
    LAYOUT_MAIN,  // one large pane on the left, two stacked on the right
};

#define MAX_PANES 4

struct pane_rect
{
    s32 X;
    s32 Y;
    s32 W;
    s32 H;
};

// The layout last used on a monitor ("\\.\DISPLAY1 3840x2160"), so each screen keeps its own.
struct monitor_layout
{
    std::string Monitor;
    s32 Layout;
};

// A page: a layout and what its panes show, switched as a whole (like tmux's windows).
struct page
{
    s32 Layout;
    s32 Focus;
    tab* PaneTab[MAX_PANES];
};

struct app_state
{
    s32 Width;
    s32 Height;
    std::vector<tab*> Tabs;
    s32 Active; // what the focused pane shows: 0 = home screen, N = Tabs[N - 1]

    s32 Layout;                // LAYOUT_*
    s32 Focus;                 // the pane that gets the keys
    tab* PaneTab[MAX_PANES];   // what each pane shows; null = the home screen
    // The current page lives in Layout/Focus/PaneTab; Pages holds every page, the current one's entry
    // refreshed when leaving it. Empty = just the one page. A tab is in at most one pane of all pages.
    std::vector<page> Pages;
    s32 Page;                  // the current page's index in Pages
    s32 DragPane;              // pane + 1 a mouse button went down in (the drag stays there); 0 = none
    std::string Monitor;       // the attached window's monitor, as the client reports it
    std::vector<monitor_layout> MonitorLayouts;

    std::vector<session_info> Sessions;
    std::string Filter;
    bool32 FilterEditing;
    s32 Selected; // home screen: 0..Tabs-1 = a running tab, then the filtered sessions
    s32 ListScroll;

    u32 PrefixKey;      // the key that goes home / back (settings "prefix"; default Alt+\): a control byte, or PREFIX_ALT | a character
    u64 CycleShownUntil; // the tab switcher box shows until then (ms; Ctrl+Tab, Ctrl+PgUp/PgDn); 0 = not shown
    tab* CameFrom;      // the tab the focused pane showed before the prefix brought it home: Esc goes back
    s32 Prompt;
    tab* PromptTab;     // the tab PROMPT_CLOSE_TAB asks about
    std::string PathInput;
    bool32 MouseCaptureOff;
    bool32 WindowUnfocused;   // the client window lost the keyboard focus (focus report ESC [ O): no accent edge
    bool32 WindowDrawsImages; // the attached client (clodw) draws images itself: panes leave them blank
    bool32 InPaste;
    std::string PasteBuf;
    std::string Status;
    u64 StatusUntil; // GetTickCount64 ms
    std::string LaunchCwd;
    std::string Clock;  // local date and time for the command bar's left ("Thu Oct 23  2:05 PM"), set by the server
    std::vector<bar_hit> BarHits; // from the last composed frame
    bool32 Quit; // set by the server (tray Quit): stop, reopening the tabs with --resume next launch
};

enum command_kind
{
    CMD_NONE = 0,
    CMD_WRITE,         // Bytes to Tab
    CMD_OPEN,          // SessionId/Cwd/Project/Title; empty SessionId = brand new session
    CMD_OPEN_SHELL,    // cmd.exe tab in Cwd
    CMD_RESTART,       // Tab
    CMD_CLOSE,         // Tab
    CMD_RESCAN,
    CMD_DETACH,        // drop the attached window; tabs keep running
    CMD_OPEN_FOLDER,   // new session in typed path Cwd, if it is a folder (closes the prompt)
    CMD_COMPLETE_FOLDER, // complete the last folder name in App.PathInput
    CMD_VIEW_STEP,     // Tab's image: show the one Step images on in its folder
};

struct app_command
{
    s32 Kind;
    tab* Tab;
    s32 Step;
    std::string Bytes;
    std::string SessionId;
    std::string Cwd;
    std::string Project;
    std::string Title;
};

// --- the prefix key: a control byte ("Ctrl+]"), or PREFIX_ALT | a character ("Alt+\", the default) ---

#define PREFIX_ALT 0x100u

// The key for a setting like "alt+\", "ctrl+]" or "Ctrl+Space"; -1 when it isn't one clod can
// use (Ctrl+H, I, M and [ arrive as Backspace, Tab, Enter and Esc).
static s32 PrefixFromName(const std::string& Name)
{
    std::string S;
    for(char C : Name) { if(C != ' ') { S += (C >= 'A' && C <= 'Z') ? (char)(C - 'A' + 'a') : C; } }
    // Alt+<printable ASCII>: PREFIX_ALT | the character (arrives as ESC + it).
    if(S.compare(0, 4, "alt+") == 0 && S.size() == 5 && S[4] > ' ' && S[4] < 0x7F) { return (s32)(PREFIX_ALT | (u8)S[4]); }
    if(S.compare(0, 5, "ctrl+") != 0) { return -1; }
    S = S.substr(5);
    s32 Key = -1;
    if(S == "space" || S == "@") { Key = 0; }
    else if(S.size() == 1 && S[0] >= 'a' && S[0] <= 'z') { Key = S[0] - 'a' + 1; }
    else if(S.size() == 1 && strchr("\\]^_", S[0])) { Key = (S[0] == '\\') ? 0x1C : (S[0] == ']' ? 0x1D : (S[0] == '^' ? 0x1E : 0x1F)); }
    if(Key == 0x08 || Key == 0x09 || Key == 0x0D) { return -1; }
    return Key;
}

static std::string PrefixName(u32 Key)
{
    if(Key & PREFIX_ALT) { return std::string("Alt+") + (char)(Key & 0xFF); }
    if(Key == 0) { return "Ctrl+Space"; }
    if(Key <= 26) { return std::string("Ctrl+") + (char)('A' + Key - 1); }
    static const char* Punct[] = {"Ctrl+\\", "Ctrl+]", "Ctrl+^", "Ctrl+_"};
    return (Key >= 0x1C && Key <= 0x1F) ? Punct[Key - 0x1C] : "Ctrl+?";
}

static tab* ActiveTab(const app_state& App)
{
    return App.Active > 0 && App.Active <= (s32)App.Tabs.size() ? App.Tabs[App.Active - 1] : 0;
}

// --- panes ---

static s32 PaneCount(s32 Layout)
{
    return Layout == LAYOUT_SIDE ? 2 : (Layout == LAYOUT_GRID ? 4 : (Layout == LAYOUT_MAIN ? 3 : 1));
}

// Pane P's cells: the area between the bars, split in halves with one-cell separator lines.
// Grid panes go left to right, top to bottom; Main's are left, top right, bottom right.
static pane_rect PaneRect(const app_state& App, s32 Pane)
{
    s32 W = Max(App.Width, 1);
    s32 H = ContentRows(App.Height);
    pane_rect R = {0, 1, W, H};
    if(App.Layout == LAYOUT_SINGLE) { return R; }
    s32 LeftW = Max(1, (W - 1) / 2);
    s32 TopH = Max(1, (H - 1) / 2);
    bool32 Right = (App.Layout == LAYOUT_GRID) ? (Pane & 1) : Pane >= 1;
    bool32 Halved = (App.Layout == LAYOUT_GRID) || (App.Layout == LAYOUT_MAIN && Pane >= 1);
    bool32 Bottom = (App.Layout == LAYOUT_GRID) ? Pane >= 2 : Pane == 2;
    R.X = Right ? LeftW + 1 : 0;
    R.W = Right ? Max(1, W - LeftW - 1) : LeftW;
    if(Halved)
    {
        R.Y = Bottom ? 1 + TopH + 1 : 1;
        R.H = Bottom ? Max(1, H - TopH - 1) : TopH;
    }
    return R;
}

static s32 TabIndex(const app_state& App, const tab* T)
{
    for(s32 I = 0; I < (s32)App.Tabs.size(); ++I) { if(App.Tabs[I] == T) { return I + 1; } }
    return 0;
}

// The pane showing T, or -1.
static s32 PaneShowing(const app_state& App, const tab* T)
{
    for(s32 P = 0; P < PaneCount(App.Layout); ++P) { if(T && App.PaneTab[P] == T) { return P; } }
    return -1;
}

// The other page with a pane showing T, or -1.
static s32 OtherPageShowing(const app_state& App, const tab* T)
{
    for(s32 G = 0; G < (s32)App.Pages.size(); ++G)
    {
        if(G == App.Page) { continue; }
        for(s32 P = 0; P < PaneCount(App.Pages[G].Layout); ++P) { if(T && App.Pages[G].PaneTab[P] == T) { return G; } }
    }
    return -1;
}

// The first tab no pane of any page shows, looking from tab position From (0-based) onward and
// wrapping; Skip is never picked. Null when every tab is on screen somewhere.
static tab* UnshownTab(const app_state& App, s32 From, const tab* Skip)
{
    s32 N = (s32)App.Tabs.size();
    for(s32 K = 0; K < N; ++K)
    {
        tab* T = App.Tabs[(From + K) % N];
        if(T != Skip && PaneShowing(App, T) < 0 && OtherPageShowing(App, T) < 0) { return T; }
    }
    return 0;
}

static void FocusPane(app_state& App, s32 Pane)
{
    App.Focus = Clamp(Pane, 0, PaneCount(App.Layout) - 1);
    App.Active = TabIndex(App, App.PaneTab[App.Focus]);
    if(App.PaneTab[App.Focus]) { App.PaneTab[App.Focus]->Activity = false; App.PaneTab[App.Focus]->Notice.clear(); }
}

// Shows tab Index (0 = home screen) in the focused pane. A tab another pane shows swaps places:
// that pane gets what this one had. A tab on another page moves here; its pane there goes home.
static void SwitchTab(app_state& App, s32 Index)
{
    if(Index < 0 || Index > (s32)App.Tabs.size()) { return; }
    tab* T = (Index > 0) ? App.Tabs[Index - 1] : 0;
    for(page& G : App.Pages)
    {
        for(tab*& P : G.PaneTab) { if(T && P == T && &G != &App.Pages[App.Page]) { P = 0; } }
    }
    s32 Shown = PaneShowing(App, T);
    if(Shown >= 0) { App.PaneTab[Shown] = App.PaneTab[App.Focus]; }
    App.PaneTab[App.Focus] = T;
    App.Active = Index;
    if(T) { T->Activity = false; T->Notice.clear(); }
}

// Next (+1) or previous (-1) tab in the focused pane, skipping tabs other panes (of any page) show
// and the home screen (the prefix goes there): cycling never pulls a tab out of another pane or
// moves the focus.
static void CycleTab(app_state& App, s32 Step)
{
    s32 Count = (s32)App.Tabs.size();
    if(Count == 0) { return; }
    s32 From = Max(App.Active, 1) - 1 + (App.Active == 0 && Step > 0 ? -1 : 0); // from home: the first or last tab
    for(s32 I = 1; I <= Count; ++I)
    {
        s32 Index = ((From + Step * I) % Count + Count) % Count;
        s32 Shown = PaneShowing(App, App.Tabs[Index]);
        if((Shown < 0 || Shown == App.Focus) && OtherPageShowing(App, App.Tabs[Index]) < 0) { SwitchTab(App, Index + 1); return; }
    }
}

// Changes the layout. The focused tab stays on screen; new panes take tabs nothing shows yet (or
// the home screen); the monitor remembers the choice.
static void SetLayout(app_state& App, s32 Layout)
{
    s32 Old = PaneCount(App.Layout);
    s32 New = PaneCount(Layout);
    if(App.Focus >= New)
    {
        App.PaneTab[New - 1] = App.PaneTab[App.Focus];
        App.Focus = New - 1;
    }
    for(s32 P = New; P < MAX_PANES; ++P) { App.PaneTab[P] = 0; }
    App.Layout = Layout;
    for(s32 P = Old; P < New; ++P) { App.PaneTab[P] = UnshownTab(App, 0, 0); }
    App.Active = TabIndex(App, App.PaneTab[App.Focus]);

    bool32 Known = false;
    for(monitor_layout& M : App.MonitorLayouts) { if(M.Monitor == App.Monitor) { M.Layout = Layout; Known = true; } }
    if(!Known && !App.Monitor.empty()) { App.MonitorLayouts.push_back(monitor_layout{App.Monitor, Layout}); }
}

// Before tab T goes away: panes showing it take the next tab nothing shows (or the home screen).
// The caller removes T from Tabs and then sets Active from the focused pane.
static void ForgetTab(app_state& App, const tab* T)
{
    if(App.PromptTab == T) { App.PromptTab = 0; App.Prompt = PROMPT_NONE; }
    if(App.CameFrom == T) { App.CameFrom = 0; }
    s32 From = TabIndex(App, T); // the position just after T
    for(s32 P = 0; P < MAX_PANES; ++P)
    {
        if(App.PaneTab[P] == T) { App.PaneTab[P] = (P < PaneCount(App.Layout)) ? UnshownTab(App, From, T) : 0; }
    }
    for(page& G : App.Pages) { for(tab*& P : G.PaneTab) { if(P == T) { P = 0; } } } // other pages: home
}

// --- pages ---

static s32 PageCount(const app_state& App) { return Max(1, (s32)App.Pages.size()); }

// Makes G the current page's live state (Layout/Focus/PaneTab); what its panes show counts as seen.
static void LoadPage(app_state& App, const page& G)
{
    App.Layout = G.Layout;
    for(s32 P = 0; P < MAX_PANES; ++P) { App.PaneTab[P] = G.PaneTab[P]; }
    for(s32 P = 0; P < PaneCount(App.Layout); ++P)
    {
        if(App.PaneTab[P]) { App.PaneTab[P]->Activity = false; App.PaneTab[P]->Notice.clear(); }
    }
    FocusPane(App, G.Focus);
    App.CycleShownUntil = 0;
}

// Goes to page Index (wrapping), keeping the one it leaves as it was.
static void GoToPage(app_state& App, s32 Index)
{
    if(App.Pages.size() < 2) { return; }
    s32 Count = (s32)App.Pages.size();
    Index = ((Index % Count) + Count) % Count;
    page& Leaving = App.Pages[App.Page];
    Leaving.Layout = App.Layout;
    Leaving.Focus = App.Focus;
    for(s32 P = 0; P < MAX_PANES; ++P) { Leaving.PaneTab[P] = App.PaneTab[P]; }
    App.Page = Index;
    LoadPage(App, App.Pages[Index]);
}

// A new page after the current one, in the same layout, every pane on the home screen; goes there.
static void NewPage(app_state& App)
{
    if(App.Pages.empty())
    {
        page First = {};
        App.Pages.push_back(First);
        App.Page = 0;
    }
    page Fresh = {};
    Fresh.Layout = App.Layout;
    App.Pages.insert(App.Pages.begin() + App.Page + 1, Fresh);
    GoToPage(App, App.Page + 1);
}

// Drops the current page (not the last one); its tabs keep running, unshown.
static void ClosePage(app_state& App)
{
    if(App.Pages.size() < 2) { return; }
    App.Pages.erase(App.Pages.begin() + App.Page);
    App.Page = Min(App.Page, (s32)App.Pages.size() - 1);
    LoadPage(App, App.Pages[App.Page]);
    if(App.Pages.size() == 1) { App.Pages.clear(); App.Page = 0; }
}

static void WriteToTab(std::vector<app_command>& Cmds, tab* Tab, const std::string& Bytes)
{
    if(!Cmds.empty() && Cmds.back().Kind == CMD_WRITE && Cmds.back().Tab == Tab)
    {
        Cmds.back().Bytes += Bytes;
        return;
    }
    app_command C = {};
    C.Kind = CMD_WRITE;
    C.Tab = Tab;
    C.Bytes = Bytes;
    Cmds.push_back(C);
}

static s32 TextWidth(const std::string& S)
{
    s32 W = 0;
    size_t I = 0;
    while(I < S.size()) { W += CharWidth(DecodeUtf8(S.data(), S.size(), &I)); }
    return W;
}

static std::string TruncateText(const std::string& S, s32 MaxW)
{
    if(MaxW <= 0) { return std::string(); }
    if(TextWidth(S) <= MaxW) { return S; }
    std::string Out;
    s32 W = 0;
    size_t I = 0;
    while(I < S.size())
    {
        size_t Start = I;
        s32 CW = CharWidth(DecodeUtf8(S.data(), S.size(), &I));
        if(W + CW > MaxW - 1) { break; }
        Out.append(S, Start, I - Start);
        W += CW;
    }
    return Out + "\xE2\x80\xA6"; // …
}

static std::string ToLower(const std::string& S)
{
    std::string Out = S;
    for(char& C : Out) { if(C >= 'A' && C <= 'Z') { C = (char)(C - 'A' + 'a'); } }
    return Out;
}

static std::vector<s32> FilterSessions(const app_state& App)
{
    std::vector<s32> Out;
    std::string Needle = ToLower(App.Filter);
    for(s32 I = 0; I < (s32)App.Sessions.size(); ++I)
    {
        const session_info& S = App.Sessions[I];
        if(Needle.empty() || ToLower(S.Title + " " + S.Cwd + " " + S.Id).find(Needle) != std::string::npos) { Out.push_back(I); }
    }
    return Out;
}

static tab* FindTabBySession(const app_state& App, const std::string& Id, s32* Index)
{
    for(s32 I = 0; I < (s32)App.Tabs.size(); ++I)
    {
        if(!Id.empty() && App.Tabs[I]->SessionId == Id) { *Index = I + 1; return App.Tabs[I]; }
    }
    return 0;
}

// --- home screen layout, relative to its pane: the running tabs in sections (a heading row, then a
// row per tab; one "nothing running" row when there are none), the keys for new ones, a blank row,
// then the Claude Code sessions: title, filter, column heads, the list ---
enum home_section
{
    SECTION_NEEDS_INPUT = 0, // claude waiting on the user
    SECTION_WORKING,         // claude mid-turn
    SECTION_COMPLETED,       // claude done, waiting for the next prompt
    SECTION_SHELLS,          // no claude running
    SECTION_COUNT,
};
static const char* SectionNames[SECTION_COUNT] = {"Needs input", "Working", "Completed", "Shells"};

static s32 TabSection(const tab& T)
{
    if(T.SessionId.empty()) { return SECTION_SHELLS; }
    if(T.NeedsInput) { return SECTION_NEEDS_INPUT; }
    return T.TitleBusy ? SECTION_WORKING : SECTION_COMPLETED;
}

// The running area's rows, top down: a tab's index into Tabs, or -1 - section for a heading. The
// selection counts only the tab rows, in this order.
static std::vector<s32> HomeRows(const app_state& App)
{
    std::vector<s32> Rows;
    for(s32 Section = 0; Section < SECTION_COUNT; ++Section)
    {
        size_t Heading = Rows.size();
        for(s32 I = 0; I < (s32)App.Tabs.size(); ++I)
        {
            if(TabSection(*App.Tabs[I]) != Section) { continue; }
            if(Rows.size() == Heading) { Rows.push_back(-1 - Section); }
            Rows.push_back(I);
        }
    }
    return Rows;
}

// The running tab the home screen's selection is on, or null (it's on a session).
static tab* HomeSelectedTab(const app_state& App)
{
    s32 Seen = 0;
    for(s32 Row : HomeRows(App))
    {
        if(Row < 0) { continue; }
        if(Seen++ == App.Selected) { return App.Tabs[Row]; }
    }
    return 0;
}

static s32 HomeSessionsRow(const app_state& App) { return Max((s32)HomeRows(App).size(), 1) + 2; }
static s32 HomeListRow(const app_state& App) { return HomeSessionsRow(App) + 3; }
static s32 HomeListRows(const app_state& App, s32 PaneH) { return Max(1, PaneH - HomeListRow(App)); }

// Moves the selection through the running tabs and on into the sessions, scrolling the sessions
// list as the focused pane shows it.
static void HomeMoveSelection(app_state& App, s32 Delta)
{
    s32 Running = (s32)App.Tabs.size();
    s32 Count = Running + (s32)FilterSessions(App).size();
    App.Selected = Clamp(App.Selected + Delta, 0, Max(0, Count - 1));
    s32 Rows = HomeListRows(App, PaneRect(App, App.Focus).H);
    s32 S = App.Selected - Running; // in the sessions list
    if(S < 0) { return; }
    if(S < App.ListScroll) { App.ListScroll = S; }
    if(S >= App.ListScroll + Rows) { App.ListScroll = S - Rows + 1; }
}

// Enter on the home screen: a running tab comes into this pane, a session opens (or its tab does).
static void OpenSelected(app_state& App, std::vector<app_command>& Cmds)
{
    s32 Running = (s32)App.Tabs.size();
    tab* Picked = HomeSelectedTab(App);
    if(Picked) { SwitchTab(App, TabIndex(App, Picked)); return; }
    std::vector<s32> Filtered = FilterSessions(App);
    s32 Index = App.Selected - Running;
    if(Index < 0 || Index >= (s32)Filtered.size()) { return; }
    const session_info& S = App.Sessions[Filtered[Index]];
    s32 Existing = 0;
    if(FindTabBySession(App, S.Id, &Existing)) { SwitchTab(App, Existing); return; }
    app_command C = {};
    C.Kind = CMD_OPEN;
    C.SessionId = S.Id;
    C.Cwd = S.Cwd;
    C.Project = S.Project;
    C.Title = S.Title;
    Cmds.push_back(C);
}

static std::string ProjectName(const std::string& Cwd)
{
    size_t Slash = Cwd.find_last_of("\\/");
    return (Slash == std::string::npos) ? Cwd : Cwd.substr(Slash + 1);
}

static void NewSession(std::vector<app_command>& Cmds, const std::string& Cwd)
{
    app_command C = {};
    C.Kind = CMD_OPEN;
    C.Cwd = Cwd;
    C.Project = ProjectName(Cwd);
    Cmds.push_back(C);
}

// Turns the command bar into a path prompt, starting from Base.
static void OpenFolderPrompt(app_state& App, const std::string& Base)
{
    App.Prompt = PROMPT_NEW_FOLDER;
    App.FilterEditing = false;
    App.PathInput = Base;
    if(!Base.empty() && Base.back() != '\\' && Base.back() != '/') { App.PathInput += '\\'; }
}

// Where the home screen starts new tabs: the folder of the tab it was opened from, else the window's.
static std::string HomeCwd(const app_state& App)
{
    return App.CameFrom ? App.CameFrom->Cwd : App.LaunchCwd;
}

// --- input tokens ---

enum key_kind
{
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_ENTER,
    KEY_TAB,
    KEY_BACKSPACE,
    KEY_ESC,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_MOUSE,
    KEY_PASTE_BEGIN,
    KEY_FOCUS,
    KEY_GO_HOME,      // CSI 7777 ~: clodw's shortcuts, "home screen in this pane" (never back out of it)
    KEY_GO_PAGE,      // CSI 7778 ; N ~: clodw's Ctrl+Alt+N, "go to page N" (Codepoint = N)
    KEY_OTHER,
};

enum
{
    KMOD_SHIFT = 1,
    KMOD_ALT   = 2,
    KMOD_CTRL  = 4,
};

struct input_token
{
    s32 Kind;
    s32 Mods;
    u32 Codepoint;
    s32 MouseButton; // SGR button code, including modifier and motion bits
    s32 MouseX;      // 1-based
    s32 MouseY;      // 1-based
    bool32 MouseRelease;
    size_t Len;
};

static input_token NextToken(const std::string& S, size_t At)
{
    input_token K = {};
    u8 B = (u8)S[At];
    if(B == 0x1B)
    {
        if(At + 1 >= S.size()) { K.Kind = KEY_ESC; K.Len = 1; return K; }
        u8 N = (u8)S[At + 1];
        if(N == '[')
        {
            size_t I = At + 2;
            char Priv = 0;
            if(I < S.size() && S[I] >= 0x3C && S[I] <= 0x3F) { Priv = S[I++]; }
            s32 P[4] = {};
            s32 Count = 0;
            while(I < S.size() && ((S[I] >= '0' && S[I] <= '9') || S[I] == ';'))
            {
                if(Count == 0) { Count = 1; }
                if(S[I] == ';') { Count += 1; }
                else if(Count <= 4) { P[Count - 1] = P[Count - 1] * 10 + (S[I] - '0'); }
                ++I;
            }
            while(I < S.size() && S[I] >= 0x20 && S[I] <= 0x2F) { ++I; }
            if(I >= S.size()) { K.Kind = KEY_OTHER; K.Len = S.size() - At; return K; }
            char Final = S[I];
            K.Len = I + 1 - At;
            K.Kind = KEY_OTHER;
            K.Mods = (Count >= 2 && P[1] > 1) ? P[1] - 1 : 0;

            if(Priv == '<' && (Final == 'M' || Final == 'm'))
            {
                K.Kind = KEY_MOUSE;
                K.Mods = 0;
                K.MouseButton = P[0];
                K.MouseX = P[1];
                K.MouseY = P[2];
                K.MouseRelease = (Final == 'm');
            }
            else if(Priv) {}
            else if(Final == '~')
            {
                switch(P[0])
                {
                    case 1: case 7: K.Kind = KEY_HOME; break;
                    case 2: K.Kind = KEY_INSERT; break;
                    case 3: K.Kind = KEY_DELETE; break;
                    case 4: case 8: K.Kind = KEY_END; break;
                    case 5: K.Kind = KEY_PGUP; break;
                    case 6: K.Kind = KEY_PGDN; break;
                    case 200: K.Kind = KEY_PASTE_BEGIN; break;
                    case 7777: K.Kind = KEY_GO_HOME; break;
                    case 7778: K.Kind = KEY_GO_PAGE; K.Codepoint = (u32)P[1]; K.Mods = 0; break;
                }
            }
            else if(Final == 'A') { K.Kind = KEY_UP; }
            else if(Final == 'B') { K.Kind = KEY_DOWN; }
            else if(Final == 'C') { K.Kind = KEY_RIGHT; }
            else if(Final == 'D') { K.Kind = KEY_LEFT; }
            else if(Final == 'H') { K.Kind = KEY_HOME; }
            else if(Final == 'F') { K.Kind = KEY_END; }
            else if(Final == 'Z') { K.Kind = KEY_TAB; K.Mods = KMOD_SHIFT; }
            else if((Final == 'I' || Final == 'O') && Count == 0) { K.Kind = KEY_FOCUS; }
            return K;
        }
        if(N == 'O' && At + 2 < S.size())
        {
            K.Len = 3;
            switch(S[At + 2])
            {
                case 'A': K.Kind = KEY_UP; break;
                case 'B': K.Kind = KEY_DOWN; break;
                case 'C': K.Kind = KEY_RIGHT; break;
                case 'D': K.Kind = KEY_LEFT; break;
                case 'H': K.Kind = KEY_HOME; break;
                case 'F': K.Kind = KEY_END; break;
                default: K.Kind = KEY_OTHER; break;
            }
            return K;
        }
        // Alt+key
        size_t I = At + 1;
        K.Kind = KEY_CHAR;
        K.Mods = KMOD_ALT;
        K.Codepoint = DecodeUtf8(S.data(), S.size(), &I);
        K.Len = I - At;
        if(K.Codepoint == 0x0D) { K.Kind = KEY_ENTER; }
        if(K.Codepoint == 0x7F) { K.Kind = KEY_BACKSPACE; }
        return K;
    }

    K.Len = 1;
    if(B == 0x0D) { K.Kind = KEY_ENTER; return K; }
    if(B == 0x09) { K.Kind = KEY_TAB; return K; }
    if(B == 0x7F || B == 0x08) { K.Kind = KEY_BACKSPACE; return K; }
    if(B < 0x20)
    {
        K.Kind = KEY_CHAR;
        K.Codepoint = B;
        K.Mods = KMOD_CTRL;
        return K;
    }
    size_t I = At;
    K.Kind = KEY_CHAR;
    K.Codepoint = DecodeUtf8(S.data(), S.size(), &I);
    K.Len = I - At;
    return K;
}

static void HandleInput(app_state& App, const std::string& In, u64 NowMs, std::vector<app_command>& Cmds)
{
    size_t At = 0;
    while(At < In.size())
    {
        // --- bracketed paste body: collect until the end marker, then deliver in one piece ---
        if(App.InPaste)
        {
            size_t End = In.find("\x1b[201~", At);
            size_t Stop = (End == std::string::npos) ? In.size() : End;
            App.PasteBuf.append(In, At, Stop - At);
            if(End == std::string::npos) { break; }
            At = End + 6;
            App.InPaste = false;

            tab* Tab = ActiveTab(App);
            if(App.Prompt == PROMPT_NEW_FOLDER)
            {
                // A pasted absolute path (e.g. Explorer's "Copy as path") replaces the prefilled one.
                std::string Pasted;
                for(char C : App.PasteBuf) { if((u8)C >= 0x20 && C != '"') { Pasted += C; } }
                while(!Pasted.empty() && Pasted.back() == ' ') { Pasted.pop_back(); }
                while(!Pasted.empty() && Pasted[0] == ' ') { Pasted.erase(0, 1); }
                bool32 Absolute = (Pasted.size() >= 2 && Pasted[1] == ':') || (!Pasted.empty() && strchr("\\/~", Pasted[0]));
                App.PathInput = Absolute ? Pasted : App.PathInput + Pasted;
            }
            else if(!Tab)
            {
                for(char C : App.PasteBuf) { if((u8)C >= 0x20) { App.Filter += C; } }
                App.FilterEditing = true;
                App.Selected = (s32)App.Tabs.size(); // the first match
                App.ListScroll = 0;
            }
            else if(!Tab->Exited)
            {
                Tab->ScrollOffset = 0;
                Tab->NeedsInput = false;
                WriteToTab(Cmds, Tab, Tab->Term.BracketedPaste ? "\x1b[200~" + App.PasteBuf + "\x1b[201~" : App.PasteBuf);
            }
            App.PasteBuf.clear();
            continue;
        }

        input_token K = NextToken(In, At);
        std::string Raw = In.substr(At, K.Len);
        At += K.Len;
        if(K.Kind == KEY_PASTE_BEGIN) { App.InPaste = true; App.PasteBuf.clear(); continue; }
        if(K.Kind == KEY_FOCUS)
        {
            App.WindowUnfocused = (Raw.back() == 'O'); // the window's own focus: the focused pane's edge shows it
            tab* Focused = ActiveTab(App);
            if(Focused && Focused->Pty && Focused->Term.FocusEvents) { WriteToTab(Cmds, Focused, Raw); }
            continue;
        }

        tab* Tab = ActiveTab(App);
        // The tab switcher box stays up only while tabs are being cycled: any other key takes it down.
        bool32 Cycling = (K.Kind == KEY_PGUP || K.Kind == KEY_PGDN) && K.Mods == KMOD_CTRL;
        if(!Cycling && K.Kind != KEY_MOUSE) { App.CycleShownUntil = 0; }

        // --- folder prompt: edit the path for a new session ---
        if(App.Prompt == PROMPT_NEW_FOLDER)
        {
            if(K.Kind == KEY_MOUSE) { continue; }
            App.Status.clear(); // a "Not a folder" or match list is stale once the path changes
            std::string& P = App.PathInput;
            bool32 Ctrl = (K.Kind == KEY_CHAR && K.Mods == KMOD_CTRL);
            if(K.Kind == KEY_ESC) { App.Prompt = PROMPT_NONE; P.clear(); }
            else if(K.Kind == KEY_ENTER) { app_command O = {}; O.Kind = CMD_OPEN_FOLDER; O.Cwd = P; Cmds.push_back(O); }
            else if(K.Kind == KEY_TAB) { app_command T = {}; T.Kind = CMD_COMPLETE_FOLDER; Cmds.push_back(T); }
            else if(K.Kind == KEY_BACKSPACE)
            {
                while(!P.empty() && ((u8)P.back() & 0xC0) == 0x80) { P.pop_back(); }
                if(!P.empty()) { P.pop_back(); }
            }
            else if(Ctrl && K.Codepoint == 0x15) { P.clear(); } // Ctrl+U
            else if(Ctrl && K.Codepoint == 0x17) // Ctrl+W: drop the last folder name
            {
                while(!P.empty() && (P.back() == '\\' || P.back() == '/')) { P.pop_back(); }
                size_t Slash = P.find_last_of("\\/");
                P.erase(Slash == std::string::npos ? 0 : Slash + 1);
            }
            else if(K.Kind == KEY_CHAR && !(K.Mods & (KMOD_CTRL | KMOD_ALT))) { AppendUtf8(P, K.Codepoint); }
            continue;
        }

        // --- y/n confirmation (close tab) ---
        if(App.Prompt)
        {
            if(K.Kind == KEY_MOUSE) { continue; }
            u32 C = (K.Kind == KEY_CHAR) ? (K.Codepoint | 0x20) : 0; // lower-case ASCII letters
            if(C == 'y' && App.Prompt == PROMPT_CLOSE_TAB && App.PromptTab)
            {
                app_command Close = {};
                Close.Kind = CMD_CLOSE;
                Close.Tab = App.PromptTab;
                Cmds.push_back(Close);
            }
            App.Prompt = PROMPT_NONE;
            App.PromptTab = 0;
            continue;
        }

        // --- global keys ---
        // The prefix: a tab's pane goes to the home screen, with that tab selected and remembered, so
        // Enter, Esc or the prefix again go straight back; on the home screen it goes back. clodw's
        // shortcuts send KEY_GO_HOME instead, which only ever goes home.
        bool32 GoHome = (K.Kind == KEY_GO_HOME);
        bool32 Prefix = (App.PrefixKey & PREFIX_ALT) ? (K.Kind == KEY_CHAR && K.Mods == KMOD_ALT && K.Codepoint == (App.PrefixKey & 0xFF))
                                                     : (K.Kind == KEY_CHAR && K.Mods == KMOD_CTRL && K.Codepoint == App.PrefixKey);
        if(GoHome || Prefix)
        {
            if(Tab)
            {
                App.CameFrom = Tab;
                App.Selected = 0; // its place among the tab rows
                for(s32 Row : HomeRows(App))
                {
                    if(Row >= 0 && App.Tabs[Row] == Tab) { break; }
                    if(Row >= 0) { App.Selected += 1; }
                }
                App.Filter.clear();
                App.FilterEditing = false;
                SwitchTab(App, 0);
            }
            else if(!GoHome && App.CameFrom) { SwitchTab(App, TabIndex(App, App.CameFrom)); }
            continue;
        }
        // Alt+Q / Alt+Shift+Q: focus the next / previous pane.
        if(K.Kind == KEY_CHAR && K.Mods == KMOD_ALT && (K.Codepoint == 'q' || K.Codepoint == 'Q'))
        {
            s32 Count = PaneCount(App.Layout);
            FocusPane(App, (App.Focus + (K.Codepoint == 'q' ? 1 : Count - 1)) % Count);
            continue;
        }
        // Alt+arrows pick a pane. 2x2 grid: up = top left, right = top right, left = bottom left,
        // down = bottom right. Side by side: down = left, right = right. Other cases go to the program.
        bool32 Arrow = K.Kind == KEY_UP || K.Kind == KEY_RIGHT || K.Kind == KEY_LEFT || K.Kind == KEY_DOWN;
        // Ctrl+Shift+Alt+arrows pick the layout: up = single, right = side by side, down = 2x2 grid,
        // left = main + 2.
        if(Arrow && K.Mods == (KMOD_CTRL | KMOD_SHIFT | KMOD_ALT))
        {
            SetLayout(App, K.Kind == KEY_UP ? LAYOUT_SINGLE : (K.Kind == KEY_RIGHT ? LAYOUT_SIDE : (K.Kind == KEY_DOWN ? LAYOUT_GRID : LAYOUT_MAIN)));
            continue;
        }
        if(Arrow && K.Mods == KMOD_ALT && App.Layout == LAYOUT_GRID)
        {
            FocusPane(App, K.Kind == KEY_UP ? 0 : (K.Kind == KEY_RIGHT ? 1 : (K.Kind == KEY_LEFT ? 2 : 3)));
            continue;
        }
        if((K.Kind == KEY_DOWN || K.Kind == KEY_RIGHT) && K.Mods == KMOD_ALT && App.Layout == LAYOUT_SIDE)
        {
            FocusPane(App, K.Kind == KEY_DOWN ? 0 : 1);
            continue;
        }
        // Alt+Insert/Home/Delete/End pick a pane the way those keys sit on a keyboard: Insert top left,
        // Home top right, Delete bottom left, End bottom right. Side by side: left / right column;
        // main + 2: Insert/Delete the main pane, Home and End the right two. A single pane: the program's.
        bool32 Block = K.Kind == KEY_INSERT || K.Kind == KEY_HOME || K.Kind == KEY_DELETE || K.Kind == KEY_END;
        if(Block && K.Mods == KMOD_ALT && App.Layout != LAYOUT_SINGLE)
        {
            bool32 Right = (K.Kind == KEY_HOME || K.Kind == KEY_END);
            bool32 Bottom = (K.Kind == KEY_DELETE || K.Kind == KEY_END);
            s32 Pane = 0;
            if(App.Layout == LAYOUT_GRID) { Pane = (Bottom ? 2 : 0) + (Right ? 1 : 0); }
            else if(App.Layout == LAYOUT_SIDE) { Pane = Right ? 1 : 0; }
            else { Pane = Right ? (Bottom ? 2 : 1) : 0; }
            FocusPane(App, Pane);
            continue;
        }
        // Pages: Alt+PgUp/PgDn step through them, clodw's Ctrl+Alt+1-9 jump.
        if((K.Kind == KEY_PGUP || K.Kind == KEY_PGDN) && K.Mods == KMOD_ALT)
        {
            if(App.Pages.size() < 2)
            {
                App.Status = "One page so far: P on the home screen makes another";
                App.StatusUntil = NowMs + 3000;
            }
            GoToPage(App, App.Page + (K.Kind == KEY_PGDN ? 1 : -1));
            continue;
        }
        if(K.Kind == KEY_GO_PAGE)
        {
            if(K.Codepoint >= 1 && (s32)K.Codepoint <= PageCount(App)) { GoToPage(App, (s32)K.Codepoint - 1); }
            continue;
        }
        if(Cycling)
        {
            CycleTab(App, K.Kind == KEY_PGDN ? 1 : -1);
            App.CycleShownUntil = NowMs + 1200; // the switcher box (Compose), a moment past the last step
            continue;
        }
        if(K.Kind == KEY_CHAR && K.Mods == KMOD_ALT && K.Codepoint >= '0' && K.Codepoint <= '9') { SwitchTab(App, (s32)(K.Codepoint - '0')); continue; }

        // --- mouse ---
        if(K.Kind == KEY_MOUSE)
        {
            s32 Button = K.MouseButton & ~(4 | 8 | 16); // strip shift/alt/ctrl bits
            bool32 Wheel = (Button & 64) != 0;
            bool32 Motion = (Button & 32) != 0;
            bool32 LeftPress = (Button == 0 && !K.MouseRelease);
            s32 Row = K.MouseY - 1; // 0-based frame row

            if(Row == 0)
            {
                if(LeftPress)
                {
                    for(const bar_hit& H : App.BarHits)
                    {
                        if(K.MouseX - 1 < H.X0 || K.MouseX - 1 >= H.X1) { continue; }
                        if(H.Pane <= -2) { GoToPage(App, -2 - H.Pane); }
                        else if(H.Pane < 0) { SwitchTab(App, 0); }
                        else { FocusPane(App, H.Pane); }
                    }
                }
                continue;
            }

            // The command bar row belongs to clod.
            if(Row > ContentRows(App.Height)) { continue; }

            // --- the pane: under the pointer, except that a drag stays in the pane it began in ---
            s32 Col = K.MouseX - 1;
            bool32 Press = !Wheel && !Motion && !K.MouseRelease;
            bool32 Held = App.DragPane && !Wheel && (K.MouseRelease || (Motion && (Button & 3) != 3));
            s32 Pane = Held ? App.DragPane - 1 : -1;
            for(s32 P = 0; P < PaneCount(App.Layout) && Pane < 0; ++P)
            {
                pane_rect R = PaneRect(App, P);
                if(Col >= R.X && Col < R.X + R.W && Row >= R.Y && Row < R.Y + R.H) { Pane = P; }
            }
            if(K.MouseRelease) { App.DragPane = 0; }
            if(Pane < 0) { continue; } // a separator line
            if(Press)
            {
                App.DragPane = Pane + 1;
                if(Pane != App.Focus) { FocusPane(App, Pane); }
            }
            pane_rect R = PaneRect(App, Pane);
            s32 X = Clamp(Col - R.X, 0, R.W - 1) + 1; // 1-based within the pane
            s32 Y = Clamp(Row - R.Y, 0, R.H - 1) + 1;
            tab* Target = App.PaneTab[Pane];

            if(!Target)
            {
                // --- home screen: a click selects a running tab or a session, a second click opens it ---
                s32 Running = (s32)App.Tabs.size();
                s32 Clicked = -1;
                std::vector<s32> Rows = HomeRows(App);
                for(s32 Ri = 0, Pos = 0; Ri < (s32)Rows.size(); ++Ri)
                {
                    if(Rows[Ri] < 0) { continue; }
                    if(Ri == Y - 1) { Clicked = Pos; }
                    Pos += 1;
                }
                s32 ListRow = HomeListRow(App);
                if(Y - 1 >= ListRow && Y - 1 < ListRow + HomeListRows(App, R.H) && App.ListScroll + (Y - 1 - ListRow) < (s32)FilterSessions(App).size())
                {
                    Clicked = Running + App.ListScroll + (Y - 1 - ListRow);
                }
                if(Wheel) { HomeMoveSelection(App, (Button & 1) ? 3 : -3); }
                else if(LeftPress && Clicked >= 0)
                {
                    if(Clicked == App.Selected) { OpenSelected(App, Cmds); }
                    else { App.Selected = Clicked; }
                }
                continue;
            }

            if(!Target->ViewImage.empty())
            {
                // The wheel steps through the folder's images; a click closes the image.
                if(Wheel) { app_command V = {}; V.Kind = CMD_VIEW_STEP; V.Tab = Target; V.Step = (Button & 1) ? 1 : -1; Cmds.push_back(V); }
                else if(Press) { Target->ViewImage.clear(); Target->ViewPixels.clear(); }
                continue;
            }
            if(Target->Term.MouseMode && Target->ScrollOffset == 0 && !Target->Exited)
            {
                if(Motion && Target->Term.MouseMode == 1000) { continue; }
                char Buf[64];
                if(Target->Term.MouseSgr)
                {
                    snprintf(Buf, sizeof(Buf), "\x1b[<%d;%d;%d%c", K.MouseButton, X, Y, K.MouseRelease ? 'm' : 'M');
                    WriteToTab(Cmds, Target, Buf);
                }
                else if(X <= 223 && Y <= 223)
                {
                    s32 Code = K.MouseRelease ? ((K.MouseButton & ~3) | 3) : K.MouseButton;
                    snprintf(Buf, sizeof(Buf), "\x1b[M%c%c%c", (char)(32 + Code), (char)(32 + X), (char)(32 + Y));
                    WriteToTab(Cmds, Target, Buf);
                }
            }
            else if(Wheel)
            {
                s32 Max = Target->Term.AltActive ? 0 : (s32)Target->Term.Scrollback.size();
                Target->ScrollOffset = Clamp(Target->ScrollOffset + ((Button & 1) ? -3 : 3), 0, Max);
                Target->SeenPushed = Target->Term.ScrollbackPushed;
            }
            continue;
        }

        // --- home screen ---
        if(!Tab)
        {
            s32 Running = (s32)App.Tabs.size();
            tab* SelectedTab = HomeSelectedTab(App);
            bool32 Typing = App.FilterEditing && K.Kind == KEY_CHAR && !(K.Mods & (KMOD_CTRL | KMOD_ALT));
            if(Typing)
            {
                AppendUtf8(App.Filter, K.Codepoint);
                App.Selected = Running; // the first match
                App.ListScroll = 0;
            }
            else if(K.Kind == KEY_BACKSPACE && App.FilterEditing)
            {
                while(!App.Filter.empty() && ((u8)App.Filter.back() & 0xC0) == 0x80) { App.Filter.pop_back(); }
                if(!App.Filter.empty()) { App.Filter.pop_back(); }
                App.Selected = Running;
                App.ListScroll = 0;
            }
            else if(K.Kind == KEY_ESC && !App.FilterEditing && App.Filter.empty() && App.CameFrom) { SwitchTab(App, TabIndex(App, App.CameFrom)); }
            else if(K.Kind == KEY_ESC)
            {
                App.Filter.clear();
                App.FilterEditing = false;
                App.Selected = 0;
                App.ListScroll = 0;
            }
            else if(K.Kind == KEY_TAB || K.Kind == KEY_LEFT || K.Kind == KEY_RIGHT)
            {
                // Another pane gets the focus (and the keys: a program there takes them).
                s32 Count = PaneCount(App.Layout);
                bool32 Back = (K.Kind == KEY_LEFT) || (K.Kind == KEY_TAB && K.Mods == KMOD_SHIFT);
                FocusPane(App, (App.Focus + (Back ? Count - 1 : 1)) % Count);
            }
            else if(K.Kind == KEY_UP) { HomeMoveSelection(App, -1); }
            else if(K.Kind == KEY_DOWN) { HomeMoveSelection(App, 1); }
            else if(K.Kind == KEY_PGUP) { HomeMoveSelection(App, -HomeListRows(App, PaneRect(App, App.Focus).H)); }
            else if(K.Kind == KEY_PGDN) { HomeMoveSelection(App, HomeListRows(App, PaneRect(App, App.Focus).H)); }
            else if(K.Kind == KEY_HOME) { HomeMoveSelection(App, -1000000); }
            else if(K.Kind == KEY_END) { HomeMoveSelection(App, 1000000); }
            else if(K.Kind == KEY_ENTER) { App.FilterEditing = false; OpenSelected(App, Cmds); }
            else if(K.Kind == KEY_CHAR && K.Mods == 0)
            {
                u32 C = K.Codepoint;
                if(C == '/') { App.FilterEditing = true; }
                else if(C == 'k') { HomeMoveSelection(App, -1); }
                else if(C == 'j') { HomeMoveSelection(App, 1); }
                else if(C >= '1' && C <= '9') { SwitchTab(App, (s32)(C - '0')); }
                else if(C == 'n') { NewSession(Cmds, HomeCwd(App)); }
                else if(C == 'o') { OpenFolderPrompt(App, HomeCwd(App)); }
                else if(C == 't') { app_command S = {}; S.Kind = CMD_OPEN_SHELL; S.Cwd = HomeCwd(App); Cmds.push_back(S); }
                else if(C == 'P') { NewPage(App); }
                else if(C == 'X') { ClosePage(App); }
                else if(C == '[' || C == ']') { GoToPage(App, App.Page + (C == ']' ? 1 : -1)); }
                else if(C == 'R' && SelectedTab) { app_command Re = {}; Re.Kind = CMD_RESTART; Re.Tab = SelectedTab; Cmds.push_back(Re); }
                else if(C == 'm')
                {
                    App.MouseCaptureOff = !App.MouseCaptureOff;
                    App.Status = App.MouseCaptureOff ? "Mouse capture off: terminal selection works, wheel scrolling off" : "Mouse capture on";
                    App.StatusUntil = NowMs + 3000;
                }
                else if(C == 'N')
                {
                    // In the selected tab's or session's folder.
                    std::vector<s32> Filtered = FilterSessions(App);
                    if(SelectedTab) { NewSession(Cmds, SelectedTab->Cwd); }
                    else if(App.Selected - Running < (s32)Filtered.size()) { NewSession(Cmds, App.Sessions[Filtered[App.Selected - Running]].Cwd); }
                }
                else if(C == 'x' && SelectedTab) { App.Prompt = PROMPT_CLOSE_TAB; App.PromptTab = SelectedTab; }
                else if(C == 'r') { app_command R = {}; R.Kind = CMD_RESCAN; Cmds.push_back(R); }
                else if(C == 'q') { app_command D = {}; D.Kind = CMD_DETACH; Cmds.push_back(D); }
            }
            continue;
        }

        // --- session tab ---
        if(!Tab->ViewImage.empty())
        {
            // --- an image is up: arrows, Space/Backspace, PgUp/PgDn, Home/End step through its
            // folder's images; any other key closes it. None reaches the program ---
            s32 Step = 0;
            bool32 Space = (K.Kind == KEY_CHAR && K.Codepoint == ' ' && !K.Mods);
            if(K.Kind == KEY_RIGHT || K.Kind == KEY_DOWN || K.Kind == KEY_PGDN || Space) { Step = 1; }
            else if(K.Kind == KEY_LEFT || K.Kind == KEY_UP || K.Kind == KEY_PGUP || K.Kind == KEY_BACKSPACE) { Step = -1; }
            else if(K.Kind == KEY_HOME) { Step = -(s32)Tab->ViewIndex; }
            else if(K.Kind == KEY_END) { Step = (s32)Tab->ViewCount - 1 - (s32)Tab->ViewIndex; }
            if(!Step && K.Kind != KEY_HOME && K.Kind != KEY_END)
            {
                Tab->ViewImage.clear();
                Tab->ViewPixels.clear();
            }
            else if(Step)
            {
                app_command V = {};
                V.Kind = CMD_VIEW_STEP;
                V.Tab = Tab;
                V.Step = Step;
                Cmds.push_back(V);
            }
            continue;
        }
        // A fullscreen child (alternate screen) keeps its own history, so it gets the keys instead.
        if(K.Mods == KMOD_SHIFT && (K.Kind == KEY_PGUP || K.Kind == KEY_PGDN) && !Tab->Term.AltActive)
        {
            s32 Page = Max(1, PaneRect(App, App.Focus).H - 1);
            s32 Max = Tab->Term.AltActive ? 0 : (s32)Tab->Term.Scrollback.size();
            Tab->ScrollOffset = Clamp(Tab->ScrollOffset + (K.Kind == KEY_PGUP ? Page : -Page), 0, Max);
            Tab->SeenPushed = Tab->Term.ScrollbackPushed;
            continue;
        }
        if(Tab->Exited)
        {
            if(K.Kind == KEY_ENTER) { app_command R = {}; R.Kind = CMD_RESTART; R.Tab = Tab; Cmds.push_back(R); }
            continue;
        }
        Tab->ScrollOffset = 0;
        Tab->NeedsInput = false; // answered
        if(Tab->Term.AppCursorKeys && Raw.size() == 3 && Raw[0] == 0x1B && Raw[1] == '[' && strchr("ABCDHF", Raw[2])) { Raw[1] = 'O'; }
        WriteToTab(Cmds, Tab, Raw);
    }
}

// Keeps a scrolled-back view pinned to the same content while new output pushes lines into scrollback.
static void TabSyncScroll(tab& Tab)
{
    if(Tab.ScrollOffset > 0)
    {
        Tab.ScrollOffset += (s32)(Tab.Term.ScrollbackPushed - Tab.SeenPushed);
        Tab.ScrollOffset = Clamp(Tab.ScrollOffset, 0, Tab.Term.AltActive ? 0 : (s32)Tab.Term.Scrollback.size());
    }
    Tab.SeenPushed = Tab.Term.ScrollbackPushed;
}

// The command a shell's title says is running ("<path>\cmd.exe - ping  -n 3 x", spaces tidied), or
// empty: at the prompt, a title some program set itself, or one of the steps clod itself has cmd
// run at startup (replay saved output, init.bat, clink, cd, claude by full path).
static std::string RunningCommand(const std::string& Title)
{
    size_t Exe = ToLower(Title).find("cmd.exe - ");
    if(Exe == std::string::npos) { return std::string(); }
    std::string Command;
    for(char C : Title.substr(Exe + 10)) { if(!(C == ' ' && (Command.empty() || Command.back() == ' '))) { Command += C; } }
    while(!Command.empty() && Command.back() == ' ') { Command.pop_back(); }
    bool32 Ours = Command.compare(0, 6, "type \"") == 0 || Command.compare(0, 6, "call \"") == 0 || Command.compare(0, 7, "cd /d \"") == 0 ||
                  Command.find("clink_x64.exe") != std::string::npos ||
                  (!Command.empty() && Command[0] == '"' && (Command.find(" --session-id ") != std::string::npos || Command.find(" --resume ") != std::string::npos));
    return Ours ? std::string() : Command;
}

static std::string TabName(const tab& Tab)
{
    // A shell: cmd titles itself "<path>\cmd.exe - <command>" while a command runs, and just its exe
    // path at the prompt. Show the running command, else the full path it's in (it follows cd); a
    // title some program set itself (title, vim) shows as is. Only the folder, at the prompt, gets the
    // prompt mark (the way claude's own titles start with its ✳), so ❯ means "the shell is yours".
    if(Tab.SessionId.empty())
    {
        const char* Mark = "❯ "; // ❯
        if(!Tab.Term.Title.empty() && ToLower(Tab.Term.Title).find("cmd.exe") == std::string::npos) { return Tab.Term.Title; }
        std::string Running = RunningCommand(Tab.Term.Title);
        if(!Running.empty()) { return Running; }
        return Mark + (!Tab.Cwd.empty() ? Tab.Cwd : Tab.Project); // the full path it's in
    }
    std::string Fallback = !Tab.Title.empty() ? Tab.Title : (!Tab.Project.empty() ? Tab.Project : "claude");
    std::string Name = Tab.Term.Title;
    if(Name.empty()) { return Fallback; }
    size_t Generic = Name.find("Claude Code");
    if(Generic != std::string::npos && !Tab.Title.empty()) { Name.replace(Generic, 11, Tab.Title); }
    return Name;
}

// --- composition ---

struct frame
{
    s32 Width;
    s32 Height;
    std::vector<cell> Cells;
    s32 CursorX;
    s32 CursorY;
    bool32 CursorHidden;
    s32 CursorShape;             // DECSCUSR value of the program owning the cursor (0 = default)
    s32 PaneCount;               // the panes' rectangles, so a window can keep a selection inside one
    pane_rect Panes[MAX_PANES];
    u32 PaneMouseTaken;          // bit per pane: clod or the program there handles the mouse, so a plain drag isn't a selection
    s32 ImagePane;               // pane + 1 whose image the window draws over the cells (WindowDrawsImages); 0 = none
    std::string ImagePath;
};

static cell Style(u32 Fg, u32 Bg, u16 Attr)
{
    cell C = {};
    C.Fg = Fg;
    C.Bg = Bg;
    C.Attr = Attr;
    return C;
}

static void FillRow(frame& F, s32 Y, s32 X0, s32 X1, cell S)
{
    if(Y < 0 || Y >= F.Height) { return; }
    for(s32 X = Max(X0, 0); X < Min(X1, F.Width); ++X) { F.Cells[(size_t)Y * F.Width + X] = S; }
}

static s32 PutText(frame& F, s32 X, s32 Y, const std::string& S, cell Style, s32 MaxX)
{
    if(Y < 0 || Y >= F.Height) { return X; }
    MaxX = Min(MaxX, F.Width);
    size_t I = 0;
    while(I < S.size() && X < MaxX)
    {
        u32 Cp = DecodeUtf8(S.data(), S.size(), &I);
        if(Cp < 0x20 || Cp == 0x7F) { Cp = ' '; }
        s32 W = CharWidth(Cp);
        if(W == 0) { continue; }
        if(X + W > MaxX) { break; }
        cell C = Style;
        C.Codepoint = Cp;
        C.Flags = (W == 2) ? CELL_WIDE : 0;
        F.Cells[(size_t)Y * F.Width + X] = C;
        if(W == 2)
        {
            cell D = Style;
            D.Flags = CELL_WIDE_CONT;
            F.Cells[(size_t)Y * F.Width + X + 1] = D;
        }
        X += W;
    }
    return X;
}

// What each ui_color is in a console client (clodw draws them in its theme's colors).
static const u32 UiDefault[UI_COUNT] = {
    Palette(236), Palette(250), Palette(255), Palette(244), Palette(240), Palette(237), Palette(240), Palette(110),
    COLOR_RGB | 0xD77757, Rgb(20, 20, 20), Rgb(70, 35, 25), Rgb(240, 200, 120), Rgb(110, 200, 120),
};

static void Compose(const app_state& App, u64 NowMs, u64 NowFiletime, frame& F, std::vector<bar_hit>& Hits)
{
    s32 W = App.Width;
    s32 H = App.Height;
    F.Width = W;
    F.Height = H;
    F.Cells.assign((size_t)Max(W, 0) * Max(H, 0), cell{});
    F.CursorHidden = true;
    F.CursorShape = 0;
    F.PaneCount = 0;
    F.PaneMouseTaken = 0;
    F.ImagePane = 0;
    F.ImagePath.clear();
    Hits.clear();
    if(W <= 0 || H <= 0) { return; }

    const tab* Active = ActiveTab(App);
    cell Bar = Style(Ui(UI_BAR_TEXT), Ui(UI_BAR), 0);
    cell BarDim = Style(Ui(UI_DIM), Ui(UI_BAR), 0);
    cell BarActive = Style(Ui(UI_ON_ACCENT), Ui(UI_ACCENT), ATTR_BOLD);
    cell BarShown = Style(Ui(UI_BRIGHT), Ui(UI_SHOWN), 0); // visible in a pane that isn't focused
    cell Dim = Style(Ui(UI_DIM), 0, 0);
    cell Accent = Style(Ui(UI_ACCENT), 0, ATTR_BOLD);

    // --- top bar: the name of what each top pane shows, above it (the focused one in the accent);
    // on the right how many tabs run and how many want a look. Lower panes' names sit on their rule ---
    FillRow(F, 0, 0, W, Bar);
    s32 NamesEnd = 0;
    for(s32 Pane = 0; Pane < PaneCount(App.Layout); ++Pane)
    {
        pane_rect PR = PaneRect(App, Pane);
        if(PR.Y != 1) { continue; }
        const tab* T = App.PaneTab[Pane];
        std::string Name = T ? TabName(*T) : std::string("Home");
        s32 X0 = PR.X;
        s32 X = PutText(F, X0, 0, " " + TruncateText(Name, Max(PR.W - 3, 1)) + " ", Pane == App.Focus ? BarActive : BarShown, PR.X + PR.W);
        bar_hit Hit = {X0, X, Pane};
        Hits.push_back(Hit);
        NamesEnd = Max(NamesEnd, X);
    }
    {
        s32 Notices = 0;
        bool32 AllFinished = true;
        for(const tab* T : App.Tabs)
        {
            if(T->Notice.empty()) { continue; }
            Notices += 1;
            AllFinished = AllFinished && T->Notice == "finished";
        }
        // The tabs by the home screen's sections ("1 needs input · 2 working · 3 completed · 1 shell"),
        // or just "7 running" when that doesn't fit; then the unseen notices.
        static const char* Labels[SECTION_COUNT] = {"needs input", "working", "completed", "shell"};
        s32 InSection[SECTION_COUNT] = {};
        for(const tab* T : App.Tabs) { InSection[TabSection(*T)] += 1; }
        std::string Note = Notices ? "\xE2\x97\x8F " + std::to_string(Notices) + (AllFinished ? " finished" : " new") : std::string(); // ●
        s32 NoteW = Notices ? 3 + TextWidth(Note) : 0;
        s32 SectionsW = 0;
        for(s32 Sec = 0; Sec < SECTION_COUNT; ++Sec)
        {
            if(!InSection[Sec]) { continue; }
            std::string Part = std::to_string(InSection[Sec]) + " " + Labels[Sec] + (Sec == SECTION_SHELLS && InSection[Sec] > 1 ? "s" : "");
            SectionsW += (SectionsW ? 3 : 0) + TextWidth(Part);
        }
        std::string Count = std::to_string(App.Tabs.size()) + " running";
        bool32 BySection = SectionsW > 0 && W - SectionsW - NoteW - 1 >= NamesEnd + 2;
        s32 Width = (BySection ? SectionsW : TextWidth(Count)) + NoteW;
        s32 RX = W - Width - 1;
        if(RX >= NamesEnd + 2)
        {
            s32 X = RX;
            if(!BySection) { X = PutText(F, X, 0, Count, Bar, W); }
            for(s32 Sec = 0; Sec < SECTION_COUNT && BySection; ++Sec)
            {
                if(!InSection[Sec]) { continue; }
                if(X > RX) { X = PutText(F, X, 0, " \xC2\xB7 ", BarDim, W); } // ·
                std::string Part = std::to_string(InSection[Sec]) + " " + Labels[Sec] + (Sec == SECTION_SHELLS && InSection[Sec] > 1 ? "s" : "");
                cell PartStyle = (Sec == SECTION_NEEDS_INPUT) ? Style(Ui(UI_ACCENT), Ui(UI_BAR), ATTR_BOLD) : (Sec == SECTION_WORKING ? Style(Ui(UI_NOTE), Ui(UI_BAR), 0) : Bar);
                X = PutText(F, X, 0, Part, PartStyle, W);
            }
            if(Notices)
            {
                X = PutText(F, X, 0, " \xC2\xB7 ", BarDim, W); // ·
                X = PutText(F, X, 0, Note, Style(Ui(UI_ACCENT), Ui(UI_BAR), ATTR_BOLD), W);
            }
            bar_hit Hit = {RX, X, -1};
            Hits.push_back(Hit);
            // --- the pages, left of the counts when there's more than one: each a number, the
            // current one in the accent; a click goes there ---
            s32 PX = RX - 2;
            for(s32 G = PageCount(App) - 1; G >= 0 && App.Pages.size() > 1; --G)
            {
                std::string Label = " " + std::to_string(G + 1) + " ";
                PX -= TextWidth(Label);
                if(PX < NamesEnd + 2) { break; }
                PutText(F, PX, 0, Label, G == App.Page ? BarActive : BarShown, W);
                bar_hit PageHit = {PX, PX + TextWidth(Label), -2 - G};
                Hits.push_back(PageHit);
            }
        }
    }

    // --- command bar (bottom row) ---
    // On the home screen its keys; in a tab only how to get home. Status or scroll position on the
    // right. In the accent while a y/n or the folder prompt is up.
    if(H >= 2)
    {
        s32 Y = H - 1;
        bool32 Asking = (App.Prompt == PROMPT_CLOSE_TAB);
        bool32 Folder = (App.Prompt == PROMPT_NEW_FOLDER);
        bool32 Orange = Asking || Folder;
        cell CmdBar = Orange ? Style(Ui(UI_ON_ACCENT), Ui(UI_ACCENT), 0) : Bar;
        cell KeyStyle = Style(Ui(Orange ? UI_ON_ACCENT : UI_BRIGHT), CmdBar.Bg, ATTR_BOLD);
        cell LabelStyle = Style(Ui(Orange ? UI_ON_ACCENT_DIM : UI_DIM), CmdBar.Bg, 0);
        FillRow(F, Y, 0, W, CmdBar);

        if(Asking)
        {
            const tab* Asked = App.PromptTab;
            std::string Question = Asked ? "Close " + std::to_string(TabIndex(App, Asked)) + " \"" + TruncateText(TabName(*Asked), 40) + "\" and end what it runs?" : std::string("Close?");
            s32 BX = PutText(F, 3, Y, Question, KeyStyle, W) + 3;
            BX = PutText(F, BX, Y, "y", KeyStyle, W);
            BX = PutText(F, BX, Y, " close", LabelStyle, W) + 2;
            BX = PutText(F, BX, Y, "any other key", KeyStyle, W);
            PutText(F, BX, Y, " cancel", LabelStyle, W);
        }
        else if(Folder)
        {
            // Label, the path (its tail when too long) and the cursor; a status or the keys on the right.
            s32 PX = PutText(F, 3, Y, "New session in ", LabelStyle, W);
            std::string Shown = App.PathInput;
            while(!Shown.empty() && TextWidth(Shown) > W - PX - 1)
            {
                size_t I = 0;
                DecodeUtf8(Shown.data(), Shown.size(), &I);
                Shown.erase(0, I);
            }
            PX = PutText(F, PX, Y, Shown, KeyStyle, W);
            std::string Right = "Tab complete  Enter open  Esc cancel";
            cell RightStyle = LabelStyle;
            if(!App.Status.empty() && NowMs < App.StatusUntil) { Right = App.Status; RightStyle = KeyStyle; }
            if(W - PX - 4 >= 8)
            {
                Right = TruncateText(Right, W - PX - 4);
                PutText(F, W - TextWidth(Right) - 1, Y, Right, RightStyle, W);
            }
            F.CursorHidden = false;
            F.CursorX = Min(PX, W - 1);
            F.CursorY = Y;
        }
        else
        {
            static const char* HomeKeys[] = {
                "Enter", "show here", "1-9", "show N", "\xE2\x86\x90\xE2\x86\x92", "pane", "x", "close", "R", "restart",
                "/", "filter", "m", "mouse", "r", "rescan", "q", "detach",
            };
            const char** Keys = HomeKeys;
            s32 KeyCount = Active ? 0 : (s32)ArrayCount(HomeKeys); // in a tab the left stays empty

            // --- right side: a status message or scroll position outranks the key hints ---
            std::string Right;
            cell RightStyle = LabelStyle;
            bool32 Important = false;
            if(!App.Status.empty() && NowMs < App.StatusUntil)
            {
                Right = App.Status;
                RightStyle = Style(Ui(UI_NOTE), CmdBar.Bg, ATTR_BOLD);
                Important = true;
            }
            else if(Active && Active->ScrollOffset > 0)
            {
                Right = "\xE2\x86\x91 " + std::to_string(Active->ScrollOffset) + " lines up  Shift+PgDn to follow";
                RightStyle = KeyStyle;
                Important = true;
            }
            else if(Active) { Right = PrefixName(App.PrefixKey) + " home"; }
            else if(App.CameFrom) { Right = "Esc back"; }
            Right = TruncateText(Right, Max(W - 3, 0));
            s32 RightX = W - TextWidth(Right) - 1;
            s32 KeysEnd = Important ? Max(RightX - 2, 0) : W;

            // The left starts with the date and time (cell 1 stays blank: clodw draws its keep-active
            // light there), then the home screen's keys.
            s32 BX = 3;
            if(!App.Clock.empty() && BX + TextWidth(App.Clock) < RightX) { BX = PutText(F, BX, Y, App.Clock, LabelStyle, W) + 3; }
            // Whole entries only: the first one that doesn't fit ends the list.
            s32 ListEnd = (Right.empty() || Important) ? KeysEnd : RightX - 2;
            for(s32 I = 0; I + 1 < KeyCount; I += 2)
            {
                if(BX + TextWidth(Keys[I]) + 1 + TextWidth(Keys[I + 1]) > ListEnd) { break; }
                BX = PutText(F, BX, Y, Keys[I], KeyStyle, KeysEnd);
                BX = PutText(F, BX, Y, std::string(" ") + Keys[I + 1], LabelStyle, KeysEnd) + 2;
            }
            if(Important || RightX >= BX) { PutText(F, RightX, Y, Right, RightStyle, W); }
        }
    }

    // --- panes: each draws its tab, or the home screen, inside its own rectangle ---
    F.PaneCount = PaneCount(App.Layout);
    for(s32 Pane = 0; Pane < PaneCount(App.Layout); ++Pane)
    {
        F.Panes[Pane] = PaneRect(App, Pane);
        pane_rect PR = PaneRect(App, Pane);
        const tab* PaneTab = App.PaneTab[Pane];
        // The home screen takes clicks itself; a program gets them as in HandleInput.
        bool32 Taken = !PaneTab || !PaneTab->ViewImage.empty() || (PaneTab->Term.MouseMode && PaneTab->ScrollOffset == 0 && !PaneTab->Exited);
        if(Taken && !App.MouseCaptureOff) { F.PaneMouseTaken |= 1u << Pane; } // capture off: every drag selects
        bool32 Focused = (Pane == App.Focus);
        s32 Top = PR.Y;
        s32 Left = PR.X;
        s32 PW = PR.W;
        s32 Right = Left + PW; // clip for text inside the pane

        if(PaneTab && !PaneTab->ViewImage.empty())
        {
            // --- an image from `clod view`, fitted to the pane above a caption row: the window draws it
            // when it can (clodw), else half blocks, two pixels a cell (top in the glyph's color, bottom
            // in the background) ---
            s32 ImgH = Max(PR.H - 1, 1);
            u32 VW = PaneTab->ViewW;
            u32 VH = PaneTab->ViewH;
            if(App.WindowDrawsImages)
            {
                if(!F.ImagePane) { F.ImagePane = Pane + 1; F.ImagePath = PaneTab->ViewImage; }
            }
            else if(VW && VH && PaneTab->ViewPixels.size() == (size_t)VW * VH)
            {
                double Scale = fmin(PW / (double)VW, ImgH * 2 / (double)VH);
                s32 DW = Max(1, (s32)(VW * Scale));
                s32 DH = Max(1, (s32)(VH * Scale));
                s32 OX = (PW - DW) / 2;
                s32 OY = (ImgH * 2 - DH) / 2;
                for(s32 R = 0; R < ImgH; ++R)
                {
                    for(s32 X = 0; X < PW; ++X)
                    {
                        u32 Px[2] = {};
                        bool32 Opaque[2] = {};
                        for(s32 Half = 0; Half < 2; ++Half)
                        {
                            s32 Sx = X - OX;
                            s32 Sy = R * 2 + Half - OY;
                            if(Sx < 0 || Sy < 0 || Sx >= DW || Sy >= DH) { continue; }
                            u32 P = PaneTab->ViewPixels[(size_t)((u32)Sy * VH / DH) * VW + (u32)Sx * VW / DW];
                            Opaque[Half] = (P >> 24) >= 128;
                            Px[Half] = Rgb((P >> 16) & 255, (P >> 8) & 255, P & 255);
                        }
                        cell C = {};
                        if(Opaque[0]) { C = Style(Px[0], Opaque[1] ? Px[1] : 0, 0); C.Codepoint = 0x2580; } // ▀
                        else if(Opaque[1]) { C = Style(Px[1], 0, 0); C.Codepoint = 0x2584; } // ▄
                        else { continue; }
                        F.Cells[(size_t)(Top + R) * W + Left + X] = C;
                    }
                }
            }
            std::string Name = PaneTab->ViewImage.substr(PaneTab->ViewImage.find_last_of("\\/") + 1);
            std::string Size = std::to_string(PaneTab->ViewFullW) + "\xC3\x97" + std::to_string(PaneTab->ViewFullH); // ×
            s32 CY = Top + PR.H - 1;
            FillRow(F, CY, Left, Right, Bar);
            s32 CX = PutText(F, Left + 1, CY, TruncateText(Name, Max(PW - 30, 8)), Style(Ui(UI_BRIGHT), Ui(UI_BAR), ATTR_BOLD), Right);
            CX = PutText(F, CX + 2, CY, Size, Bar, Right);
            if(PaneTab->ViewCount > 1)
            {
                CX = PutText(F, CX + 3, CY, std::to_string(PaneTab->ViewIndex + 1) + "/" + std::to_string(PaneTab->ViewCount), Bar, Right);
                CX = PutText(F, CX + 3, CY, "\xE2\x86\x90\xE2\x86\x92 next", BarDim, Right); // ←→
            }
            PutText(F, CX + 3, CY, "any other key closes", BarDim, Right);
            continue;
        }

        if(PaneTab)
        {
            // --- tab content: screen and scrollback, clipped to the pane ---
            const vt_terminal& T = PaneTab->Term;
            const std::vector<cell>& Screen = T.Screens[T.AltActive ? 1 : 0];
            s32 SbCount = T.AltActive ? 0 : (s32)T.Scrollback.size();
            s32 Offset = Clamp(PaneTab->ScrollOffset, 0, SbCount);
            s32 First = SbCount - Offset;
            for(s32 R = 0; R < PR.H; ++R)
            {
                s32 Line = First + R;
                const cell* Src = 0;
                s32 Len = 0;
                if(Line < SbCount)
                {
                    const std::vector<cell>& L = T.Scrollback[Line];
                    Src = L.data();
                    Len = (s32)L.size();
                }
                else if(Line - SbCount < T.Rows)
                {
                    Src = Screen.data() + (size_t)(Line - SbCount) * T.Cols;
                    Len = T.Cols;
                }
                s32 N = Min(Len, PW);
                if(N <= 0) { continue; }
                cell* Dst = F.Cells.data() + (size_t)(Top + R) * W + Left;
                memcpy(Dst, Src, N * sizeof(cell));
                if(Dst[N - 1].Flags & CELL_WIDE) { Dst[N - 1] = cell{}; }
            }

            if(Focused && Offset == 0 && !T.CursorHidden && !PaneTab->Exited && App.Prompt != PROMPT_NEW_FOLDER &&
               T.CursorY < PR.H && T.CursorX < PW)
            {
                F.CursorHidden = false;
                F.CursorShape = T.CursorShape;
                F.CursorX = Left + T.CursorX;
                F.CursorY = Top + T.CursorY;
            }

            if(PaneTab->Exited)
            {
                cell Banner = Style(Rgb(255, 255, 255), Rgb(150, 40, 40), 0);
                FillRow(F, Top + PR.H - 1, Left, Right, Banner);
                PutText(F, Left + 1, Top + PR.H - 1, "cmd not started   Enter try again   " + PrefixName(App.PrefixKey) + " x close", Banner, Right);
            }
            continue;
        }

        // --- home screen: the running tabs in sections, keys for new ones, then the Claude Code sessions ---
        std::vector<s32> Rows = HomeRows(App);
        s32 Bottom = Top + PR.H; // rows below belong to other panes or the command bar
        cell KeyText = Style(Ui(UI_BRIGHT), 0, ATTR_BOLD);
        if(Rows.empty()) { PutText(F, Left + 1, Top, "Nothing running yet.", Dim, Right); }
        s32 NameX = Left + 5;
        s32 NameW = Clamp(PW / 3, 12, 40);
        s32 StatusX = NameX + NameW + 2;
        s32 CwdX = StatusX + 16;
        for(s32 Ri = 0, Pos = 0; Ri < (s32)Rows.size() && Top + Ri < Bottom; ++Ri)
        {
            s32 Y = Top + Ri;
            if(Rows[Ri] < 0)
            {
                // --- a section heading and how many it holds ---
                s32 Section = -1 - Rows[Ri];
                s32 Count = 0;
                for(const tab* T : App.Tabs) { Count += (TabSection(*T) == Section); }
                cell Head = Style(Section == SECTION_NEEDS_INPUT ? Ui(UI_ACCENT) : 0, 0, ATTR_BOLD);
                s32 HX = PutText(F, Left + 1, Y, SectionNames[Section], Head, Right);
                PutText(F, HX + 2, Y, std::to_string(Count), Dim, Right);
                continue;
            }
            const tab* T = App.Tabs[Rows[Ri]];
            bool32 Sel = Focused && App.Selected == Pos++; // other panes' home screens show no selection
            cell Row = Sel ? Style(0, Ui(UI_ROW), 0) : cell{};
            cell RowDim = Style(Ui(UI_DIM), Row.Bg, 0);
            FillRow(F, Y, Left, Right, Row);
            if(Sel) { PutText(F, Left + 1, Y, "\xE2\x80\xBA", Style(Ui(UI_ACCENT), Row.Bg, ATTR_BOLD), Right); } // ›
            // The number (1-9 / Alt+digit shows it) is in the accent while some pane shows the tab.
            std::string Num = std::to_string(Rows[Ri] + 1);
            bool32 OnScreen = PaneShowing(App, T) >= 0;
            PutText(F, Left + 4 - TextWidth(Num), Y, Num, OnScreen ? Style(Ui(UI_ACCENT), Row.Bg, ATTR_BOLD) : RowDim, Right);
            PutText(F, NameX, Y, TruncateText(TabName(*T), NameW), Sel ? Style(0, Row.Bg, ATTR_BOLD) : Row, Right);

            // --- beyond its section: a notice (finished while unseen, ...), else what a shell runs ---
            std::string Status;
            cell StatusStyle = RowDim;
            bool32 Shell = T->SessionId.empty();
            if(T->Exited) { Status = "exited"; }
            else if(!T->Notice.empty()) { Status = "\xE2\x97\x8F " + T->Notice; StatusStyle = Style(Ui(UI_ACCENT), Row.Bg, ATTR_BOLD); } // ●
            else if(Shell && !RunningCommand(T->Term.Title).empty()) { Status = "running"; StatusStyle = Style(Ui(UI_NOTE), Row.Bg, 0); }
            else if(Shell && T->Activity) { Status = "\xE2\x97\x8F new output"; StatusStyle = Style(Ui(UI_ACCENT), Row.Bg, 0); } // ●
            else if(Shell) { Status = "at prompt"; }
            PutText(F, StatusX, Y, TruncateText(Status, 15), StatusStyle, Right);
            // On another page: says which, before the folder (Enter moves the tab here).
            s32 OnPage = OtherPageShowing(App, T);
            std::string Where = (OnPage >= 0 ? "page " + std::to_string(OnPage + 1) + " \xC2\xB7 " : std::string()) + T->Cwd; // ·
            PutText(F, CwdX, Y, TruncateText(Where, Right - CwdX - 1), RowDim, Right);
        }
        {
            static const char* NewKeys[] = {"t", "new shell", "n", "new claude", "N", "new in project", "o", "new in folder"};
            s32 KX = Left + 5;
            s32 KY = Top + Max((s32)Rows.size(), 1);
            for(u32 I = 0; I + 1 < ArrayCount(NewKeys) && KY < Bottom; I += 2)
            {
                if(KX + TextWidth(NewKeys[I]) + 1 + TextWidth(NewKeys[I + 1]) > Right) { break; }
                KX = PutText(F, KX, KY, NewKeys[I], KeyText, Right);
                KX = PutText(F, KX, KY, std::string(" ") + NewKeys[I + 1], Dim, Right) + 3;
            }
        }
        {
            // --- the pages, on the row under those keys: numbers (the current one in the accent),
            // then their keys ---
            s32 GY = Top + Max((s32)Rows.size(), 1) + 1;
            if(GY < Bottom)
            {
                s32 GX = PutText(F, Left + 1, GY, "Pages", Style(0, 0, ATTR_BOLD), Right) + 2;
                for(s32 G = 0; G < PageCount(App); ++G)
                {
                    GX = PutText(F, GX, GY, " " + std::to_string(G + 1) + " ", G == App.Page ? BarActive : BarShown, Right) + 1;
                }
                static const char* PageKeys[] = {"P", "new page", "X", "close page", "[ ]", "switch", "Alt+PgUp/PgDn", "anywhere"};
                GX += 2;
                for(u32 I = 0; I + 1 < ArrayCount(PageKeys); I += 2)
                {
                    if(GX + TextWidth(PageKeys[I]) + 1 + TextWidth(PageKeys[I + 1]) > Right) { break; }
                    GX = PutText(F, GX, GY, PageKeys[I], KeyText, Right);
                    GX = PutText(F, GX, GY, std::string(" ") + PageKeys[I + 1], Dim, Right) + 3;
                }
            }
        }

        s32 ST = Top + HomeSessionsRow(App);
        if(ST + 3 > Bottom) { continue; } // no room for the sessions list
        std::vector<s32> Filtered = FilterSessions(App);
        s32 HX = PutText(F, Left + 1, ST, "Claude Code sessions", Style(0, 0, ATTR_BOLD), Right);
        PutText(F, HX + 2, ST, std::to_string(Filtered.size()) + (App.Filter.empty() ? "" : " of " + std::to_string(App.Sessions.size())), Dim, Right);
        std::string Launch = "new ones start in " + HomeCwd(App);
        if(Right - (HX + 16) >= TextWidth(Launch) + 1) { PutText(F, Right - TextWidth(Launch) - 1, ST, Launch, Dim, Right); }

        if(App.FilterEditing || !App.Filter.empty())
        {
            s32 FX = PutText(F, Left + 1, ST + 1, "/ ", Accent, Right);
            FX = PutText(F, FX, ST + 1, App.Filter, cell{}, Right);
            if(Focused && App.FilterEditing && FX < Right)
            {
                F.CursorHidden = false;
                F.CursorX = FX;
                F.CursorY = ST + 1;
            }
        }
        else
        {
            PutText(F, Left + 1, ST + 1, "/ to filter", Dim, Right);
        }

        const s32 AgeW = 5;
        const s32 ProjW = 18;
        s32 ProjX = Left + 4 + AgeW + 1;
        s32 TitleX = ProjX + ProjW + 1;
        PutText(F, Left + 4, ST + 2, "  AGE", Dim, Right);
        PutText(F, ProjX, ST + 2, "PROJECT", Dim, Right);
        PutText(F, TitleX, ST + 2, "TITLE", Dim, Right);

        s32 ListRows = HomeListRows(App, PR.H);
        s32 ListY = Top + HomeListRow(App);
        s32 Scroll = Clamp(App.ListScroll, 0, Max(0, (s32)Filtered.size() - ListRows));
        if(Filtered.empty())
        {
            PutText(F, Left + 4, ListY, App.Sessions.empty() ? "No sessions found. Press n to start one." : "No matches.", Dim, Right);
        }
        for(s32 R = 0; R < ListRows && Scroll + R < (s32)Filtered.size() && ListY + R < Top + PR.H; ++R)
        {
            s32 Index = Scroll + R;
            const session_info& S = App.Sessions[Filtered[Index]];
            s32 Y = ListY + R;
            bool32 Sel = Focused && (s32)App.Tabs.size() + Index == App.Selected;
            cell Row = Sel ? Style(0, Ui(UI_ROW), 0) : cell{};
            cell RowDim = Style(Ui(UI_DIM), Row.Bg, 0);
            FillRow(F, Y, Left, Right, Row);

            if(Sel) { PutText(F, Left + 1, Y, "\xE2\x80\xBA", Style(Ui(UI_ACCENT), Row.Bg, ATTR_BOLD), Right); } // ›
            s32 Dummy = 0;
            if(FindTabBySession(App, S.Id, &Dummy)) { PutText(F, Left + 2, Y, "\xE2\x97\x8F", Style(Ui(UI_LIVE), Row.Bg, 0), Right); } // ●

            // --- age, right aligned ---
            s64 Secs = NowFiletime > S.ModifiedTime ? (s64)((NowFiletime - S.ModifiedTime) / 10000000ull) : 0;
            char Age[32];
            if(Secs < 60) { snprintf(Age, sizeof(Age), "now"); }
            else if(Secs < 3600) { snprintf(Age, sizeof(Age), "%dm", (int)(Secs / 60)); }
            else if(Secs < 86400) { snprintf(Age, sizeof(Age), "%dh", (int)(Secs / 3600)); }
            else if(Secs < 86400 * 60) { snprintf(Age, sizeof(Age), "%dd", (int)(Secs / 86400)); }
            else { snprintf(Age, sizeof(Age), "%dmo", (int)(Secs / (86400 * 30))); }
            PutText(F, Left + 4 + AgeW - (s32)strlen(Age), Y, Age, RowDim, Right);

            PutText(F, ProjX, Y, TruncateText(S.Project, ProjW), Style(Ui(UI_PROJECT), Row.Bg, 0), Right);
            PutText(F, TitleX, Y, TruncateText(S.Title.empty() ? "(no prompt)" : S.Title, Right - TitleX - 1), Sel ? Style(0, Row.Bg, ATTR_BOLD) : Row, Right);
        }
    }

    // --- separator lines between panes; the ones bordering the focused pane are orange ---
    if(App.Layout != LAYOUT_SINGLE)
    {
        pane_rect Focus = PaneRect(App, App.Focus);
        s32 SplitX = PaneRect(App, 0).W; // the vertical line's column
        s32 ContentH = ContentRows(H);
        s32 SplitY = 1 + Max(1, (ContentH - 1) / 2); // the horizontal line's row
        bool32 FullWidthRule = (App.Layout == LAYOUT_GRID);
        bool32 RightRule = (App.Layout == LAYOUT_MAIN);
        for(s32 Y = 1; Y <= ContentH; ++Y)
        {
            for(s32 Sx = 0; Sx < W; ++Sx)
            {
                bool32 OnV = (Sx == SplitX);
                bool32 OnH = (Y == SplitY) && (FullWidthRule || (RightRule && Sx > SplitX));
                if(!OnV && !OnH) { continue; }
                // Box drawing: plain lines, a cross where both meet, a tee where Main's rule joins.
                u32 Cp = OnV ? 0x2502 : 0x2500; // │ ─
                if(OnV && OnH) { Cp = 0x253C; } // ┼
                if(OnV && !OnH && Y == SplitY && RightRule) { Cp = 0x251C; } // ├
                bool32 Edge = (Sx == Focus.X - 1 || Sx == Focus.X + Focus.W) && Y >= Focus.Y - 1 && Y <= Focus.Y + Focus.H;
                Edge = Edge || ((Y == Focus.Y - 1 || Y == Focus.Y + Focus.H) && Sx >= Focus.X - 1 && Sx <= Focus.X + Focus.W);
                cell C = Style(Ui(Edge && !App.WindowUnfocused ? UI_ACCENT : UI_RULE), 0, 0);
                C.Codepoint = Cp;
                F.Cells[(size_t)Y * W + Sx] = C;
            }
        }

        // --- the names of panes below the top row, on the rule above each ---
        for(s32 Pane = 0; Pane < PaneCount(App.Layout); ++Pane)
        {
            pane_rect PR = PaneRect(App, Pane);
            if(PR.Y == 1) { continue; }
            const tab* T = App.PaneTab[Pane];
            std::string Name = T ? TabName(*T) : std::string("Home");
            PutText(F, PR.X + 1, PR.Y - 1, " " + TruncateText(Name, Max(PR.W - 4, 1)) + " ", Pane == App.Focus ? BarActive : BarShown, PR.X + PR.W);
        }
    }

    // --- tab switcher: for a moment after each Ctrl+Tab / Ctrl+PgUp/PgDn, a box in the focused pane
    // lists the tabs the cycle goes through (those no other pane shows), the one now here in the accent ---
    if(NowMs < App.CycleShownUntil)
    {
        pane_rect PR = PaneRect(App, App.Focus);
        const tab* Here = App.PaneTab[App.Focus];
        std::vector<const tab*> Ring;
        s32 NameW = 0;
        s32 Current = 0;
        for(const tab* T : App.Tabs)
        {
            s32 Shown = PaneShowing(App, T);
            if(Shown >= 0 && Shown != App.Focus) { continue; }
            if(T == Here) { Current = (s32)Ring.size(); }
            Ring.push_back(T);
            NameW = Max(NameW, TextWidth(TabName(*T)));
        }
        s32 BoxW = Min(NameW + 9, PR.W - 2);
        s32 BoxH = Min((s32)Ring.size() + 2, PR.H);
        if(BoxW >= 12 && BoxH >= 3)
        {
            s32 X0 = PR.X + (PR.W - BoxW) / 2;
            s32 Y0 = PR.Y + (PR.H - BoxH) / 2;
            s32 X1 = X0 + BoxW;
            cell Box = Style(Ui(UI_BAR_TEXT), Ui(UI_BAR), 0);
            cell BoxDim = Style(Ui(UI_DIM), Ui(UI_BAR), 0);
            for(s32 R = 0; R < BoxH; ++R) { FillRow(F, Y0 + R, X0, X1, Box); }
            PutText(F, X0 + 2, Y0, "Tabs", BoxDim, X1);
            // A long list scrolls so the current tab stays in view.
            s32 Rows = BoxH - 2;
            s32 First = Clamp(Current - Rows / 2, 0, Max(0, (s32)Ring.size() - Rows));
            for(s32 R = 0; R < Rows && First + R < (s32)Ring.size(); ++R)
            {
                const tab* T = Ring[First + R];
                cell S = (T == Here) ? BarActive : Box;
                FillRow(F, Y0 + 1 + R, X0 + 1, X1 - 1, S);
                std::string Num = std::to_string(TabIndex(App, T));
                PutText(F, X0 + 4 - TextWidth(Num), Y0 + 1 + R, Num, (T == Here) ? S : BoxDim, X1 - 1);
                PutText(F, X0 + 6, Y0 + 1 + R, TruncateText(TabName(*T), BoxW - 8), S, X1 - 1);
            }
            F.CursorHidden = true;
        }
    }
}

// --- rendering: diff two frames into VT output ---

static void AppendColor(std::string& Out, u32 Color, bool32 Background)
{
    char Buf[32];
    if(COLOR_TAG(Color) == COLOR_UI) { Color = UiDefault[Min((s32)(Color & 0xFF), UI_COUNT - 1)]; }
    u32 Tag = COLOR_TAG(Color);
    if(Tag == COLOR_PALETTE)
    {
        u32 I = Color & 0xFF;
        if(I < 8) { snprintf(Buf, sizeof(Buf), ";%u", (Background ? 40 : 30) + I); }
        else if(I < 16) { snprintf(Buf, sizeof(Buf), ";%u", (Background ? 100 : 90) + I - 8); }
        else { snprintf(Buf, sizeof(Buf), ";%u;5;%u", Background ? 48 : 38, I); }
        Out += Buf;
    }
    else if(Tag == COLOR_RGB)
    {
        snprintf(Buf, sizeof(Buf), ";%u;2;%u;%u;%u", Background ? 48 : 38, (Color >> 16) & 0xFF, (Color >> 8) & 0xFF, Color & 0xFF);
        Out += Buf;
    }
}

// The last MaxLines of a tab's main screen (scrollback, then the screen down to its last non-blank
// row) as text with SGR colors and CRLF line ends, ending with a note line. Replayed by cmd's
// `type` when a shell tab comes back after the server restarts. Empty when there's nothing to keep.
static std::string HistoryText(const vt_terminal& T, s32 MaxLines)
{
    // --- the lines, oldest first ---
    std::vector<const cell*> Lines;
    std::vector<s32> Lens;
    for(const std::vector<cell>& L : T.Scrollback) { Lines.push_back(L.data()); Lens.push_back((s32)L.size()); }
    for(s32 Y = 0; Y < T.Rows; ++Y) { Lines.push_back(T.Screens[0].data() + (size_t)Y * T.Cols); Lens.push_back(T.Cols); }
    // Trailing blanks (and blank rows at the end) aren't worth keeping.
    for(size_t I = 0; I < Lines.size(); ++I)
    {
        while(Lens[I] > 0 && (Lines[I][Lens[I] - 1].Codepoint == 0 || Lines[I][Lens[I] - 1].Codepoint == ' ') &&
              !Lines[I][Lens[I] - 1].Bg) { Lens[I] -= 1; }
    }
    s32 End = (s32)Lines.size();
    while(End > 0 && Lens[End - 1] == 0) { End -= 1; }
    s32 Begin = Max(0, End - MaxLines);
    if(Begin >= End) { return std::string(); }

    // --- as text: an SGR whenever the style changes ---
    std::string Out;
    for(s32 I = Begin; I < End; ++I)
    {
        cell Pen = {};
        for(s32 X = 0; X < Lens[I]; ++X)
        {
            cell C = Lines[I][X];
            if(C.Flags & CELL_WIDE_CONT) { continue; }
            if(Pen.Fg != C.Fg || Pen.Bg != C.Bg || Pen.Attr != C.Attr)
            {
                Out += "\x1b[0";
                if(C.Attr & ATTR_BOLD) { Out += ";1"; }
                if(C.Attr & ATTR_DIM) { Out += ";2"; }
                if(C.Attr & ATTR_ITALIC) { Out += ";3"; }
                if(C.Attr & ATTR_UNDERLINE) { Out += ";4"; }
                if(C.Attr & ATTR_INVERSE) { Out += ";7"; }
                if(C.Attr & ATTR_STRIKE) { Out += ";9"; }
                AppendColor(Out, C.Fg, false);
                AppendColor(Out, C.Bg, true);
                Out += "m";
                Pen = C;
            }
            AppendUtf8(Out, C.Codepoint >= 0x20 ? C.Codepoint : ' ');
        }
        Out += "\x1b[0m\r\n";
    }
    Out += "\x1b[0;90m\xE2\x94\x80\xE2\x94\x80 earlier output, before clod restarted \xE2\x94\x80\xE2\x94\x80\x1b[0m\r\n"; // ── ... ──
    return Out;
}

// True for glyphs some terminals draw at a different width than conhost's table says (symbols,
// emoji, dingbats). Plain text, box drawing, arrows and math operators are the same everywhere.
static bool32 OuterWidthMayDiffer(u32 Cp)
{
    if(Cp < 0x1100) { return false; }
    if(Cp >= 0x2010 && Cp <= 0x205E) { return false; } // general punctuation
    if(Cp >= 0x2190 && Cp <= 0x22FF) { return false; } // arrows, math operators
    if(Cp >= 0x2500 && Cp <= 0x259F) { return false; } // box drawing, block elements
    if(Cp >= 0x3000 && Cp <= 0x9FFF) { return false; } // CJK
    if(Cp >= 0xAC00 && Cp <= 0xD7A3) { return false; } // Hangul
    return true;
}

// Returns false when nothing visible changed. OuterWide: sorted codepoints the window's console draws
// 2 cells wide where CharWidth says 1 (a classic console sizes ambiguous-width characters by font).
static bool32 RenderFrame(std::string& Out, const frame& Prev, const frame& Next, bool32 Full, const std::vector<u32>& OuterWide)
{
    if(Prev.Width != Next.Width || Prev.Height != Next.Height || Prev.Cells.size() != Next.Cells.size()) { Full = true; }

    std::string Body;
    if(Full) { Body += "\x1b[0m\x1b[2J"; }

    s32 W = Next.Width;
    cell Pen = {};
    bool32 PenKnown = false;
    s32 CurX = -1;
    s32 CurY = -1;
    char Buf[64];

    for(s32 Y = 0; Y < Next.Height; ++Y)
    {
        const cell* Row = Next.Cells.data() + (size_t)Y * W;
        const cell* Old = Full ? 0 : Prev.Cells.data() + (size_t)Y * W;
        bool32 ForceNext = false; // the previous glyph may have spilled into this cell on the outer terminal
        for(s32 X = 0; X < W; ++X)
        {
            cell C = Row[X];
            bool32 Orphan = (C.Flags & CELL_WIDE_CONT) && (X == 0 || !(Row[X - 1].Flags & CELL_WIDE));
            if((C.Flags & CELL_WIDE_CONT) && !Orphan) { continue; }
            if(Orphan) { C.Flags = 0; C.Codepoint = 0; }
            bool32 Wide = (C.Flags & CELL_WIDE) && X + 1 < W;
            // Narrow base + U+FE0F is two cells in conhost and one two-cell emoji in grapheme-aware
            // terminals: send them together and both agree.
            bool32 EmojiPair = !(C.Flags & CELL_WIDE) && X + 1 < W && Row[X + 1].Codepoint == 0xFE0F &&
                               C.Codepoint >= 0x20 && !IsClusterExtender(C.Codepoint);
            // A glyph the window draws 2 cells wide may cover the next cell when that one is blank
            // in the same colors (the usual case: "● text", " — "); otherwise it gets a narrow stand-in.
            bool32 OuterWider = !(C.Flags & CELL_WIDE) && !EmojiPair && std::binary_search(OuterWide.begin(), OuterWide.end(), C.Codepoint);
            bool32 Cover = OuterWider && X + 1 < W && (Row[X + 1].Codepoint == 0 || Row[X + 1].Codepoint == ' ') && !Row[X + 1].Flags &&
                           Row[X + 1].Bg == C.Bg && (Row[X + 1].Attr & ATTR_INVERSE) == (C.Attr & ATTR_INVERSE);

            bool32 Changed = Full || ForceNext || !CellEqual(Row[X], Old[X]) ||
                             ((Wide || EmojiPair || OuterWider) && X + 1 < W && !CellEqual(Row[X + 1], Old[X + 1]));
            ForceNext = false;
            if(!Changed) { continue; }

            if(CurX != X || CurY != Y)
            {
                snprintf(Buf, sizeof(Buf), "\x1b[%d;%dH", Y + 1, X + 1);
                Body += Buf;
            }
            if(!PenKnown || Pen.Fg != C.Fg || Pen.Bg != C.Bg || Pen.Attr != C.Attr)
            {
                Body += "\x1b[0";
                if(C.Attr & ATTR_BOLD) { Body += ";1"; }
                if(C.Attr & ATTR_DIM) { Body += ";2"; }
                if(C.Attr & ATTR_ITALIC) { Body += ";3"; }
                if(C.Attr & ATTR_UNDERLINE) { Body += ";4"; }
                if(C.Attr & ATTR_BLINK) { Body += ";5"; }
                if(C.Attr & ATTR_INVERSE) { Body += ";7"; }
                if(C.Attr & ATTR_HIDDEN) { Body += ";8"; }
                if(C.Attr & ATTR_STRIKE) { Body += ";9"; }
                AppendColor(Body, C.Fg, false);
                AppendColor(Body, C.Bg, true);
                Body += "m";
                Pen = C;
                PenKnown = true;
            }

            // --- glyph, substituted where the outer terminal would lay it out differently ---
            u32 Cp = C.Codepoint;
            bool32 Uncertain = false;
            if(Cp < 0x20 || ((C.Flags & CELL_WIDE) && !Wide) || IsClusterExtender(Cp))
            {
                // Joiners, combining marks etc. would merge into the neighbour and shift the row.
                Body += ' ';
            }
            else if(IsRegionalIndicator(Cp))
            {
                // Regional indicators pair up into one flag glyph; spell them as letters instead.
                Body += (char)('A' + (Cp - 0x1F1E6));
                if(Wide) { Body += ' '; }
            }
            else if(OuterWider && !Cover)
            {
                // No room for the window's 2-cell glyph: a narrow stand-in keeps the row aligned.
                char Narrow = '*';
                if(Cp == 0x2014 || Cp == 0x2015) { Narrow = '-'; }
                else if(Cp >= 0x2024 && Cp <= 0x2026) { Narrow = '.'; }
                else if(Cp == 0x2190 || Cp == 0x21D0) { Narrow = '<'; }
                else if(Cp == 0x2192 || Cp == 0x21D2) { Narrow = '>'; }
                else if(Cp == 0x2191 || Cp == 0x21D1) { Narrow = '^'; }
                else if(Cp == 0x2193 || Cp == 0x21D3) { Narrow = 'v'; }
                else if(Cp == 0x2194 || Cp == 0x21D4) { Narrow = '-'; }
                else if(Cp == 0x2195) { Narrow = '|'; }
                else if(Cp >= 0x2460 && Cp <= 0x2468) { Narrow = (char)('1' + (Cp - 0x2460)); } // ①..⑨
                Body += Narrow;
            }
            else
            {
                AppendUtf8(Body, Cp);
                if(EmojiPair) { AppendUtf8(Body, 0xFE0F); }
                Uncertain = (EmojiPair || OuterWidthMayDiffer(Cp)) && !Cover;
            }

            // After a glyph whose drawn width isn't certain, re-sync the cursor and repaint the next
            // cell too, in case it got covered (new glyph) or blanked (overwritten old glyph). The
            // same goes for an old glyph the window drew 2 wide. A covering glyph re-syncs too, so the
            // rest of the row lands right even if the window's measured width was wrong.
            s32 Cells = (Wide || EmojiPair) ? 2 : 1;
            CurX = (Uncertain || Cover) ? -1 : X + Cells;
            CurY = Y;
            if(CurX >= W) { CurX = -1; }
            bool32 OldWider = Old && std::binary_search(OuterWide.begin(), OuterWide.end(), Old[X].Codepoint);
            ForceNext = Uncertain || (Old && OuterWidthMayDiffer(Old[X].Codepoint)) || (OldWider && !Cover);
            if(EmojiPair || Cover) { X += 1; }
        }
    }

    bool32 ShapeChanged = Full || Prev.CursorShape != Next.CursorShape;
    bool32 CursorChanged = ShapeChanged || Prev.CursorHidden != Next.CursorHidden || Prev.CursorX != Next.CursorX || Prev.CursorY != Next.CursorY;
    if(Body.empty() && !CursorChanged) { return false; }

    Out += "\x1b[?2026h\x1b[?25l";
    Out += Body;
    Out += "\x1b[0m";
    if(ShapeChanged)
    {
        snprintf(Buf, sizeof(Buf), "\x1b[%d q", Next.CursorShape);
        Out += Buf;
    }
    if(!Next.CursorHidden)
    {
        snprintf(Buf, sizeof(Buf), "\x1b[%d;%dH\x1b[?25h", Next.CursorY + 1, Next.CursorX + 1);
        Out += Buf;
    }
    Out += "\x1b[?2026l";
    return true;
}

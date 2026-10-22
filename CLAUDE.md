# CLAUDE.md — clod project

Project-specific architecture, rules and gotchas. This complements the global
`~/.claude/CLAUDE.md` (functional-first, simplicity, ZII, no-helper-until-second-use,
side-effects-at-edges); those rules apply here, match them.

## What this project is

A Windows terminal tab manager for Claude Code sessions, in C++ (MSVC unity build, `build.bat`).
Each tab is a real `cmd.exe` (running the repo's `shell\init.bat`, with clink) in its own ConPTY, and is a
claude tab while `claude.exe` runs inside that cmd. clod emulates each tab's screen and composites the active one between a tab bar (top row) and a command bar (bottom row), tmux-style.
It is meant to run in any Windows terminal (Windows Terminal, conhost/cmd.exe, Git Bash in WT).

## Process architecture

- `clod --server` (hidden, no console) owns everything: ConPTY children, `vt_terminal` emulators,
  scrollback, `app_state`, composition, diff rendering and the tray icon.
- `clod` is a thin client. It relays console input to the server and server output to the
  console over a per-user named pipe (`code/clod_ipc.cpp`). Closing the window only kills the
  client. The first client spawns the server.
- `clodw.exe` (`code/clodw*.cpp`) is clod's own window, a second kind of client. It attaches
  with `ATTACH_CELLS` and gets `MSG_CELLS` (the composed cell grid: changed rows, cursor, title,
  mouse mode; `EncodeCells` in `clod_server.cpp`) instead of VT, so no other terminal re-measures
  text. It draws with one Direct3D 11 pixel shader over a cell buffer + glyph atlas; glyphs are
  drawn on first use with Direct2D/DirectWrite (font fallback, color emoji, scaled to fit their
  cells). Input goes back as VT bytes, so `HandleInput` serves both clients. Font and size: the
  window menu's Font... (any installed font) or `~\.clod\settings.txt` (`font`, `font_size`).
  Written from scratch after refterm's technique; refterm is GPL-2.0, so copy none of its code.
- clodw text (stage 2): cells come from ConPTY one codepoint each, so clodw groups a base cell
  with the cells that join it (`IsClusterExtender`, what follows a ZWJ, a flag's second letter)
  and draws the cluster as one glyph across all of them (up to `GLYPH_MAX_CELLS`), leaving blank
  the cells ConPTY gave the parts. Color clusters (FE0F, emoji) use Segoe UI Emoji first, else
  the main font's own monochrome glyph wins. A one-cell glyph wider than its cell spills into a
  following blank cell of the same style; otherwise it's squeezed sideways (full height). Flags
  stay letters: Windows ships no flag emoji.
- clodw input and clipboard (stage 3, all client-side):
  - Selection: a plain drag in a pane whose program doesn't take the mouse, Shift+drag in any
    pane; double-click selects a word, triple-click the pane's row. It stays in the pane it
    started in (pane rects and the `PaneMouseTaken` bits come in the `MSG_CELLS` header;
    `MouseMode` there is the server's own wish, always on, so it can't tell). A plain selection
    still reports its press/release so the server focuses the pane. Ctrl+C copies when there's a
    selection (else it's ^C for the program), Ctrl+Shift+C copies, typing or clicking clears it.
    A finished selection is copied at once (`copy_on_select = off` in settings.txt stops that);
    right-click pastes and is never reported to programs (Claude's fullscreen has no use for it).
  - Zoom: Ctrl+= / Ctrl+- / Ctrl+wheel change the font size for this window only; Ctrl+0 goes
    back to settings.txt's size.
  - Turn finished (and OSC 9/777, bell): the tray balloon fires when the tab isn't on screen or
    the client window isn't in the foreground; for clodw the server also sends BEL, which flashes
    its taskbar button while it's in the background.
  - Clink's `terminal.mouse_input` must stay `off` in the default `clink_settings`: its `auto`
    turns on mouse input under ConPTY, which makes the shell pane take the mouse.
  - The window's place (normal rect + maximized) is saved on exit to
    `%LOCALAPPDATA%\clod\window<instance>.txt` and restored if still on a monitor; not for `--test`.
  - OSC 52 from a program (how Claude Code copies) arrives in `MSG_OUTPUT` and goes to the
    clipboard. Ctrl+click opens an http/https/file URL found in the text under the pointer.
  - IME: composition and candidate windows are placed at the cursor in the window's font.
  - Cursor shape: the emulator keeps DECSCUSR (`vt_terminal::CursorShape`); the frame carries it
    (console clients get it re-sent, clodw draws block/underline/bar and blinks the odd shapes).
  - With `clodw.exe` next to `clod.exe`, the tray (`LaunchClientWindow`) opens clodw.
- Bump `CLOD_PROTOCOL_VERSION` (`clod_ipc.cpp`) whenever a message's layout or meaning changes:
  the server turns away an attach from another build with a "quit clod from the tray" message.
- A second client takes over from the first. The open tab list is persisted in
  `%LOCALAPPDATA%\clod\tabs.txt`, so a crash or reboot reopens tabs with `--resume`. Shell tabs
  come back as a fresh cmd in the folder they started in.
- `code/clod_app.cpp` is the pure core. Input becomes `app_command`s that the server executes;
  `Compose` turns state into a cell grid; `RenderFrame` diffs two grids into VT bytes.
  Keep OS calls out of it.

## Panes

- The area between the bars shows a layout of panes (`app_state::Layout`: single, side by side,
  2x2 grid, main + 2; prefix `z`/`v`/`g`/`b`; Alt+Q / Alt+Shift+Q cycle the focus, prefix
  arrows move it by direction; Ctrl+arrows stay with the program as word jumps). Each pane shows a tab
  or the sessions page (`PaneTab[]`, null = sessions page); **a tab is never in two panes**.
  Picking a tab another pane shows (its number, a click) focuses that pane (`SwitchTab`);
  cycling (Ctrl+PgUp/PgDn, prefix `n`/`p`) skips such tabs and never moves the focus (`CycleTab`). `App.Active` is always the
  focused pane's tab, so code that uses `ActiveTab` (keys, prompts, the command bar) still works.
- Changing layout keeps the focused tab on screen and fills new panes with tabs nothing shows
  (`SetLayout`); closing a tab gives its pane the next unshown one (`ForgetTab`).
- Each visible tab's terminal and ConPTY are sized to its pane; off-screen tabs take the focused
  pane's size once a window resize settles. Mouse events go to the pane under the pointer (a drag
  stays in its pane) in pane-relative coordinates; a click focuses the pane.
- Layouts are remembered per monitor: clients send `MSG_MONITOR` (`MonitorKey`: device name and
  resolution) on attach and when the window moves; the server switches to that monitor's last
  layout. The layout, panes and per-monitor layouts are saved in `tabs.txt`.
- clodw's keep-active (`keep_active = off` disables): while clodw is the foreground window, it
  nudges the mouse a pixel and back whenever Windows' idle time passes 2 minutes, so Slack and
  the like don't go "away". Its light is the bar's last cell (green = keeping active), which the
  server leaves blank.

## Tabs: cmd hosting claude

- Opening a session starts
  `cmd /k "call init.bat & clink inject & cd /d <cwd> & claude --resume|--session-id <id>"`, so claude
  exiting drops to the shell prompt (Claude Code prints its own `claude --resume <id>` hint).
  `exit` in cmd closes the tab.
- Clink is vendored unmodified in `vendor/clink` (GPL-3.0, like clod; version and source link in
  `vendor/clink.SOURCE`). `build.bat` copies it to `build\clink`. Clink loads its DLL from
  `%TEMP%\clink\dll_cache`, so running tabs never lock `build\clink`. To upgrade, replace
  `vendor/clink` with a new release zip and update `vendor/clink.SOURCE`.
- The shell setup belongs to the user and must stay fully editable on their machine. The repo's
  `shell/` holds only the **defaults**; `build.bat` copies them to `build\shell`. On server start,
  if `%USERPROFILE%\.clod\init.bat` is missing, every default file missing from `~\.clod` is copied
  there (never overwriting). Tabs then use only `~\.clod`: `init.bat` (run first, via `call`) and
  the clink profile `~\.clod\clink` (`inject --profile`), where clink natively reads
  `clink_settings`, `.inputrc` and Lua scripts and keeps history. Inherited `CLINK_*` variables are
  dropped so another clink setup can't leak in; `init.bat` can set its own before the inject.
  Changing a default in `shell/` doesn't reach an existing `~\.clod`; to see it, edit `~\.clod`
  or delete the file plus `init.bat` to reseed. Keep `clink.autoupdate = off` in the default
  `clink_settings`: the binaries are the vendored copy.
- Never resume a session while an older claude of it is still alive: Claude then thinks another
  live session has its name and renames this one for good. So shutdown closes the pipe first,
  closes every console and waits (5 s) for the claudes to exit on their own, and startup waits
  (15 s) for any live process registered under a session it's about to resume
  (`ReadClaudeSession` on `<ClaudeConfigDir>\sessions\*.json`).
- Shell tabs keep their recent output across a server restart: every 30 s and at shutdown the
  last 200 lines (`HistoryText`, with colors) go to `%LOCALAPPDATA%\clod\history<instance>\<id>.txt`
  as UTF-16 with a BOM; a restored shell's cmd chain starts with `type` of that file, so it shows
  above the new prompt. The id (`tab::HistoryId`) is the shell line's second field in `tabs.txt`.
  Closing a tab deletes its file; startup deletes files no tab uses.
- A shell tab is named for what it runs: cmd titles itself `<path>\cmd.exe - <command>` for
  external programs (`RunningCommand` tidies it and ignores clod's own startup steps), and the
  tab keeps the last such command (`tab::LastCommand`) once idle; before any, `cmd <folder>`.
- The server finds claude by session file, not by how it was started: about once a second it walks
  each tab's process tree and reads `<ClaudeConfigDir>\sessions\<pid>.json` (`sessionId`, `cwd`;
  `procStart` is checked against the process creation time). So a `claude` typed in a shell, `-c`,
  the resume picker and `/resume` all keep `tab::SessionId` right. No claude found = plain shell
  (`SessionId` empty); the tabs file, tab name, dedupe and tray Quit all key off that.

## Rules from the user (don't regress these)

- **Never expose a way to end every session and forget the tabs.** The tray's single "Quit"
  stops the server but keeps `tabs.txt`. It asks first only if some claude tab is mid-reply
  (`tab::TitleBusy`) or some shell tab is running a program (its cmd has a child process).
  An idle shell at its prompt or an idle claude doesn't count.
- In the clod window, `q` (sessions page) and prefix `q` only **detach**. Stop and end-all are
  not reachable from the window UI. A tab is dropped only by closing it (prefix `x`) or by `exit`
  in its cmd. Claude exiting returns the tab to its shell.
- The command prefix is `Ctrl+\` by default (`prefix = ctrl+...` in `~\.clod\settings.txt`, read on
  server start and each attach; `PrefixFromName`/`PrefixName` in `clod_app.cpp`). Pressing it twice
  sends the key itself. The command bar lists the prefix commands only while the prefix is armed
  (whole entries, dropped when they don't fit); otherwise, in a tab, its left side is empty and the
  right shows only a grey "Ctrl+\ commands" (status and scroll position take that spot when
  present). clodw adds direct shortcuts (Ctrl+Tab, Ctrl+Shift+T/N/O/S/W) that it sends as prefix +
  key, using the `PrefixKey` the server puts in each `MSG_CELLS` header.
- The top bar shows **only tabs**. Commands, the close-tab prompt, status messages and the
  scroll position all live in the bottom command bar.

## Character widths (subtle; read before touching CharWidth or RenderFrame)

- Child output reaches clod through ConPTY, which lays text out with the **system conhost**
  widths: 1 or 2 cells, never 0. FE0F, ZWJ and combining marks each take their own cell.
  `CharWidth` must match exactly. The table `code/clod_widths.h` is generated from a
  measurement (`tools/widthprobe.cpp --table` then `tools/gen_widths.py`). A mismatch shifts
  rows and leaves stale characters behind the diff renderer.
- Windows Terminal clusters graphemes differently. So `RenderFrame`:
  - emits narrow base + FE0F as one unit;
  - draws cluster extenders (ZWJ, combining, skin tones, keycap) as spaces;
  - spells regional indicators as letters;
  - re-syncs the cursor after glyphs whose outer width may differ (`OuterWidthMayDiffer`).
- A classic console window (conhost: taskbar pin, tray, double-clicked exe) sizes East Asian
  Ambiguous characters by its **font**: with e.g. Iosevka Extended, `— … ← → ● ○ ①` are 2 cells,
  while ConPTY (no font) makes them 1. Unhandled, rows shift and the diff leaves stray letters.
  So after attaching, the client measures the `code/clod_ambiguous.h` set (from
  `tools/gen_ambiguous.py`) in an off-screen buffer and sends the 2-wide ones (`MSG_WIDTHS`;
  empty in Windows Terminal). `RenderFrame` then lets such a glyph cover a following blank cell
  of the same colors, else draws a narrow stand-in (`- . < > ^ v *`), and re-syncs the cursor.
- To debug drift: `CLOD_CAPTURE=<file>` makes the client record what it writes, and
  `tools/replaycheck.exe <capture> <report>` (build with `tools\build_replaycheck.bat`) replays
  it in the console it runs in and reports the first frame leaving a stray character. Run it in
  the same kind of window as the bug, e.g. launched directly (classic console, per-exe font in
  `HKCU\Console\<exe path>`) or `wt.exe -w new --maximized`.

## Claude Code specifics

- Claude's fullscreen renderer uses the alternate screen and mouse mode 1003. clod mirrors the
  mouse mode to the outer terminal and passes wheel / Shift+PgUp through when the child is in
  the alt screen.
- Strip a parent Claude session's env markers from children (`CLAUDECODE`,
  `CLAUDE_CODE_CHILD_SESSION`, ... in `StartTab`). Otherwise the child behaves like a subagent
  and stops saving its transcript.
- Session cwd: pick the `cwd` whose mangled form (non-alphanumerics -> `-`) equals the project
  folder name. Resume needs that folder, and long sessions wander between directories.
- Title spinner `◐◑◒◓` means busy; `✳` means idle. The busy-to-idle edge is the "turn
  finished" notification.

## Building and testing

- `build.bat` builds `tools/make_icon.cpp`, generates `build\clod.ico`, compiles `code\clod.rc`
  and links. It deletes or renames a running `clod.exe` first, so it works while clod runs.
- The user keeps their real clod running. To try a build, use **`clod --test`**: a throwaway
  private server (`CLOD_EPHEMERAL`) with no tray and no tabs file, which dies with its window.
  Never tell the user to kill their clod for testing.
- Automated tests drive clod through `tools/clod_drive.exe` (a pty driver that dumps the
  screen; build with `tools\build_drive.bat`):
  - Set `CLOD_INSTANCE=<name>` for an isolated server and tabs file.
  - Set `CLOD_CLAUDE="cmd.exe /k echo"` for a fake child.
  - If a test server must be killed, pick it by CreationDate (started seconds ago). The user's
    own `--server` is usually running too.
- clodw: `build\clodw.exe --test` (own throwaway server). Check it by screenshot: PowerShell
  `PrintWindow(hwnd, dc, 2)` from a **DPI-aware** script (`SetProcessDPIAware`), then read the PNG.
  Drive it with `PostMessageW` (`WM_CHAR`, `WM_KEYDOWN`, `WM_LBUTTONDOWN`); posted mouse
  coordinates get DPI-scaled unless the sender is DPI-aware too.
- When testing with real claude, always end it with `/exit`. Killing claude during startup adds
  strikes to `fullscreenAutoDisabled` in `~/.claude.json`, and Claude then turns its fullscreen
  renderer off machine-wide (`/tui fullscreen` resets it).
- Edit C++ string escapes (`\x1b`, `\\`) with the Edit tool. Shell heredocs and inline python
  through the Bash tool mangle backslashes.

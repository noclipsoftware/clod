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
- A second client takes over from the first. The open tab list is persisted in
  `%LOCALAPPDATA%\clod\tabs.txt`, so a crash or reboot reopens tabs with `--resume`. Shell tabs
  come back as a fresh cmd in the folder they started in.
- `code/clod_app.cpp` is the pure core. Input becomes `app_command`s that the server executes;
  `Compose` turns state into a cell grid; `RenderFrame` diffs two grids into VT bytes.
  Keep OS calls out of it.

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
- In the clod window, `q` (sessions page) and `Ctrl+] q` only **detach**. Stop and end-all are
  not reachable from the window UI. A tab is dropped only by closing it (`Ctrl+] x`) or by `exit`
  in its cmd. Claude exiting returns the tab to its shell.
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
- When testing with real claude, always end it with `/exit`. Killing claude during startup adds
  strikes to `fullscreenAutoDisabled` in `~/.claude.json`, and Claude then turns its fullscreen
  renderer off machine-wide (`/tui fullscreen` resets it).
- Edit C++ string escapes (`\x1b`, `\\`) with the Edit tool. Shell heredocs and inline python
  through the Bash tool mangle backslashes.

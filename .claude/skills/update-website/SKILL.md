---
name: update-website
description: Refresh clod's landing page at noclip.gd/clod after working on clod - update the website text in CLAUDE.md, run `build content` (fresh build, both screenshots, text, icon into content\), rebuild the site, preview it, and (only with the user's go-ahead) deploy it. Use for "update the website", "push a website update", "new screenshots for the landing page", "refresh noclip.gd/clod", or when a capture in tools\screenshot breaks.
---

# Update clod's landing page (noclip.gd/clod)

clod makes the content; the site only lays it out:

- `content\` (this repo) is what the page shows: `screenshot.png` (2x2), `screenshot-shell.png`,
  `clod.png` / `clod.ico`, and `content.json` (tagline, install line, features).
- `build content` writes it: a normal build, then `tools\screenshot\capture.py` for both scenes,
  then `tools\content.py` (text from CLAUDE.md's "## Website content", icon from `build\clod.ico`).
- `C:\UnitySrc\kevin.gd` is the site: `build.py` copies `content\` into `build\clod\` and fills
  `static\clod\index.html`'s `{{clod_...}}` slots. Layout and the hero art live there.

Do the steps in order. Stop and ask when a step needs the user.

## 1. Text

"## Website content" in this repo's `CLAUDE.md` holds the tagline, the install line and the main
features list. Compare it with what changed since the last update (`git log`, new CLAUDE.md
sections) and propose changes to the user; don't change it without their OK. Keep its style: zen,
short phrases, no descriptions, only what's special about clod.

## 2. build content

Tell the user first: it builds clod, then pops up a clodw window twice (~30 s each), and they must
not type anywhere meanwhile (keys land in the captured window). If another session is editing or
building clod right now, wait for it. Then run `build content` here.

Check both screenshots by compositing them over the page's peach and reading the result:

```
python -c "from PIL import Image; im = Image.open('content/screenshot.png'); bg = Image.new('RGBA', im.size, (237,184,136,255)); Image.alpha_composite(bg, im).convert('RGB').save('preview.jpg')"
```

A broken run looks obviously wrong (single pane instead of 2x2, panes in the wrong place, a
"Directory missing" line, stray text on a prompt). See "Screenshots" below, fix, and rerun just the
failed capture (`python tools\screenshot\capture.py [--scene shell]`).

## 3. Build and preview

In `C:\UnitySrc\kevin.gd`: `python build.py` (it warns if `content\content.json` is missing). Serve
`build\` (`python -m http.server 8000 --directory build`, unless it's already running) and open
http://localhost:8000/clod/ for the user. Wait for their approval; iterate if asked.

## 4. Deploy (only when the user says so)

Publishing is public: confirm with the user right before it, every time.

- `public\` in kevin.gd is a git submodule (the GitHub Pages repo). If it's empty, run
  `git submodule update --init` there first.
- `python deploy.py` in kevin.gd pulls `public\`, builds, copies `build\` in, commits "update" and
  pushes. That's the live site.
- Then ask whether to commit the sources too: this repo (`content\`, CLAUDE.md, tools) and kevin.gd.

## Screenshots (tools\screenshot)

`capture.py` shoots `build\clodw.exe` over pure black, pure white, then black again (retried while
the two black shots differ, i.e. clod was still updating). alpha = 255 - (white - black), color =
black / alpha: see-through cells come out at clod's opacity, and the title bar, rounded corners and
shadow keep theirs. `python tools\screenshot\capture.py [--scene grid|shell] [--out <png>]
[--size WxH] [--opacity 95] [--keep-scratch]`.

- **Scenes**: `SCENES` in capture.py: the layout key (as VT bytes), the panes in clod's order (top
  left, top right, bottom left, bottom right), hidden sessions, window size, output file. A pane is a
  session (opened through the home screen's filter), the home screen, or a shell tab (`t`) with text
  typed into it (`\r` is Enter, `('alt', key)` is Alt+key). Alt+Q moves to the next pane; the focus
  ends on the home screen if one shows.
- **Isolated**: a `clodw --test` throwaway server with `USERPROFILE` at a scratch home (fresh
  `~\.clod\settings.txt`: Iosevka Clod, the opacity, theme Clod, keep_active off). The user's real
  clod, settings and tabs are never touched.
- **Fake sessions**: `CLAUDE_CONFIG_DIR` points at scratch `projects\<mangled cwd>\<id>.jsonl` files
  (the home screen's list; no real session titles on a public page). `CLOD_CLAUDE` runs
  `fake_claude.py`, which registers `sessions\<pid>.json` like Claude Code, sets a busy (◐) or idle
  (✳) title and draws a still transcript in Claude Code's look. Ids and titles must match between
  `SESSIONS` in capture.py and `SCENARIOS` in fake_claude.py.
- **Tabs that want a look**: a scene's `hidden` sessions are opened first and sent off screen with
  Ctrl+\ (0x1C); their scenario's `later` then finishes the turn or rings the bell, so the top bar
  reads `N running · ● M new` and the home screen shows "● finished" / "● needs you".
- **Fake codebase**: `meshkit\`, copied to `C:\src\meshkit` and made a git repo with fixed commits
  (`MESHKIT_HISTORY`: fixed dates and author, so hashes and `ll` dates never change) plus one
  uncommitted change (the prompt's `*`). The sessions' folders (`C:\src\game`, ...) are made too.
  All removed afterwards, `C:\src` itself only if the script made it; it refuses to run if any
  already exist.
- **Neovim**: the real nvim on PATH via a scratch `nvim.bat` (`--clean`, plus `nvim_init.lua`, which
  clears the backgrounds so the pane stays see-through and scrolls to the shown lines).

Gotchas:

- The capture follows clod: when it breaks, check recent commits for changed keys or home screen
  behavior first. Known so far:
  - Layouts switch only with Ctrl+Shift+Alt+arrows. Posted messages can't hold modifiers, so the
    script posts the VT bytes clodw would send (`ESC [1;8B` grid, `ESC [1;8A` single) in one burst.
  - On the home screen, plain letters are commands, and Esc with an empty filter means "back to the
    tab you came from". Filters are cleared with `/` plus backspaces, never Esc.
  - A session whose folder doesn't exist shows "Directory missing" in the command bar.
- JSON clod reads must be compact (`"key":"value"`, no spaces). A sessions\<pid>.json clod can't
  parse makes the tab drop to "Shells" after a grace period.
- fake_claude.py must use `os.get_terminal_size()`: `shutil`'s trusts `COLUMNS`/`LINES`, which hold
  the whole window's size, not the pane's.
- The capture brings clodw to the front: anything typed elsewhere lands in clod.
- clink's interactive completion list (Tab, Ctrl+Space) closes when the window takes the focus for
  the capture; Alt+= (`possible-completions`) prints the list instead, so it stays.
- The window title is reset to `clod` before the capture (a `--test` server titles it "clod TEST").
- Edit these files with the Edit tool: shell heredocs mangle the backslashes in Windows paths.

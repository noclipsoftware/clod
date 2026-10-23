-- clod's shell setup, run by clink inside each tab's cmd when it starts editing the first line.
-- In-process Lua, so it adds nothing to a tab's start time (doskey.exe or batch work in init.bat
-- would). Changes to PATH reach what you type in the shell, not a claude started with the tab.

-- The folder holding name on PATH, or nil.
local function on_path(name)
    for dir in (os.getenv("PATH") or ""):gmatch("[^;]+") do
        dir = dir:gsub('"', ""):gsub("[\\/]+$", "")
        if os.isfile(dir.."\\"..name) then return dir end
    end
end

local function add_to_path(dir)
    os.setenv("PATH", (os.getenv("PATH") or "")..";"..path.normalise(dir))
end

local tips = {}

-- --- unix tools (ls, grep, cat, less, ...) from Git for Windows' usr\bin ---
-- Next to the git on PATH, else the usual install folders. Appended, so Windows' own
-- find/sort/timeout and MSVC's link.exe still win.
if not on_path("ls.exe") then
    local candidates = {}
    local git = on_path("git.exe")
    if git then
        table.insert(candidates, git.."\\..\\usr\\bin")     -- <git>\cmd\git.exe
        table.insert(candidates, git.."\\..\\..\\usr\\bin") -- <git>\mingw64\bin\git.exe
    end
    for _, root in ipairs({ (os.getenv("ProgramFiles") or "").."\\Git",
                            (os.getenv("ProgramFiles(x86)") or "").."\\Git",
                            (os.getenv("LOCALAPPDATA") or "").."\\Programs\\Git",
                            (os.getenv("USERPROFILE") or "").."\\scoop\\apps\\git\\current" }) do
        table.insert(candidates, root.."\\usr\\bin")
    end
    for _, dir in ipairs(candidates) do
        if os.isfile(dir.."\\ls.exe") then
            add_to_path(dir)
            break
        end
    end
    if not on_path("ls.exe") then
        table.insert(tips, "for ls, grep, cat and other unix commands, install Git for Windows:  winget install Git.Git")
    end
end

-- --- claude: on PATH, or where its installer puts it (this PATH may predate the install) ---
if not on_path("claude.exe") and not on_path("claude.cmd") then
    local installed = (os.getenv("USERPROFILE") or "").."\\.local\\bin"
    if os.isfile(installed.."\\claude.exe") then
        add_to_path(installed)
    else
        table.insert(tips, "to run Claude Code, install it from PowerShell:  irm https://claude.ai/install.ps1 | iex")
    end
end

-- Grey tips for what's missing; delete this loop to stop them.
for _, tip in ipairs(tips) do
    clink.print("\x1b[90mTip: "..tip.."\x1b[m")
end
if #tips > 0 then clink.print("") end

-- --- ls colors (unless you set your own): folders bright blue, programs green, links cyan,
-- archives red, media magenta ---
if not os.getenv("LS_COLORS") then
    os.setenv("LS_COLORS", "di=1;94:ln=96:or=91:mi=91:ex=92:*.exe=92:*.bat=92:*.cmd=92:*.ps1=92:"..
        "*.zip=91:*.7z=91:*.rar=91:*.tar=91:*.gz=91:*.png=95:*.jpg=95:*.gif=95:*.mp4=95:*.mp3=95:*.wav=95")
end

-- --- aliases: $* is the rest of the typed line, $1..$9 single arguments ---
if on_path("ls.exe") then
    local ls = 'ls --color=auto --show-control-chars -F -I "NTUSER.DAT*" -I "ntuser.*" '
    os.setalias("ls", ls.."$*")
    os.setalias("ll", ls.."-ghGA $*")
end

-- --- view <image>: shows an image over this tab (clod view; any key closes it) ---
local clod_dir = os.getenv("CLOD_DIR")
if clod_dir then
    os.setalias("view", '"'..clod_dir..'clod.exe" view $*')
end

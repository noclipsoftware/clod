-- clod's prompt: the folder and its git branch (a * when tracked files changed), then the input on its
-- own line after a lambda, red when the last command failed. Git status runs in the background
-- (clink.promptcoroutine), so the prompt never waits on git; it fills in when ready.
local green  = "\x1b[32m"
local cyan   = "\x1b[36m"
local yellow = "\x1b[33m"
local red    = "\x1b[31m"
local norm   = "\x1b[m"

local prev_git_dir
local prev_status

local function collect_status()
    return git.getstatus(true) or {} -- true: skip untracked files, much faster in big repos
end

local clod_prompt = clink.promptfilter(150)
function clod_prompt:filter(prompt)
    local out = green..os.getcwd()..norm
    local git_dir = git.getgitdir()
    if git_dir then
        if prev_git_dir ~= git_dir then
            prev_git_dir = git_dir
            prev_status = nil
        end
        local status = clink.promptcoroutine(collect_status)
        if status then prev_status = status else status = prev_status or {} end
        local branch = status.branch or git.getbranch(git_dir, true) -- fast: reads .git\HEAD
        if branch and branch ~= "" and branch ~= ".invalid" then
            out = out.." "..cyan..branch..norm
            if status.dirty then out = out..yellow.."*"..norm end
        end
    end
    local failed = os.geterrorlevel and os.geterrorlevel() ~= 0
    return out.."\n"..(failed and red or "").."λ"..norm.." "
end

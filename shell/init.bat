@echo off
rem Runs first in every clod tab's cmd, before clink and claude start.

rem --- unix tools (ls, grep, cat, less, ...) from Git for Windows' usr\bin, if it's installed ---
rem Looks next to the git on PATH, then the usual install folders, then the installer's registry key.
rem Appended to PATH, so Windows' own find/sort/timeout and MSVC's link.exe still win.
set "_ClodGit="
set "_ClodGitUsr="
for %%G in (git.exe) do set "_ClodGit=%%~dp$PATH:G"
if defined _ClodGit if exist "%_ClodGit%..\usr\bin\ls.exe" set "_ClodGitUsr=%_ClodGit%..\usr\bin"
if defined _ClodGit if exist "%_ClodGit%..\..\usr\bin\ls.exe" if not defined _ClodGitUsr set "_ClodGitUsr=%_ClodGit%..\..\usr\bin"
for %%D in ("%ProgramFiles%\Git" "%ProgramFiles(x86)%\Git" "%LOCALAPPDATA%\Programs\Git" "%USERPROFILE%\scoop\apps\git\current") do if not defined _ClodGitUsr if exist "%%~D\usr\bin\ls.exe" set "_ClodGitUsr=%%~D\usr\bin"
if not defined _ClodGitUsr for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\GitForWindows" /v InstallPath 2^>nul') do if "%%A"=="REG_SZ" if exist "%%B\usr\bin\ls.exe" set "_ClodGitUsr=%%B\usr\bin"
if defined _ClodGitUsr for %%P in ("%_ClodGitUsr%") do set "PATH=%PATH%;%%~fP"

rem --- claude: on PATH, or where its installer puts it (PATH may predate the install) ---
set "_ClodClaude="
for %%G in (claude.exe claude.cmd) do if not "%%~$PATH:G"=="" set "_ClodClaude=1"
if not defined _ClodClaude if exist "%USERPROFILE%\.local\bin\claude.exe" set "PATH=%PATH%;%USERPROFILE%\.local\bin"
if exist "%USERPROFILE%\.local\bin\claude.exe" set "_ClodClaude=1"

rem --- tips in grey for what's missing; delete these lines to stop them ---
set "_ClodEsc="
if not defined _ClodGitUsr set "_ClodEsc=1"
if not defined _ClodClaude set "_ClodEsc=1"
if defined _ClodEsc for /f %%E in ('echo prompt $E^| cmd') do set "_ClodEsc=%%E"
if not defined _ClodGitUsr echo %_ClodEsc%[90mTip: for ls, grep, cat and other unix commands, install Git for Windows:  winget install Git.Git%_ClodEsc%[m
if not defined _ClodClaude echo %_ClodEsc%[90mTip: to run Claude Code, install it from PowerShell:  irm https://claude.ai/install.ps1 ^| iex%_ClodEsc%[m
set "_ClodGit="
set "_ClodGitUsr="
set "_ClodClaude="
set "_ClodEsc="

rem --- ls colors: folders bright blue, programs green, links cyan, archives red, media magenta ---
set "LS_COLORS=di=1;94:ln=96:or=91:mi=91:ex=92:*.exe=92:*.bat=92:*.cmd=92:*.ps1=92:*.zip=91:*.7z=91:*.rar=91:*.tar=91:*.gz=91:*.png=95:*.jpg=95:*.gif=95:*.mp4=95:*.mp3=95:*.wav=95"

rem --- aliases (ls, ll, gl, ...): edit aliases.txt next to this file ---
if exist "%~dp0aliases.txt" doskey /macrofile="%~dp0aliases.txt"

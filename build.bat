@echo off
setlocal
rem Unity build of clod with MSVC. Run from any shell; sets up the VS environment if needed.
rem   build          optimized
rem   build debug    unoptimized, debug CRT

where cl >nul 2>nul
if not errorlevel 1 goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul

:build
set "ROOT=%~dp0"
if not exist "%ROOT%build" mkdir "%ROOT%build"
pushd "%ROOT%build"

set FLAGS=/nologo /std:c++20 /utf-8 /EHsc /W4 /wd4100 /wd4505 /wd4201 /Zi /FC
set OPT=/O2 /MT
if "%1"=="debug" set OPT=/Od /MTd

rem --- icon: drawn by tools\make_icon.cpp, then compiled into a resource for the exe ---
cl %FLAGS% /O2 /MT "%ROOT%tools\make_icon.cpp" /Fe:make_icon.exe /link /INCREMENTAL:NO user32.lib gdi32.lib >nul
if errorlevel 1 (set ERR=1& goto :done)
"%ROOT%build\make_icon.exe" clod.ico
if errorlevel 1 (set ERR=1& goto :done)
rc /nologo /i "%ROOT%build" /fo clod.res "%ROOT%code\clod.rc"
if errorlevel 1 (set ERR=1& goto :done)

rem --- clink (vendor\clink, GPL-3.0) goes next to clod.exe; shell tabs inject it into cmd ---
xcopy /e /i /y /q /d "%ROOT%vendor\clink" clink >nul
if errorlevel 1 (set ERR=1& goto :done)
rem --- default shell setup; the server copies it to %USERPROFILE%\.clod on first run ---
xcopy /e /i /y /q /d "%ROOT%shell" shell >nul
if errorlevel 1 (set ERR=1& goto :done)

rem --- a running clod locks clod.exe, but Windows still lets it be renamed: move it aside so the ---
rem --- new build can land while the old one keeps running. Old copies go once nothing uses them. ---
del /q clod.old-*.exe >nul 2>nul
if exist clod.exe del /q clod.exe >nul 2>nul
if exist clod.exe move /y clod.exe clod.old-%RANDOM%.exe >nul

cl %FLAGS% %OPT% "%ROOT%code\clod.cpp" clod.res /Fe:clod.exe /link /INCREMENTAL:NO /OPT:REF /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%code\clod.manifest" kernel32.lib user32.lib shell32.lib advapi32.lib gdi32.lib
set ERR=%errorlevel%
if not "%ERR%"=="0" goto :done

rem --- clodw.exe: clod's own window (Direct3D 11 + DirectWrite), same move-aside trick ---
del /q clodw.old-*.exe >nul 2>nul
if exist clodw.exe del /q clodw.exe >nul 2>nul
if exist clodw.exe move /y clodw.exe clodw.old-%RANDOM%.exe >nul
cl %FLAGS% %OPT% "%ROOT%code\clodw.cpp" clod.res /Fe:clodw.exe /link /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%code\clod.manifest" kernel32.lib user32.lib shell32.lib advapi32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib d2d1.lib dwrite.lib comdlg32.lib imm32.lib
set ERR=%errorlevel%

:done
popd
exit /b %ERR%

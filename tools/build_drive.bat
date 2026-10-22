@echo off
setlocal
where cl >nul 2>nul
if not errorlevel 1 goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
:build
set "ROOT=%~dp0.."
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
pushd "%ROOT%\build"
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /wd4100 /wd4505 /Zi /O2 /MT "%ROOT%\tools\clod_drive.cpp" /Fe:clod_drive.exe /link /INCREMENTAL:NO
set ERR=%errorlevel%
popd
exit /b %ERR%

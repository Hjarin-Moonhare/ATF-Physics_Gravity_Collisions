@echo off
rem Puts the 64-bit MSVC tools on PATH for the script that calls this one (no setlocal here on purpose: the caller
rem keeps what it sets). Nothing to do when cl.exe is already on PATH (a Developer Command Prompt). Otherwise it finds
rem the newest Visual Studio or Build Tools with the C++ x64 tools through vswhere and runs that copy's vcvars64.bat.
where cl.exe >nul 2>nul && exit /b 0
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
where vswhere.exe >nul 2>nul || goto novs
set "TFP_VS="
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TFP_VS=%%i"
if not defined TFP_VS goto novs
if not exist "%TFP_VS%\VC\Auxiliary\Build\vcvars64.bat" goto novs
call "%TFP_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
exit /b 0
:novs
echo No Visual Studio with the C++ x64 tools found. Install Visual Studio 2022 or newer, or its Build Tools, with
echo "Desktop development with C++", or run this from a Developer Command Prompt.
exit /b 1

@echo off
rem Reads A-Body 1.5's release INIs (config\release\Fixed and Variable) with the release build's own LoadSettings
rem (tests\ini_check_release_collisions.cpp, built with TFP_RELEASE + TFP_RELEASE_COLLISIONS) and checks what it got.
setlocal
cd /d "%~dp0.."
call tools\vcvars.bat || exit /b 1
if not exist build\ini_check mkdir build\ini_check
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /DTFP_RELEASE /DTFP_RELEASE_COLLISIONS tests\ini_check_release_collisions.cpp /Fo:build\ini_check\ /Fe:build\ini_check\ini_check.exe /link /NOLOGO shell32.lib ole32.lib || exit /b 1
for %%c in (Fixed Variable) do (
    copy /y config\release\%%c\TFPhysics.ini build\ini_check\TFPhysics.ini >nul
    echo %%c:
    build\ini_check\ini_check.exe || exit /b 1
)

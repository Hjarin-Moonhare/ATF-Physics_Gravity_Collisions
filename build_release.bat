@echo off
rem Builds the plain release TFPhysics.dll (TFP_RELEASE: gravity, springs and body-shape physics; no log, no
rem statistics, no collisions): build\release\TFPhysics.dll. This is what A-Body 1.1.1 shipped (TF Physics 1.1/1.2).
setlocal
cd /d "%~dp0"
call tools\vcvars.bat || exit /b 1
if not exist build\release mkdir build\release
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /DTFP_RELEASE /LD src\plugin.cpp /Fo:build\release\ /Fe:build\release\TFPhysics.dll /link /NOLOGO shell32.lib ole32.lib || exit /b 1
echo built build\release\TFPhysics.dll

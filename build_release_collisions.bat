@echo off
rem Builds the release TFPhysics.dll with collisions between bodies on (TFP_RELEASE + TFP_RELEASE_COLLISIONS; still no
rem log and no world collision): build\release_col\TFPhysics.dll. This is what A-Body 1.5 ships (TF Physics 1.3).
rem Settings to go with it: config\release\Fixed or Variable\TFPhysics.ini.
setlocal
cd /d "%~dp0"
call tools\vcvars.bat || exit /b 1
if not exist build\release_col mkdir build\release_col
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /DTFP_RELEASE /DTFP_RELEASE_COLLISIONS /LD src\plugin.cpp /Fo:build\release_col\ /Fe:build\release_col\TFPhysics.dll /link /NOLOGO shell32.lib ole32.lib || exit /b 1
echo built build\release_col\TFPhysics.dll

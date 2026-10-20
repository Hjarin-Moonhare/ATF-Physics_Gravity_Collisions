@echo off
rem Builds the logging build of TFPhysics.dll (the 5 s log, statistics, collisions, the world-collision test code):
rem build\TFPhysics.dll. Settings to go with it: config\TFPhysics.ini.
setlocal
cd /d "%~dp0"
call tools\vcvars.bat || exit /b 1
if not exist build mkdir build
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /LD src\plugin.cpp /Fo:build\ /Fe:build\TFPhysics.dll /link /NOLOGO shell32.lib ole32.lib || exit /b 1
echo built build\TFPhysics.dll

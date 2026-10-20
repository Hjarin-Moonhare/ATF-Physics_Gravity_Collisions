@echo off
rem Stress test (tests\stress_test.cpp): the plugin's whole per-frame path on many fake bodies from 11 threads at once,
rem the memory guards against unreadable memory, and the cost per body-update. Logging build.
setlocal
cd /d "%~dp0.."
call tools\vcvars.bat || exit /b 1
if not exist build\tests mkdir build\tests
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 tests\stress_test.cpp /Fo:build\tests\ /Fe:build\tests\stress_test.exe /link /NOLOGO shell32.lib ole32.lib || exit /b 1
copy /y config\TFPhysics.ini build\tests\TFPhysics.ini >nul
build\tests\stress_test.exe

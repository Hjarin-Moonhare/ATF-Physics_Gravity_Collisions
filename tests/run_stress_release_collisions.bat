@echo off
rem The stress test built as the A-Body 1.5 release (TFP_RELEASE + TFP_RELEASE_COLLISIONS). The log case fails by
rem design there: the release build writes no log.
setlocal
cd /d "%~dp0.."
call tools\vcvars.bat || exit /b 1
if not exist build\tests_relcol mkdir build\tests_relcol
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /DTFP_RELEASE /DTFP_RELEASE_COLLISIONS tests\stress_test.cpp /Fo:build\tests_relcol\ /Fe:build\tests_relcol\stress_test.exe /link /NOLOGO shell32.lib ole32.lib || exit /b 1
copy /y config\TFPhysics.ini build\tests_relcol\TFPhysics.ini >nul
build\tests_relcol\stress_test.exe

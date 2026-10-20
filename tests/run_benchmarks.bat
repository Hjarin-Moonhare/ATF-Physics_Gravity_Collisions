@echo off
rem Two small measurements behind the plugin's memory rules:
rem   fault_cost.cpp  what one guarded read of bad memory costs (SafeCopy catching an access violation), with and
rem                   without a vectored exception handler installed first (crash loggers install one)
rem   vq_bench.cpp    what VirtualQuery costs inside a large region: the kernel walks the pages, so the cost grows with
rem                   the region's size. This is why nothing that runs every update may ask Windows about memory.
setlocal
cd /d "%~dp0.."
call tools\vcvars.bat || exit /b 1
if not exist build\tests mkdir build\tests
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 tests\fault_cost.cpp /Fo:build\tests\ /Fe:build\tests\fault_cost.exe /link /NOLOGO shell32.lib ole32.lib || exit /b 1
build\tests\fault_cost.exe
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 tests\vq_bench.cpp /Fo:build\tests\ /Fe:build\tests\vq_bench.exe /link /NOLOGO || exit /b 1
build\tests\vq_bench.exe

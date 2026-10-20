@echo off
rem Builds and runs the offline tests against the plugin's own code. No game needed:
rem   collide_test.cpp  collisions on A-Body's skeleton in 10 poses, other bodies, dead bodies, the world proxies
rem                     (against stand-ins for the game's Havok functions)
rem   gravity_test.cpp  gravity shapes, spring gravity per part, the breast size reading, the INI
rem   shape_test.cpp    body-shape physics: reading, factors, damping, spring gravity, swing scaling
rem   addrlib_test.cpp  the Address Library reader and every start-up check, on a fake game image; with the game found
rem                     (the first argument, else STARFIELD_DIR, else the usual Steam folders) also on the real
rem                     Starfield.exe, read from disk (read only)
setlocal
cd /d "%~dp0.."
call tools\vcvars.bat || exit /b 1
if not exist build\tests mkdir build\tests
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 tests\collide_test.cpp /Fo:build\tests\ /Fe:build\tests\collide_test.exe /link /NOLOGO shell32.lib ole32.lib || exit /b 1
build\tests\collide_test.exe || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 tests\gravity_test.cpp /Fo:build\tests\ /Fe:build\tests\gravity_test.exe /link /NOLOGO shell32.lib ole32.lib || exit /b 1
copy /y config\TFPhysics.ini build\tests\TFPhysics.ini >nul
build\tests\gravity_test.exe || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 tests\shape_test.cpp /Fo:build\tests\ /Fe:build\tests\shape_test.exe /link /NOLOGO shell32.lib ole32.lib || exit /b 1
copy /y config\TFPhysics.ini build\tests\TFPhysics.ini >nul
build\tests\shape_test.exe || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 tests\addrlib_test.cpp /Fo:build\tests\ /Fe:build\tests\addrlib_test.exe /link /NOLOGO shell32.lib ole32.lib || exit /b 1
build\tests\addrlib_test.exe %1

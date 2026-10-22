@echo off
rem Builds and runs the offline tests against the plugin's own code. No game needed:
rem   collide_test.cpp  collisions on A-Body's skeleton in 10 poses, other bodies, dead bodies, the world proxies
rem                     (against stand-ins for the game's Havok functions)
rem   gravity_test.cpp  gravity shapes, spring gravity per part, the breast size reading, the INI
rem   shape_test.cpp    body-shape physics: reading, factors, damping, spring gravity, swing scaling, tuning each part
rem   tune_dump.cpp     how a TFPhysics.ini and its tuning files are read (tune_dump.exe path\to\TFPhysics.ini)
rem   dent_test.cpp     the dents against their design model, on A-Body's three dent rigs (tests\data, tests\cases_dent)
rem   breast_mass_test.cpp  the breast and glute masses and the relative damping on made-up motion
rem   freemorph_test.cpp    FreeMorph's solve against its design model (needs tests\data\FreeMorph.bin: A-Body 2.0's)
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
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 tests\tune_dump.cpp /Fo:build\tests\ /Fe:build\tests\tune_dump.exe /link /NOLOGO shell32.lib ole32.lib || exit /b 1
build\tests\tune_dump.exe config\TFPhysics.ini >nul || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 tests\dent_test.cpp /Fo:build\tests\ /Fe:build\tests\dent_test.exe /link /NOLOGO || exit /b 1
build\tests\dent_test.exe || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /Isrc tests\breast_mass_test.cpp /Fo:build\tests\ /Fe:build\tests\breast_mass_test.exe /link /NOLOGO || exit /b 1
build\tests\breast_mass_test.exe || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /Isrc tests\freemorph_test.cpp /Fo:build\tests\ /Fe:build\tests\freemorph_test.exe /link /NOLOGO || exit /b 1
if exist tests\data\FreeMorph.bin (
    build\tests\freemorph_test.exe tests\data\FreeMorph.bin tests\data\freemorph_vectors.bin || exit /b 1
) else (
    echo freemorph_test: skipped, no tests\data\FreeMorph.bin
)
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 tests\addrlib_test.cpp /Fo:build\tests\ /Fe:build\tests\addrlib_test.exe /link /NOLOGO shell32.lib ole32.lib || exit /b 1
build\tests\addrlib_test.exe %1

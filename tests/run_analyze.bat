@echo off
rem Static analysis of the plugin (MSVC /analyze, all warnings): writes build\analyze.txt and prints the warning count.
rem Nothing is built. The count to beat: 30 (all known and harmless; see tests\README.md).
setlocal
cd /d "%~dp0.."
call tools\vcvars.bat || exit /b 1
if not exist build\analyze mkdir build\analyze
cl /nologo /std:c++20 /O2 /MT /EHsc /Wall /wd4514 /wd4820 /wd4710 /wd4711 /wd5045 /wd4625 /wd4626 /wd5026 /wd5027 /wd4668 /wd5039 /analyze /analyze:external- /c src\plugin.cpp /Fo:build\analyze\ > build\analyze.txt 2>&1
type build\analyze.txt | find /c "warning"

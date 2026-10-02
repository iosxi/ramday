@echo off
rem Build RamDay.exe (needs MinGW-w64 gcc and windres on PATH)
rem
rem MinGW's gcc links its own default-manifest.o, which clashes with ours
rem (two RT_MANIFEST #1). An empty default-manifest.o found first via -B
rem replaces it, so only ramday.manifest ends up in the exe.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
echo.> build\empty.c
gcc -c build\empty.c -o build\default-manifest.o || exit /b 1
windres ramday.rc -O coff -o build\ramday.res.o || exit /b 1
gcc -Bbuild/ -O2 -Wall -Wextra -municode -mwindows -static -s -o RamDay.exe ramday.c build\ramday.res.o -lcomctl32 -lshell32 -ladvapi32 -lole32 || exit /b 1
echo built RamDay.exe

@echo off
setlocal
rem GSticky Windows build script (MinGW-w64 / GCC)
rem Usage: double-click this file, or run win\build.bat from a command prompt.
rem Output: dist\GSticky.exe (static-linked single file, no runtime dependencies)
rem
rem Steps: windres embeds win\GSticky.ico as a resource, then links with main.cpp.
rem GSticky.ico is committed; Python is only needed when regenerating it from icon.png.
rem
rem Compiler lookup: g++ / windres from PATH by default. If the toolchain is not
rem on PATH, set GSTICKY_GXX to the absolute path of g++.exe; windres is then
rem derived from the same directory.

set "GXX=g++"
if defined GSTICKY_GXX set "GXX=%GSTICKY_GXX%"
set "WINDRES=windres"
for %%I in ("%GXX%") do if exist "%%~dpIwindres.exe" set "WINDRES=%%~dpIwindres.exe"

"%GXX%" --version >nul 2>&1
if errorlevel 1 (
    echo [GSticky] ERROR: g++.exe not found.
    echo            Add the MinGW-w64 bin directory to PATH, or run:
    echo            set "GSTICKY_GXX=[full path to g++.exe]"
    exit /b 1
)

set "SRC=%~dp0main.cpp"
set "RES=%~dp0GSticky_res.o"
set "DIST=%~dp0..\dist"
set "OUT=%DIST%\GSticky.exe"
if not exist "%DIST%" mkdir "%DIST%"

echo [GSticky] embedding icon ...
pushd "%~dp0"
"%WINDRES%" -i "GSticky.rc" -o "GSticky_res.o" --output-format=coff
if errorlevel 1 (popd & echo [GSticky] ICON RESOURCE FAILED & exit /b 1)
popd

echo [GSticky] compiler: %GXX%
"%GXX%" -std=c++17 -O2 -s -mwindows -static -Wall -Wextra -Wno-unused-parameter "%SRC%" "%RES%" -o "%OUT%" -luser32 -lgdi32 -lshell32 -ladvapi32
if errorlevel 1 (echo [GSticky] BUILD FAILED & exit /b 1)

echo [GSticky] BUILD OK: %OUT%
for %%F in ("%OUT%") do echo [GSticky] size: %%~zF bytes

@echo off
setlocal
cd /d "%~dp0"
set "GCC="
for %%G in (gcc.exe) do set "GCC=%%~$PATH:G"
if not defined GCC if exist C:\msys64\ucrt64\bin\gcc.exe set "GCC=C:\msys64\ucrt64\bin\gcc.exe"
if not defined GCC echo gcc not found. Install MinGW-w64 UCRT64 and rerun. & exit /b 1
for %%I in ("%GCC%") do set "PATH=%%~dpI;%PATH%"
set "VERSION="
set /p VERSION=<"%~dp0..\VERSION"
if not defined VERSION echo VERSION file is missing or empty. & exit /b 1
echo #define DSH_SUBAGENT_VERSION "%VERSION%">"%~dp0version.h"
"%GCC%" -municode -O2 -s -Wall -Wextra -static-libgcc -o dsh-subagent.exe dsh-subagent.c
exit /b %ERRORLEVEL%

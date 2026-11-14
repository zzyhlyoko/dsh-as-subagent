@echo off
setlocal
set "GCC="
for %%G in (gcc.exe) do set "GCC=%%~$PATH:G"
if not defined GCC if exist C:\msys64\ucrt64\bin\gcc.exe set "GCC=C:\msys64\ucrt64\bin\gcc.exe"
if not defined GCC echo gcc not found. Install MinGW-w64 UCRT64 and rerun. & exit /b 1
for %%I in ("%GCC%") do set "PATH=%%~dpI;%PATH%"
call "%~dp0..\scripts\build.cmd"
if errorlevel 1 exit /b 1
set "OUT=%TEMP%\dsh-as-subagent-smoke"
if not exist "%OUT%" mkdir "%OUT%"
"%GCC%" -municode -O2 -s -Wall -Wextra -static-libgcc -o "%OUT%\fake-node.exe" "%~dp0fake-node.c"
if errorlevel 1 exit /b 1
"%GCC%" -municode -O2 -s -Wall -Wextra -static-libgcc -o "%OUT%\smoke.exe" "%~dp0smoke.c"
if errorlevel 1 exit /b 1
"%OUT%\smoke.exe" "%~dp0.." "%OUT%\fake-node.exe"
exit /b %ERRORLEVEL%

@echo off
REM Build "PLSC v0.3.7.exe" (Pekora Live Stream Checker) with MinGW (gcc).
REM Key flags / libs:
REM   -municode  -> wWinMain entry      -mwindows -> GUI (no console)
REM   -lwininet  -> InternetOpenW...    -lshell32 -> ShellExecuteW
REM   -lgdi32/-luser32 -> Win32 + drawing   -lmsimg32 -> AlphaBlend
REM   -lgdiplus  -> image decode        -lole32   -> CreateStreamOnHGlobal
REM Embedded via PLSC_res.rc (windres): pekora.png + pixelfont.ttf + loading.gif.
REM Stream cover images are fetched live from YouTube at runtime.
setlocal
cd /d "%~dp0"
set "EXE=PLSC v0.3.7.exe"
windres PLSC_res.rc -O coff -o PLSC_res.o
if errorlevel 1 goto fail
gcc "Pekora Live Stream Checker.c" PLSC_res.o -o "%EXE%" -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE -municode -mwindows -lwininet -lshell32 -lgdi32 -luser32 -lmsimg32 -lgdiplus -lole32 -Wall
if errorlevel 1 goto fail
echo.
echo Build OK: "%EXE%"
endlocal
exit /b 0
:fail
echo.
echo Build FAILED.
endlocal
exit /b 1

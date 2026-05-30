@echo off
REM Build pekora_live.exe with MinGW (gcc).
REM Key flags / libs:
REM   -municode  -> wWinMain entry      -mwindows -> GUI (no console)
REM   -lwininet  -> InternetOpenW...    -lshell32 -> ShellExecuteW
REM   -lgdi32/-luser32 -> Win32 + drawing   -lmsimg32 -> AlphaBlend
REM   -lgdiplus  -> image decode        -lole32   -> CreateStreamOnHGlobal
REM Embedded via pekora_res.rc (windres): pekora.png + pixelfont.ttf (DotGothic16).
REM Stream cover images are fetched live from YouTube at runtime.
setlocal
cd /d "%~dp0"
windres pekora_res.rc -O coff -o pekora_res.o
if errorlevel 1 goto fail
gcc pekora_live.c pekora_res.o -o pekora_live.exe -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE -municode -mwindows -lwininet -lshell32 -lgdi32 -luser32 -lmsimg32 -lgdiplus -lole32 -Wall
if errorlevel 1 goto fail
echo.
echo Build OK: pekora_live.exe
endlocal
exit /b 0
:fail
echo.
echo Build FAILED.
endlocal
exit /b 1
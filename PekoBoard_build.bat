@echo off
REM Build "Peko Board v0.4.0.exe" with MinGW (gcc).
REM Key flags / libs:
REM   -municode  -> wWinMain entry      -mwindows -> GUI (no console)
REM   -lwininet  -> InternetOpenW...    -lshell32 -> ShellExecuteW / tray
REM   -lgdi32/-luser32 -> Win32 + drawing   -lmsimg32 -> AlphaBlend/GradientFill
REM   -lgdiplus  -> image decode        -lole32   -> CreateStreamOnHGlobal
REM   -lwinmm    -> waveOut music player, PlaySound, timeBeginPeriod
REM   -O2        -> keep the bundled minimp3 MP3 decoder fast
REM Bundled headers: minimp3.h / minimp3_ex.h (lieff/minimp3, CC0 public domain).
REM Embedded via PekoBoard_res.rc (windres): pekora.png + "pekora postures.png"
REM (expression sheet) + pixelfont.ttf + loading.gif.
setlocal
cd /d "%~dp0"
set "EXE=Peko Board v0.4.0.exe"
windres PekoBoard_res.rc -O coff -o PekoBoard_res.o
if errorlevel 1 goto fail
gcc -O2 peko_main.c peko_pages.c peko_draw.c peko_chara.c peko_music.c peko_parser.c peko_net.c peko_img.c peko_lang.c peko_cfg.c PekoBoard_res.o -o "%EXE%" -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE -municode -mwindows -lwininet -lshell32 -lgdi32 -luser32 -lmsimg32 -lgdiplus -lole32 -lwinmm -Wall
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

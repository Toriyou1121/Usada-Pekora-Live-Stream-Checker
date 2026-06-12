@echo off
REM ===========================================================================
REM  package.bat  -  bundle everything needed for the GitHub repo / release.
REM  Rebuilds the .exe (best effort), then zips the source, assets, build
REM  script, the built .exe and the font licence into one versioned archive.
REM  Output:  PekoBoard_<VERSION>.zip
REM ===========================================================================
setlocal
cd /d "%~dp0"

REM --- read the version straight from the header (single source of truth) ---
set "VERSION="
for /f "usebackq delims=" %%V in (`powershell -NoProfile -Command "[regex]::Match((Get-Content -Raw -LiteralPath '%~dp0peko.h'),'v\d+\.\d+\.\d+').Value"`) do set "VERSION=%%V"
if not defined VERSION set "VERSION=v0.0.0"
echo Packaging version %VERSION%
set NAME=PekoBoard_%VERSION%
set STAGE=%NAME%
set OUT=%NAME%.zip
set "EXE=Peko Board %VERSION%.exe"

REM --- 1. make sure the bundled exe is current (needs MinGW; ok to skip) ---
if exist "%~dp0PekoBoard_build.bat" (
    echo Building latest "%EXE%" ...
    call "%~dp0PekoBoard_build.bat"
)

REM --- 2. stage the files that exist into a clean folder ---
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
for %%F in (
    peko.h
    peko_main.c
    peko_pages.c
    peko_draw.c
    peko_chara.c
    peko_music.c
    peko_parser.c
    peko_net.c
    peko_img.c
    peko_lang.c
    peko_cfg.c
    minimp3.h
    minimp3_ex.h
    PekoBoard_res.rc
    pekora.png
    pixelfont.ttf
    loading.gif
    PekoBoard_build.bat
    package.bat
    README.md
    CHANGELOG.md
    DotGothic16-OFL.txt
) do (
    if exist "%%~F" ( copy /y "%%~F" "%STAGE%\" >nul ) else ( echo   [skip] %%~F not found )
)
if exist "%EXE%" ( copy /y "%EXE%" "%STAGE%\" >nul ) else ( echo   [skip] "%EXE%" not found )

REM --- ship the music/voice/cover drop folders (instructions only, never
REM     the user's own audio or art files) ---
mkdir "%STAGE%\music" 2>nul
mkdir "%STAGE%\voice" 2>nul
mkdir "%STAGE%\cover" 2>nul
if exist "music\PUT_YOUR_MUSIC_HERE.txt"  copy /y "music\PUT_YOUR_MUSIC_HERE.txt"  "%STAGE%\music\" >nul
if exist "voice\PUT_VOICE_CLIPS_HERE.txt" copy /y "voice\PUT_VOICE_CLIPS_HERE.txt" "%STAGE%\voice\" >nul
if exist "cover\PUT_COVER_ART_HERE.txt"   copy /y "cover\PUT_COVER_ART_HERE.txt"   "%STAGE%\cover\" >nul

REM --- 3. zip the folder (PowerShell Compress-Archive) ---
if exist "%OUT%" del "%OUT%"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%' -DestinationPath '%OUT%' -Force"
rmdir /s /q "%STAGE%"

if exist "%OUT%" (
    echo.
    echo Created %OUT%
    echo Upload this to your GitHub release, or commit the source files directly.
) else (
    echo.
    echo FAILED to create %OUT%
)
endlocal

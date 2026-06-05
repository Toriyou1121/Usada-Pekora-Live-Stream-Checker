@echo off
REM ===========================================================================
REM  package.bat  -  bundle everything needed for the GitHub repo / release.
REM  Rebuilds the .exe (best effort), then zips the source, assets, build
REM  script, the built .exe and the font licence into one versioned archive.
REM  Output:  PLSC_<VERSION>.zip
REM ===========================================================================
setlocal
cd /d "%~dp0"

REM --- read the version straight from the source (single source of truth) ---
set "VERSION="
for /f "usebackq delims=" %%V in (`powershell -NoProfile -Command "[regex]::Match((Get-Content -Raw -LiteralPath '%~dp0Pekora Live Stream Checker.c'),'v\d+\.\d+\.\d+').Value"`) do set "VERSION=%%V"
if not defined VERSION set "VERSION=v0.0.0"
echo Packaging version %VERSION%
set NAME=PLSC_%VERSION%
set STAGE=%NAME%
set OUT=%NAME%.zip
set "EXE=PLSC %VERSION%.exe"

REM --- 1. make sure the bundled exe is current (needs MinGW; ok to skip) ---
if exist "%~dp0PLSC_build.bat" (
    echo Building latest "%EXE%" ...
    call "%~dp0PLSC_build.bat"
)

REM --- 2. stage the files that exist into a clean folder ---
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
for %%F in (
    "Pekora Live Stream Checker.c"
    PLSC_res.rc
    pekora.png
    pixelfont.ttf
    loading.gif
    PLSC_build.bat
    package.bat
    README.md
    CHANGELOG.md
    DotGothic16-OFL.txt
) do (
    if exist "%%~F" ( copy /y "%%~F" "%STAGE%\" >nul ) else ( echo   [skip] %%~F not found )
)
if exist "%EXE%" ( copy /y "%EXE%" "%STAGE%\" >nul ) else ( echo   [skip] "%EXE%" not found )

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
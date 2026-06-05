# Usada Pekora Live Stream Checker

A Win32 desktop client that reports whether the VTuber Usada Pekora is live on
YouTube and lists her upcoming streams. Single C source file, no runtime
dependencies; art and font are embedded in the executable.

Unofficial, non-commercial fan project. Not affiliated with Usada Pekora,
hololive, or COVER Corp.

<img width="2559" height="1440" alt="Interface" src="https://github.com/user-attachments/assets/cab6e551-a8ee-4568-888f-4124076cab4e" />


## Requirements

- Windows 10/11, 64-bit.
- Network access to youtube.com. Where YouTube is blocked, a working HTTP
  proxy/VPN is required; the app reports a connection error otherwise.

## Usage

Download the latest `PLSC_vX.Y.Z.zip` from Releases, unzip, and run
`PLSC v0.3.7.exe`. No installer. The binary is unsigned, so SmartScreen may
prompt on first launch (More info -> Run anyway).

The schedule is read from `youtube.com/@usadapekora/streams`. Times are shown
in Japan time by default; the clock button toggles to local system time. The
language button cycles Japanese / English / Traditional Chinese. `F11` toggles
full screen; the gear menu sets the window resolution.

## Notable details

- Schedule entries are parsed from the page's embedded `ytInitialData`, not a
  public API (none is exposed). A `PREF=tz` cookie pins YouTube's server-side
  formatting to JST, so the local-time conversion is deterministic.
- Each entry is tagged with the timezone it is shown in; the header carries a
  localized weekday and the live clock.
- GDI rendering with a double-buffered ~60 fps loop. Cover thumbnails are
  decoded with GDI+; the loading animation and split-flap text effects are
  CPU-only.
- Dropping a `pixel.ttf` next to the executable overrides the bundled
  DotGothic16 font.

## Build

Requires MinGW-w64 (gcc + windres). Run `PLSC_build.bat`, or:

```
windres PLSC_res.rc -O coff -o PLSC_res.o
gcc "Pekora Live Stream Checker.c" PLSC_res.o -o "PLSC v0.3.7.exe" ^
    -municode -mwindows ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE ^
    -lwininet -lshell32 -lgdi32 -luser32 -lmsimg32 -lgdiplus -lole32
```

`package.bat` rebuilds and bundles the source, assets, and binary into
`PLSC_<version>.zip`.

`PART 1` (the parser) compiles on its own with `-DSELFTEST` to run against a
saved copy of the streams page.

## License

The bundled DotGothic16 font is licensed under the SIL Open Font License 1.1
(see `DotGothic16-OFL.txt`). Stream data, thumbnails, and character likeness
belong to their respective owners. No license is set for the application code
yet; add a `LICENSE` file before redistributing.

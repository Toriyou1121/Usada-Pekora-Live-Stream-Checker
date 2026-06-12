# Peko Board

A Win32 desktop fan app for the VTuber Usada Pekora: stream schedule board,
replay (VOD) gallery, local music player, and an interactive mascot. A single
native executable written in C with GDI. No frameworks, no installer, no
runtime dependencies.

Unofficial, non-commercial fan project. Not affiliated with Usada Pekora,
hololive, or COVER Corp.

Peko Board is the successor to Usada Pekora Live Stream Checker (v0.3.x).

<!-- TODO: replace with an interface screenshot -->
<img width="2559" height="1440" alt="Interface v0 4 0" src="https://github.com/user-attachments/assets/86cb2ce0-6388-4a67-b8e5-98726727d915" />

## What it does

### BOARD

- Split-flap schedule board with live / upcoming streams, fetched from
  `youtube.com/@usadapekora/streams`.
- Flip-card clock, ON AIR / OFFLINE status, and a countdown to the next
  scheduled stream.
- Clickable watch and chat links per entry.
- Cover-photo carousel and an X (Twitter) quick-links card (profile, latest
  posts, fan-art tag).
- Times shown in JST by default; one click switches to local system time.

### REPLAYS

- The most recent finished streams as photo cards with lazily loaded
  thumbnails; click a card to open the archive on YouTube.

### MUSIC

- Local music player: drop MP3 / WAV files into the `music` folder next to
  the exe.
- Playlist, seek bar, loop / shuffle, and a volume slider that only affects
  music.
- Spinning vinyl with album art from the `cover` folder (name an image like
  a track, or `default.png`).
- MP3 is decoded in-app by the bundled minimp3 decoder and streamed through
  waveOut, so playback works even on Windows editions whose MCI / DirectShow
  media components are missing or broken. Seeking is sample-accurate.
- Playback continues across tabs, with a now-playing chip in the bottom bar.

### Pekora

- Interactive mascot with an idle breathing animation, click reactions with
  particles, optional voice clips (random `.wav` from the `voice` folder),
  petting, and a persistent affection rank.
- Talk mode: a short dialogue box in her corner. She asks questions and the
  answers you pick (click or keys 1-3) change her reaction and affection.
  Press `T` to talk, Space / Enter to advance, Esc to close. She occasionally
  starts a conversation on her own. Dialogue is fully localized.
- Emotion-driven motion: each of 10 moods plays its own motion recipe (hops,
  shakes, sways, camera zoom and dimming) plus an animated icon above her
  head.
- Extras: context-aware phrases (morning / night / on-air), a daily omikuji
  fortune on the board's sakura blossom, and a Konami-code easter egg.

### General

- Live notification: background auto-check every 5 or 10 minutes; a tray
  balloon and a LIVE marker in the window title when Pekora goes live.
  Optional minimize-to-tray.
- All networking runs on worker threads, so the UI never freezes during a
  check. Rendering is double-buffered with partial repaints.
- UI in Japanese, English, and Traditional Chinese.
- Settings persist in `PekoBoard.ini`. `F11` toggles full screen; window
  size, volumes, auto-check, notification, petal, and tray options are in
  the gear menu.

## Requirements

- Windows 10/11, 64-bit.
- Network access to youtube.com. Where YouTube is blocked, a working HTTP
  proxy / VPN is required.

## Usage

Download `PekoBoard_vX.Y.Z.zip` from Releases, unzip, and run
`Peko Board vX.Y.Z.exe`. The binary is unsigned, so SmartScreen may prompt
on first launch (More info -> Run anyway).

Optional drop folders next to the exe (included, with instructions inside):

- `music\` - MP3 / WAV files for the MUSIC tab.
- `cover\` - album art for the vinyl label.
- `voice\` - short `.wav` clips played when you click Pekora.
- `pixel.ttf` - optional file that overrides the bundled DotGothic16 font.

## Build

Requires MinGW-w64 (gcc + windres). Run `PekoBoard_build.bat`, or:

```
windres PekoBoard_res.rc -O coff -o PekoBoard_res.o
gcc -O2 peko_main.c peko_pages.c peko_draw.c peko_chara.c peko_music.c ^
    peko_parser.c peko_net.c peko_img.c peko_lang.c peko_cfg.c ^
    PekoBoard_res.o -o "Peko Board v0.4.0.exe" ^
    -municode -mwindows ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE ^
    -lwininet -lshell32 -lgdi32 -luser32 -lmsimg32 -lgdiplus -lole32 -lwinmm
```

`package.bat` rebuilds and bundles the source, assets, and binary into
`PekoBoard_<version>.zip`.

`peko_parser.c` compiles on its own with `-DSELFTEST` to run the parser
against a saved copy of the streams page.

Implementation notes:

- The schedule is parsed from the page's embedded `ytInitialData`; no public
  API exists for this. A `PREF=tz` cookie pins YouTube's server-side
  formatting to JST so the local-time conversion is deterministic.
- The page fetch and every thumbnail download / decode run on worker threads
  and post results back to the UI thread as window messages.
- The X card is links-only on purpose: X blocks anonymous reads, so an
  embedded timeline cannot be made reliable.

## License

The bundled DotGothic16 font is licensed under the SIL Open Font License 1.1
(see `DotGothic16-OFL.txt`). The bundled MP3 decoder (`minimp3.h` /
`minimp3_ex.h`, github.com/lieff/minimp3) is CC0 public domain. Stream data,
thumbnails, and character likeness belong to their respective owners. No
license is set for the application code yet.

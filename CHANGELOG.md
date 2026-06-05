# Changelog

## v0.3.7

Changes since v0.3.6.

Interface
- The board header now shows the current date and weekday, replacing the
  static "PEKORA STREAM BOARD" caption.
- Each scheduled entry is tagged with the timezone it is shown in: local time
  in Japan-time mode, or the detected system UTC offset in system-time mode.
- Schedule rows now reveal with a split-flap-style flicker instead of the
  previous slide-in, and replay a short flicker when their content changes
  (timezone or language switch).
- Buttons have a hover highlight (fade-in/out state layer).
- A loading animation appears on the board while a check runs.

Behavior
- Default language is now Japanese. The language button cycles
  Japanese -> English -> Traditional Chinese.

Naming and build
- Window title is now "Usada Pekora Live Stream Checker".
- Binary is `PLSC v0.3.7.exe`; resource and build files renamed to
  `PLSC_res.rc` / `PLSC_res.o` and `PLSC_build.bat`.

The previous release (v0.3.6) and its source remain available in
`UsadaPekoraLiveChecker_v0.3.6.zip`.

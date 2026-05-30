# Usada Pekora YouTube Live Checker

A cute little **Windows desktop app** for fans of the VTuber **Usada Pekora** 🥕.
One click tells you whether she's **live right now**, shows her **upcoming-stream
timetable** on a Japanese split-flap "departure board" with a live flip-clock,
the **stream cover**, and her **character art** in the corner.

> Unofficial fan-made tool. Not affiliated with Usada Pekora, hololive, or COVER Corp.

<!-- Add a screenshot here, e.g.:  ![screenshot](docs/screenshot.png) -->

---

## ✨ Features

- **Live / upcoming check** — scrapes `youtube.com/@usadapekora/streams` and shows
  `ON AIR` / `OFFLINE` plus every scheduled stream.
- **Split-flap board** with a real **card-flip clock** (system time).
- **Per-stream links** — `watch` (the live / waiting room) and `chat`.
- **Cover carousel** — thumbnails fetched live from YouTube, auto + manual (`◀ ▶`).
- **Timezone toggle** 🕒 — show schedule times in **Japan time (JST)** or **your
  computer's time**.
- **3 languages** — English / 日本語 / 繁體中文.
- **Resolution presets** (1280×720, 1920×1080, 2560×1440) + **full screen** (`F11`),
  DPI-aware, **pixel-font** UI.
- **Membership "Join"** button and a link to this project.

---

## ⬇️ Download & run

1. Grab the latest **`UsadaPekoraLiveChecker_vX.Y.Z.zip`** from the
   [**Releases**](https://github.com/Toriyou1121/Usada-Pekora-Youtube-Live-Checker/releases) page.
2. Unzip and run **`pekora_live.exe`**.

- **Windows 10 / 11**, 64-bit. No install, no extra DLLs — everything (art + font)
  is embedded in the single `.exe`.
- First launch may show a SmartScreen prompt (the app is unsigned) → **More info →
  Run anyway**.
- Needs internet to reach YouTube. In regions where YouTube is blocked
  (e.g. mainland China) a working **VPN / proxy** is required — the app will say so
  if it can't connect.

---

## 🛠️ Build from source

Requires **MinGW-w64 (gcc + windres)**. From the project folder just run:

```bat
build_pekora.bat
```

That runs `windres` to embed `pekora.png` + `pixelfont.ttf`, then `gcc`. Full command:

```bat
windres pekora_res.rc -O coff -o pekora_res.o
gcc pekora_live.c pekora_res.o -o pekora_live.exe -municode -mwindows ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE ^
    -lwininet -lshell32 -lgdi32 -luser32 -lmsimg32 -lgdiplus -lole32
```

To build a release zip: **`package.bat`** (rebuilds, then bundles everything into
`UsadaPekoraLiveChecker_<version>.zip`).

---

## 🎛️ Controls

| Control | Action |
|---|---|
| **Check** | Load live / upcoming streams |
| **Channel** | Open her YouTube channel |
| **Language** | Cycle EN / JA / ZH-Hant |
| **GitHub** | Open this project |
| **Join** | Open the channel membership page |
| 🕒 (title bar) | Toggle schedule times: Japan time ↔ your system time |
| ⚙ (title bar) | Resolution presets / full screen |
| **F11** | Full screen on/off |
| `◀ ▶` | Switch the cover image (also auto-rotates) |
| a row's **watch / chat** | Open that stream / its live chat |

### Custom pixel font (optional)
The UI ships with the **DotGothic16** pixel font embedded. To use a different one
(e.g. a Traditional-Chinese pixel font), drop a `pixel.ttf` next to `pekora_live.exe`
— it overrides the built-in font automatically.

---

## 🧩 How it works (brief)

Single C file, **Win32 + GDI**, no frameworks. It fetches the channel's *streams*
page over HTTPS (WinINet, honoring the system proxy), pulls live/upcoming entries
straight out of YouTube's embedded JSON, decodes thumbnails with GDI+, and draws
everything with a ~60 fps double-buffered renderer. A `PREF=tz` cookie asks YouTube
to format schedule times in JST; the app converts to your timezone on demand.

---

## 📄 Credits & license

- **Author:** Toriyou1121 — built with **Claude Code** (model Claude Opus 4.8).
- **Font:** [DotGothic16](https://github.com/fontworks-fonts/DotGothic16) by the
  DotGothic16 Project Authors, under the **SIL Open Font License 1.1**
  (see `DotGothic16-OFL.txt`).
- Stream data, thumbnails, and character likeness belong to their respective owners
  (YouTube / COVER Corp / hololive). This is a non-commercial fan project.

> Choose a license for your own code (e.g. MIT) and add a `LICENSE` file.
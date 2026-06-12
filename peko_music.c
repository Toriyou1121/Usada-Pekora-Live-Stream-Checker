/*
 * peko_music.c - local music playback with a built-in decoder.
 *
 * MCI/DirectShow MP3 playback is broken on some Windows editions (notably
 * the one this app was born on: MCIERR_INTERNAL on every open), so Peko
 * Board decodes audio itself:
 *
 *   .mp3  -> minimp3 (lieff/minimp3, CC0 public domain, bundled headers)
 *   .wav  -> small built-in RIFF reader (PCM 8/16-bit and float32)
 *
 * Output is a plain waveOut stream: 4 x 4096-frame buffers refilled by a
 * feeder thread that waits on the device event. Volume is applied in
 * software per-sample (so the music slider never touches the voice clips),
 * seeking is sample-accurate via minimp3's index, and when the last buffer
 * drains the feeder posts WM_APP_TRACKEND to the UI thread, which decides
 * what plays next (loop / shuffle / stop).
 *
 * Cover art: an image in the "cover" folder named like the track
 * (cover\<track name>.png/jpg/...) - or cover\default.* as a fallback - is
 * loaded on track change and shown as the spinning vinyl label.
 */
#include "peko.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

Track g_tracks[MAX_TRACKS];
int   g_trackCount = 0;
int   g_curTrack   = -1;
int   g_playing    = 0;        /* 0 stopped, 1 playing, 2 paused */

/* ---------- decode source: mp3 or wav ---------- */

typedef struct {
    int       isMp3;
    mp3dec_ex_t mp3;
    FILE     *wf;              /* wav file                              */
    int       wavFmt;          /* 1 pcm, 3 float                        */
    int       wavBits, wavAlign;
    long      wavDataStart;
    unsigned long long wavDataBytes;
    int       hz, ch;
    unsigned long long totalFrames;   /* per-channel sample frames      */
} Source;

static int rd_u32le(FILE *f, unsigned *v)
{
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    *v = b[0] | (b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
    return 1;
}

static int src_open_wav(Source *s, const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    char id[4]; unsigned sz, rifftype;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) ||
        !rd_u32le(f, &sz) || fread(&rifftype, 1, 4, f) != 4 ||
        memcmp(&rifftype, "WAVE", 4)) { fclose(f); return 0; }
    int haveFmt = 0;
    for (;;) {
        if (fread(id, 1, 4, f) != 4 || !rd_u32le(f, &sz)) break;
        if (!memcmp(id, "fmt ", 4)) {
            unsigned char fmt[40];
            unsigned n = sz < 40 ? sz : 40;
            if (fread(fmt, 1, n, f) != n) break;
            if (sz > n) fseek(f, sz - n, SEEK_CUR);
            s->wavFmt   = fmt[0] | (fmt[1] << 8);
            s->ch       = fmt[2] | (fmt[3] << 8);
            s->hz       = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            s->wavAlign = fmt[12] | (fmt[13] << 8);
            s->wavBits  = fmt[14] | (fmt[15] << 8);
            if (s->wavFmt == 0xFFFE && sz >= 26)     /* EXTENSIBLE: subformat */
                s->wavFmt = fmt[24] | (fmt[25] << 8);
            haveFmt = 1;
        } else if (!memcmp(id, "data", 4)) {
            s->wavDataStart = ftell(f);
            s->wavDataBytes = sz;
            fseek(f, (sz + 1) & ~1u, SEEK_CUR);
        } else {
            fseek(f, (sz + 1) & ~1u, SEEK_CUR);     /* chunks are word-aligned */
        }
        if (haveFmt && s->wavDataStart) break;
    }
    int okfmt = haveFmt && s->wavDataStart && s->ch >= 1 && s->ch <= 2 &&
                s->hz >= 8000 && s->wavAlign > 0 &&
                ((s->wavFmt == 1 && (s->wavBits == 8 || s->wavBits == 16)) ||
                 (s->wavFmt == 3 && s->wavBits == 32));
    if (!okfmt) { fclose(f); return 0; }
    s->wf = f;
    s->totalFrames = s->wavDataBytes / s->wavAlign;
    fseek(f, s->wavDataStart, SEEK_SET);
    return 1;
}

static int src_open(Source *s, const wchar_t *path)
{
    memset(s, 0, sizeof *s);
    const wchar_t *dot = wcsrchr(path, L'.');
    if (dot && _wcsicmp(dot, L".wav") == 0)
        return src_open_wav(s, path);
    if (mp3dec_ex_open_w(&s->mp3, path, MP3D_SEEK_TO_SAMPLE) != 0) return 0;
    if (s->mp3.info.channels < 1 || s->mp3.info.channels > 2) {
        mp3dec_ex_close(&s->mp3); return 0;
    }
    s->isMp3 = 1;
    s->hz = s->mp3.info.hz;
    s->ch = s->mp3.info.channels;
    s->totalFrames = s->mp3.samples / s->ch;
    return 1;
}

static void src_close(Source *s)
{
    if (s->isMp3) mp3dec_ex_close(&s->mp3);
    if (s->wf) fclose(s->wf);
    memset(s, 0, sizeof *s);
}

/* read up to `frames` interleaved frames as int16; returns frames read */
static int src_read(Source *s, short *out, int frames)
{
    if (s->isMp3) {
        size_t n = mp3dec_ex_read(&s->mp3, out, (size_t)frames * s->ch);
        return (int)(n / s->ch);
    }
    if (s->wavBits == 16 && s->wavFmt == 1) {
        size_t n = fread(out, s->wavAlign, frames, s->wf);
        return (int)n;
    }
    if (s->wavBits == 8) {                       /* unsigned 8-bit pcm */
        unsigned char tmp[4096];
        int total = 0;
        while (total < frames) {
            int want = frames - total;
            if (want > (int)(sizeof tmp / s->wavAlign)) want = sizeof tmp / s->wavAlign;
            int n = (int)fread(tmp, s->wavAlign, want, s->wf);
            if (n <= 0) break;
            for (int i = 0; i < n * s->ch; ++i)
                out[total * s->ch + i] = (short)((tmp[i] - 128) << 8);
            total += n;
        }
        return total;
    }
    /* float32 */
    {
        float tmp[2048];
        int total = 0;
        while (total < frames) {
            int want = frames - total;
            if (want > (int)(sizeof tmp / sizeof(float) / s->ch))
                want = sizeof tmp / sizeof(float) / s->ch;
            int n = (int)fread(tmp, s->wavAlign, want, s->wf);
            if (n <= 0) break;
            for (int i = 0; i < n * s->ch; ++i) {
                float v = tmp[i] * 32767.0f;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                out[total * s->ch + i] = (short)v;
            }
            total += n;
        }
        return total;
    }
}

static void src_seek(Source *s, unsigned long long frame)
{
    if (frame > s->totalFrames) frame = s->totalFrames;
    if (s->isMp3) mp3dec_ex_seek(&s->mp3, frame * s->ch);
    else fseek(s->wf, s->wavDataStart + (long)(frame * s->wavAlign), SEEK_SET);
}

/* ---------- waveOut streaming engine ---------- */

#define NBUF       4
#define BUF_FRAMES 4096

static CRITICAL_SECTION g_mcs;
static HANDLE        g_mevent;          /* device signals: a buffer drained */
static HANDLE        g_mthread;
static volatile LONG g_mquit;

static Source        g_src;
static HWAVEOUT      g_wo;
static WAVEHDR       g_hdr[NBUF];
static short        *g_buf[NBUF];
static int           g_queued[NBUF];
static int           g_active = 0;      /* a track is open                  */
static int           g_eof = 0;
static int           g_ended = 0;       /* TRACKEND already posted          */
static unsigned long long g_seekBase = 0;  /* frames before last reset      */
static volatile LONG g_volQ8 = 179;     /* software volume, 0..256          */

static HBITMAP g_cover = NULL;          /* current track's album art        */
static int     g_coverW = 0, g_coverH = 0;

/* fill and queue every free buffer; returns 1 while audio is still flowing */
static void feeder_fill(void)
{
    if (!g_active || !g_wo) return;
    int vol = (int)g_volQ8;
    for (int i = 0; i < NBUF; ++i) {
        if (g_queued[i]) {
            if (g_hdr[i].dwFlags & WHDR_DONE) g_queued[i] = 0;
            else continue;
        }
        if (g_eof) continue;
        int n = src_read(&g_src, g_buf[i], BUF_FRAMES);
        if (n <= 0) { g_eof = 1; continue; }
        short *p = g_buf[i];
        int cnt = n * g_src.ch;
        if (vol != 256)
            for (int k = 0; k < cnt; ++k) p[k] = (short)((p[k] * vol) >> 8);
        g_hdr[i].dwBufferLength = cnt * sizeof(short);
        g_hdr[i].dwFlags &= ~WHDR_DONE;
        if (waveOutWrite(g_wo, &g_hdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
            g_queued[i] = 1;
        else g_eof = 1;
    }
    if (g_eof && !g_ended) {
        int busy = 0;
        for (int i = 0; i < NBUF; ++i)
            if (g_queued[i] && !(g_hdr[i].dwFlags & WHDR_DONE)) busy = 1;
        if (!busy) {
            g_ended = 1;
            if (g_hMain) PostMessageW(g_hMain, WM_APP_TRACKEND, 0, 0);
        }
    }
}

static DWORD WINAPI music_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_mevent, 100);
        if (g_mquit) return 0;
        EnterCriticalSection(&g_mcs);
        feeder_fill();
        LeaveCriticalSection(&g_mcs);
    }
}

static void engine_ensure(void)
{
    if (g_mthread) return;
    InitializeCriticalSection(&g_mcs);
    g_mevent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_mthread = CreateThread(NULL, 0, music_thread, NULL, 0, NULL);
}

/* close the waveOut device + source (g_mcs must be held) */
static void engine_close(void)
{
    if (g_wo) {
        waveOutReset(g_wo);
        for (int i = 0; i < NBUF; ++i) {
            if (g_hdr[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof(WAVEHDR));
            free(g_buf[i]); g_buf[i] = NULL;
            g_queued[i] = 0;
        }
        waveOutClose(g_wo);
        g_wo = NULL;
    }
    if (g_active) src_close(&g_src);
    g_active = 0; g_eof = 0; g_ended = 0; g_seekBase = 0;
}

/* ---------- cover art ---------- */

static void exe_dir(wchar_t *out)        /* with trailing backslash */
{
    DWORD n = GetModuleFileNameW(NULL, out, MAX_PATH);
    wchar_t *s = (n && n < MAX_PATH) ? wcsrchr(out, L'\\') : NULL;
    if (s) s[1] = 0; else out[0] = 0;
}

static void cover_load(int idx)
{
    if (g_cover) { DeleteObject(g_cover); g_cover = NULL; }
    g_coverW = g_coverH = 0;
    if (idx < 0 || idx >= g_trackCount) return;
    static const wchar_t *ext[] = { L"png", L"jpg", L"jpeg", L"bmp" };
    wchar_t dir[MAX_PATH], path[MAX_PATH + 160];
    exe_dir(dir);
    for (int pass = 0; pass < 2 && !g_cover; ++pass) {
        const wchar_t *base = pass == 0 ? g_tracks[idx].name : L"default";
        for (int e = 0; e < 4 && !g_cover; ++e) {
            _snwprintf(path, MAX_PATH + 160, L"%lscover\\%ls.%ls", dir, base, ext[e]);
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
                g_cover = bitmap_from_file(path, &g_coverW, &g_coverH, 0xFF20242C);
        }
    }
}

HBITMAP music_cover(int *w, int *h) { *w = g_coverW; *h = g_coverH; return g_cover; }

/* ---------- public API (UI thread) ---------- */

static void scan_ext(const wchar_t *dir, const wchar_t *pattern)
{
    wchar_t spec[MAX_PATH];
    _snwprintf(spec, MAX_PATH, L"%ls\\%ls", dir, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_trackCount >= MAX_TRACKS) break;
        Track *t = &g_tracks[g_trackCount];
        _snwprintf(t->path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        wcsncpy(t->name, fd.cFileName, 127); t->name[127] = 0;
        wchar_t *dot = wcsrchr(t->name, L'.');
        if (dot) *dot = 0;
        g_trackCount++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int track_cmp(const void *a, const void *b)
{
    return _wcsicmp(((const Track *)a)->name, ((const Track *)b)->name);
}

void music_scan(void)
{
    g_trackCount = 0;
    wchar_t dir[MAX_PATH];
    exe_dir(dir);
    wcsncat(dir, L"music", MAX_PATH - wcslen(dir) - 1);
    scan_ext(dir, L"*.mp3");
    scan_ext(dir, L"*.wav");
    qsort(g_tracks, g_trackCount, sizeof(Track), track_cmp);
}

void music_stop(void)
{
    if (!g_mthread) return;
    EnterCriticalSection(&g_mcs);
    engine_close();
    LeaveCriticalSection(&g_mcs);
    g_playing = 0;
}

void music_shutdown(void)
{
    music_stop();
    if (g_mthread) {
        g_mquit = 1;
        SetEvent(g_mevent);
        WaitForSingleObject(g_mthread, 1000);
        CloseHandle(g_mthread);
        g_mthread = NULL;
    }
    if (g_cover) { DeleteObject(g_cover); g_cover = NULL; }
}

void music_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    InterlockedExchange(&g_volQ8, v * 256 / 100);
}

int music_play(int idx)
{
    if (idx < 0 || idx >= g_trackCount) return 0;
    engine_ensure();
    music_stop();

    EnterCriticalSection(&g_mcs);
    int ok = 0;
    if (src_open(&g_src, g_tracks[idx].path)) {
        WAVEFORMATEX wf;
        memset(&wf, 0, sizeof wf);
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = (WORD)g_src.ch;
        wf.nSamplesPerSec = (DWORD)g_src.hz;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)(g_src.ch * 2);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, (DWORD_PTR)g_mevent, 0,
                        CALLBACK_EVENT) == MMSYSERR_NOERROR) {
            ok = 1;
            for (int i = 0; i < NBUF; ++i) {
                g_buf[i] = (short *)malloc(BUF_FRAMES * g_src.ch * sizeof(short));
                memset(&g_hdr[i], 0, sizeof(WAVEHDR));
                g_hdr[i].lpData = (LPSTR)g_buf[i];
                g_hdr[i].dwBufferLength = BUF_FRAMES * g_src.ch * sizeof(short);
                waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof(WAVEHDR));
                g_queued[i] = 0;
            }
            g_active = 1; g_eof = 0; g_ended = 0; g_seekBase = 0;
            music_set_volume(g_cfg.musicVol);
            feeder_fill();                      /* prime the first buffers */
        } else {
            src_close(&g_src);
        }
    }
    LeaveCriticalSection(&g_mcs);

    if (!ok) return 0;
    g_curTrack = idx;
    g_playing = 1;
    cover_load(idx);
    SetEvent(g_mevent);
    return 1;
}

void music_toggle(void)
{
    if (!g_active) {
        if (g_trackCount > 0)
            music_play(g_curTrack >= 0 ? g_curTrack : 0);
        return;
    }
    if (g_playing == 1)      { waveOutPause(g_wo);   g_playing = 2; }
    else if (g_playing == 2) { waveOutRestart(g_wo); g_playing = 1; }
}

void music_next(int dir)
{
    if (g_trackCount <= 0) return;
    int nxt;
    if (g_cfg.shuffle && g_trackCount > 1) {
        nxt = g_curTrack;
        while (nxt == g_curTrack) nxt = rand() % g_trackCount;
    } else {
        nxt = (g_curTrack < 0 ? 0 : g_curTrack + dir);
        if (nxt < 0) nxt = g_trackCount - 1;
        if (nxt >= g_trackCount) nxt = 0;
    }
    music_play(nxt);
}

int music_len_ms(void)
{
    if (!g_active || g_src.hz <= 0) return 0;
    return (int)(g_src.totalFrames * 1000 / (unsigned)g_src.hz);
}

int music_pos_ms(void)
{
    if (!g_active || !g_wo || g_src.hz <= 0) return 0;
    MMTIME mt;
    mt.wType = TIME_SAMPLES;
    if (waveOutGetPosition(g_wo, &mt, sizeof mt) != MMSYSERR_NOERROR) return 0;
    unsigned long long played =
        (mt.wType == TIME_SAMPLES) ? mt.u.sample
      : (mt.wType == TIME_BYTES)   ? mt.u.cb / (unsigned)(g_src.ch * 2) : 0;
    unsigned long long f = g_seekBase + played;
    if (f > g_src.totalFrames) f = g_src.totalFrames;
    return (int)(f * 1000 / (unsigned)g_src.hz);
}

void music_seek_ms(int ms)
{
    if (!g_active || !g_wo || g_src.hz <= 0) return;
    if (ms < 0) ms = 0;
    unsigned long long frame = (unsigned long long)ms * (unsigned)g_src.hz / 1000;
    EnterCriticalSection(&g_mcs);
    waveOutReset(g_wo);                          /* returns queued buffers   */
    for (int i = 0; i < NBUF; ++i) g_queued[i] = 0;
    if (g_playing == 2) waveOutPause(g_wo);      /* reset cleared the pause  */
    src_seek(&g_src, frame);
    g_seekBase = frame;
    g_eof = 0; g_ended = 0;
    feeder_fill();
    LeaveCriticalSection(&g_mcs);
    SetEvent(g_mevent);
}

/* the feeder drained the last buffer: decide what plays next */
void music_on_trackend(void)
{
    if (!g_active) return;
    if (g_cfg.loopMode == 2) {                   /* repeat one */
        music_play(g_curTrack);
    } else if (g_cfg.shuffle || g_cfg.loopMode == 1 ||
               (g_curTrack >= 0 && g_curTrack < g_trackCount - 1)) {
        music_next(+1);
    } else {
        music_stop();                            /* end of list, no loop */
        InvalidateRect(g_hMain, NULL, FALSE);
    }
}

/*
 * Usada Pekora Live Stream Checker (PLSC) - v0.3.7
 *
 * Win32/GDI desktop client that reports whether the VTuber Usada Pekora is
 * live on YouTube and lists upcoming streams. Single translation unit; the
 * schedule is parsed out of the channel page's embedded ytInitialData rather
 * than a public API (none is exposed for this).
 *
 * Source layout, top to bottom:
 *   PART 1  streams-page parser   (also builds standalone with -DSELFTEST)
 *   PART 2  image decode (GDI+) + HTTPS (WinINet)
 *   PART 3  language tables
 *   PART 4  drawing
 *   PART 5  layout + actions
 *   PART 6  window procedure
 *
 * Build: PLSC_build.bat (MinGW-w64 gcc + windres). Assets are embedded via
 * PLSC_res.rc; a pixel.ttf next to the .exe overrides the bundled font.
 *
 * Author: Toriyou1121
 * Bundled font: DotGothic16, SIL Open Font License 1.1.
 * Repository: https://github.com/Toriyou1121/Usada-Pekora-Live-Stream-Checker
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <wininet.h>
#include <shellapi.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <math.h>

#ifdef _MSC_VER
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "ole32.lib")
#endif

#define STREAMS_URL   L"https://www.youtube.com/@usadapekora/streams?hl=ja"
#define CHANNEL_HOME  L"https://www.youtube.com/@usadapekora"
#define JOIN_URL      L"https://www.youtube.com/@usadapekora/join"
#define GITHUB_URL    L"https://github.com/Toriyou1121/Usada-Pekora-Live-Stream-Checker"

/* ===== PART 1 - STREAMS-PAGE PARSER (also builds alone with -DSELFTEST) =====
 * No JSON library: we find each field by these literal anchor substrings in
 * the page's embedded ytInitialData, then read the value that follows. */
#define A_REGION   "\"lockupViewModel\":{"      /* start of a stream entry   */
#define A_VIDID    "\"videoId\":\""             /* the 11-char video id      */
#define A_TITLE    "\"lockupMetadataViewModel\":{\"title\":{\"content\":\""  /* stream title */
#define A_CONTENT  "\"content\":\""             /* a metadata text run       */
#define A_LIVE     "THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE"  /* present => live now */

/* "公開予定" (upcoming marker) as raw UTF-8 bytes, charset-independent. */
static const char TOK_SCHEDULED[] =
    { (char)0xE5,(char)0x85,(char)0xAC, (char)0xE9,(char)0x96,(char)0x8B,
      (char)0xE4,(char)0xBA,(char)0x88, (char)0xE5,(char)0xAE,(char)0x9A, 0x00 };

/* One parsed stream. parse_streams() fills an array of these. */
typedef struct {
    int       isLive;        /* 1 = live right now                  */
    long long sortKey;       /* -1 for live, else UTC epoch          */
    int       hasTime;       /* 1 if utc below is valid              */
    time_t    utc;           /* scheduled start, UTC (mode-agnostic) */
    wchar_t   when[64];      /* raw schedule text (parse fallback)   */
    wchar_t   title[320];
    wchar_t   url[96];
    char      id[16];
    HBITMAP   thumb;         /* cached cover (lazy)                  */
    int       tw, th;
    int       thumbState;    /* 0 untried, 1 ok, 2 failed            */
} StreamItem;

#define MAX_ITEMS 64
static StreamItem g_items[MAX_ITEMS];
static int        g_itemCount = 0;

/* ---------- text helpers: UTF-8 encode, JSON-string decode, substring search
 * The page is UTF-8 and titles may use \uXXXX escapes, so we decode to UTF-8
 * then widen to wchar_t for the Win32 "W" APIs. ---------- */

static int enc_utf8(unsigned cp, char *o)
{
    if (cp < 0x80)        { o[0] = (char)cp; return 1; }
    else if (cp < 0x800)  { o[0] = (char)(0xC0 | (cp >> 6));
                            o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    else if (cp < 0x10000){ o[0] = (char)(0xE0 | (cp >> 12));
                            o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode a JSON string starting just after the opening quote into UTF-8. */
static void json_str(const char *p, char *out, int cap)
{
    int o = 0;
    while (*p && o < cap - 4) {
        char c = *p++;
        if (c == '"') break;
        if (c == '\\') {
            char e = *p++;
            switch (e) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': case 'b': case 'f': break;
            case '"': out[o++] = '"';  break;
            case '\\':out[o++] = '\\'; break;
            case '/': out[o++] = '/';  break;
            case 'u': {
                char hx[5] = { p[0], p[1], p[2], p[3], 0 };
                unsigned cp = (unsigned)strtoul(hx, NULL, 16);
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    char hx2[5] = { p[2], p[3], p[4], p[5], 0 };
                    unsigned lo = (unsigned)strtoul(hx2, NULL, 16);
                    p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                o += enc_utf8(cp, out + o);
                break;
            }
            default: out[o++] = e; break;
            }
        } else out[o++] = c;
    }
    out[o] = '\0';
}

static void u8_to_w(const char *u8, wchar_t *dst, int dstCap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, dst, dstCap);
    if (n <= 0) dst[0] = L'\0';
}

static const char *find_in(const char *s, const char *e, const char *needle)
{
    const char *p = strstr(s, needle);
    return (p && p < e) ? p : NULL;
}

static const char *rfind_before(const char *base, const char *pos, const char *needle)
{
    size_t nl = strlen(needle);
    if ((size_t)(pos - base) < nl) return NULL;
    for (const char *q = pos - nl; q >= base; --q)
        if (memcmp(q, needle, nl) == 0) return q;
    return NULL;
}

/* ---------- the parser itself ----------
 * parse_streams() walks every "lockupViewModel" block, keeps the ones that are
 * live or upcoming, extracts id/title/scheduled-time, de-duplicates, and sorts
 * them live-first then by start time. Result lands in g_items / g_itemCount. */

static int already_have(const char *id)
{
    for (int i = 0; i < g_itemCount; ++i)
        if (strcmp(g_items[i].id, id) == 0) return 1;
    return 0;
}

static void format_when(const char *schedUtf8, StreamItem *it)
{
    /* YouTube is asked (via the tz cookie) to render these in JST. We turn the
     * JST wall-clock into a real UTC epoch so the GUI can show it in JST OR in
     * the user's own timezone. JST is a fixed UTC+9 (no DST). */
    wchar_t raw[128];
    u8_to_w(schedUtf8, raw, 128);
    int y, mo, d, h, mi;
    if (swscanf(raw, L"%d/%d/%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
        struct tm t;
        memset(&t, 0, sizeof t);
        t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
        t.tm_hour = h; t.tm_min = mi;
        time_t asUTC = _mkgmtime(&t);            /* treat components as UTC */
        if (asUTC != (time_t)-1) {
            it->utc = asUTC - 9 * 3600;          /* ...they were JST -> real UTC */
            it->hasTime = 1;
            it->sortKey = (long long)it->utc;
        } else {
            it->sortKey = 4000000000000LL;
        }
        _snwprintf(it->when, 64, L"%02d/%02d %02d:%02d", mo, d, h, mi);
    } else {                                     /* keep YouTube's raw text */
        wcsncpy(it->when, raw, 63);
        it->when[63] = L'\0';
        it->sortKey = 4000000000000LL;           /* unknown times sort last */
    }
}

static void parse_streams(const char *html)
{
    g_itemCount = 0;
    const char *end = html + strlen(html);
    const char *r = strstr(html, A_REGION);
    while (r && g_itemCount < MAX_ITEMS) {
        const char *next = strstr(r + 1, A_REGION);
        const char *e    = next ? next : end;

        int isLive = (find_in(r, e, A_LIVE) != NULL);
        const char *sched = find_in(r, e, TOK_SCHEDULED);

        if (isLive || sched) {
            StreamItem it;
            memset(&it, 0, sizeof it);
            it.isLive = isLive;

            const char *vp = find_in(r, e, A_VIDID);
            if (vp) {
                vp += strlen(A_VIDID);
                int k = 0;
                while (k < 15 && vp[k] && vp[k] != '"') { it.id[k] = vp[k]; k++; }
                it.id[k] = '\0';
            }

            char tu8[1024] = "";
            const char *tp = find_in(r, e, A_TITLE);
            if (tp) json_str(tp + strlen(A_TITLE), tu8, sizeof tu8);
            u8_to_w(tu8[0] ? tu8 : "(no title)", it.title, 320);

            if (isLive) { it.sortKey = -1; it.when[0] = L'\0'; }
            else {
                const char *cp = rfind_before(r, sched, A_CONTENT);
                char su8[256] = "";
                if (cp) json_str(cp + strlen(A_CONTENT), su8, sizeof su8);
                format_when(su8, &it);
            }

            if (it.id[0])
                _snwprintf(it.url, 96, L"https://www.youtube.com/watch?v=%hs", it.id);
            else
                wcscpy(it.url, CHANNEL_HOME);

            if (!it.id[0] || !already_have(it.id))
                g_items[g_itemCount++] = it;
        }
        r = next;
    }
    /* live first, then upcoming by time ascending */
    for (int i = 0; i < g_itemCount - 1; ++i)
        for (int j = i + 1; j < g_itemCount; ++j)
            if (g_items[j].sortKey < g_items[i].sortKey) {
                StreamItem t = g_items[i]; g_items[i] = g_items[j]; g_items[j] = t;
            }
}

/* ===== GUI HALF (skipped under -DSELFTEST) - Win32 + GDI, owner-drawn ===== */
#ifndef SELFTEST

/* ---- control ids & timer id ---- */
#define ID_BTN_CHECK 101
#define ID_BTN_CHAN  102
#define ID_BTN_LANG  103
#define ID_BTN_JOIN  104
#define ID_BTN_SET   105      /* settings (resolution) gear            */
#define ID_BTN_GIT   106      /* GitHub project link                   */
#define ID_BTN_TIME  107      /* schedule timezone toggle (clock icon) */
#define ID_PREV      110
#define ID_NEXT      111
#define ID_TIMER     1
/* resolution menu command ids (client width x height) */
#define ID_RES_720   201      /* 1280 x 720  */
#define ID_RES_1080  202      /* 1920 x 1080 */
#define ID_RES_1440  203      /* 2560 x 1440 */
#define ID_FULLSCRN  204      /* full screen (menu + F11)              */
#define ID_TIME_JST  205      /* schedule in Japan time                */
#define ID_TIME_SYS  206      /* schedule in system time               */

/* ---- palette (Pekora blue / sakura pink) ---- */
#define C_INDIGO   RGB(74, 132, 200)    /* title bar (Pekora blue)        */
#define C_INDIGO2  RGB(126, 176, 226)   /* title bar pattern              */
#define C_PEKO     RGB(64, 146, 222)    /* accent blue                    */
#define C_PEKODK   RGB(40, 110, 188)
#define C_RED      RGB(235, 102, 120)   /* soft pink-red (LIVE)           */
#define C_GREEN    RGB(74, 192, 150)    /* membership join                */
#define C_SAKURA   RGB(255, 170, 200)   /* sakura pink                    */
#define C_SAKURA_D RGB(240, 128, 170)
#define C_SKY      RGB(196, 228, 255)
#define C_SKY_L    RGB(226, 243, 255)   /* board bg top                   */
#define C_PINK_L   RGB(255, 233, 243)   /* board bg bottom                */
#define C_NAVY     RGB(54, 76, 122)     /* main board text                */
#define C_NAVY_DIM RGB(126, 146, 180)
#define C_TILEBG   RGB(255, 255, 255)
#define C_TILEBORD RGB(150, 200, 240)
#define C_PAPER_T  RGB(230, 244, 255)   /* window bg top                  */
#define C_PAPER_B  RGB(255, 238, 246)   /* window bg bottom               */

#define IMG_PEKORA  101
#define IMG_PIXEL   200      /* embedded pixel font (RCDATA) */
#define IMG_GIF     300      /* embedded running-Pekora loading GIF (RCDATA) */

/* ===== PART 2 - IMAGES & NETWORK: GDI+ image decode + WinINet HTTPS GET ===== */

/* GDI+ "flat" C entry points (the normal gdiplus.h header is C++ only). */
typedef struct { UINT32 v; void *cb; BOOL sb; BOOL se; } GpStartupInput;
__declspec(dllimport) int  WINAPI GdiplusStartup(ULONG_PTR*, const GpStartupInput*, void*);
__declspec(dllimport) void WINAPI GdiplusShutdown(ULONG_PTR);
__declspec(dllimport) int  WINAPI GdipCreateBitmapFromStream(void*, void**);
__declspec(dllimport) int  WINAPI GdipCreateHBITMAPFromBitmap(void*, HBITMAP*, DWORD);
__declspec(dllimport) int  WINAPI GdipGetImageWidth(void*, UINT*);
__declspec(dllimport) int  WINAPI GdipGetImageHeight(void*, UINT*);
__declspec(dllimport) int  WINAPI GdipDisposeImage(void*);
/* multi-frame (animated GIF) decode: pick each frame, then snapshot to HBITMAP */
__declspec(dllimport) int  WINAPI GdipImageGetFrameCount(void*, const GUID*, UINT*);
__declspec(dllimport) int  WINAPI GdipImageSelectActiveFrame(void*, const GUID*, UINT);
/* the GIF's per-frame "time" dimension GUID (FrameDimensionTime) */
static const GUID GIF_DIM_TIME =
    { 0x6aedbd6d, 0x3fb5, 0x418a, { 0x83,0xa6,0x7f,0x45,0x22,0x9d,0xc8,0x72 } };

static HBITMAP bitmap_from_memory(const void *data, DWORD size,
                                  int *w, int *h, DWORD bg)
{
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return NULL;
    void *mp = GlobalLock(hMem);
    memcpy(mp, data, size);
    GlobalUnlock(hMem);

    IStream *stm = NULL;
    if (CreateStreamOnHGlobal(hMem, TRUE, &stm) != S_OK) { GlobalFree(hMem); return NULL; }

    void *bmp = NULL;
    HBITMAP hb = NULL;
    if (GdipCreateBitmapFromStream(stm, &bmp) == 0 && bmp) {
        UINT uw = 0, uh = 0;
        GdipGetImageWidth(bmp, &uw);
        GdipGetImageHeight(bmp, &uh);
        GdipCreateHBITMAPFromBitmap(bmp, &hb, bg);
        GdipDisposeImage(bmp);
        if (hb) { *w = (int)uw; *h = (int)uh; }
    }
    stm->lpVtbl->Release(stm);   /* frees hMem */
    return hb;
}

static HBITMAP load_image_res(WORD id, int *w, int *h, DWORD bg)
{
    HRSRC hr = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!hr) return NULL;
    DWORD sz = SizeofResource(NULL, hr);
    void *data = LockResource(LoadResource(NULL, hr));
    if (!data || !sz) return NULL;
    return bitmap_from_memory(data, sz, w, h, bg);
}

/* Decode an embedded animated GIF into one 32-bit HBITMAP per frame (alpha
 * kept, so the frames AlphaBlend cleanly over the board gradient). Returns the
 * frame count; fills out[] (up to cap) plus the pixel width/height. */
#define MAX_GIF_FRAMES 64
static int load_gif_frames_res(WORD id, HBITMAP *out, int cap, int *w, int *h)
{
    *w = *h = 0;
    HRSRC hr = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!hr) return 0;
    DWORD sz = SizeofResource(NULL, hr);
    void *data = LockResource(LoadResource(NULL, hr));
    if (!data || !sz) return 0;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sz);
    if (!hMem) return 0;
    void *mp = GlobalLock(hMem);
    memcpy(mp, data, sz);
    GlobalUnlock(hMem);

    IStream *stm = NULL;
    if (CreateStreamOnHGlobal(hMem, TRUE, &stm) != S_OK) { GlobalFree(hMem); return 0; }

    void *bmp = NULL;
    int n = 0;
    if (GdipCreateBitmapFromStream(stm, &bmp) == 0 && bmp) {
        UINT uw = 0, uh = 0, frames = 1;
        GdipGetImageWidth(bmp, &uw);
        GdipGetImageHeight(bmp, &uh);
        if (GdipImageGetFrameCount(bmp, &GIF_DIM_TIME, &frames) != 0 || frames < 1)
            frames = 1;
        if (frames > (UINT)cap) frames = (UINT)cap;
        for (UINT i = 0; i < frames; ++i) {
            if (GdipImageSelectActiveFrame(bmp, &GIF_DIM_TIME, i) != 0) break;
            HBITMAP hb = NULL;
            if (GdipCreateHBITMAPFromBitmap(bmp, &hb, 0x00000000) == 0 && hb)
                out[n++] = hb;
        }
        GdipDisposeImage(bmp);
        if (n > 0) { *w = (int)uw; *h = (int)uh; }
    }
    stm->lpVtbl->Release(stm);   /* frees hMem */
    return n;
}

/* HTTPS GET into a malloc'd buffer (works for HTML text and binary JPGs).
 * Uses the machine's configured proxy, so a system VPN/proxy is honored.
 * The PREF=tz cookie makes YouTube render schedule times in JST server-side
 * (so they match what a viewer in Japan sees, regardless of our timezone). */
static char *fetch_url(const wchar_t *url, size_t *out_size)
{
    *out_size = 0;
    HINTERNET hInet = InternetOpenW(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) PekoraLiveChecker/3.0",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInet) return NULL;
    /* tell YouTube to format times in Japan time (WinINet cookie jar) */
    InternetSetCookieW(L"https://www.youtube.com/", NULL, L"PREF=tz=Asia.Tokyo");
    HINTERNET hUrl = InternetOpenUrlW(hInet, url,
        L"Accept-Language: ja,en;q=0.8\r\n", (DWORD)-1,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_UI, 0);
    if (!hUrl) { InternetCloseHandle(hInet); return NULL; }

    size_t cap = 1 << 20, total = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { InternetCloseHandle(hUrl); InternetCloseHandle(hInet); return NULL; }
    DWORD nread = 0;
    while (InternetReadFile(hUrl, buf + total, (DWORD)(cap - total - 1), &nread) && nread > 0) {
        total += nread;
        if (cap - total < 65536) {
            size_t nc = cap * 2; char *nb = (char *)realloc(buf, nc);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb; cap = nc;
        }
    }
    if (buf) { buf[total] = '\0'; *out_size = total; }
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInet);
    return buf;
}

/* ===== PART 3 - LANGUAGE PACKS: every visible string in EN / JA / ZH =====
 * The Lang button cycles g_lang through LANGS[]; fonts switch to match. */

typedef enum { LANG_EN = 0, LANG_JA = 1, LANG_ZH = 2, LANG_COUNT } LangId;

typedef struct {
    const wchar_t *window_title;
    const wchar_t *board_title;   /* (legacy header caption; now shows the date) */
    const wchar_t *btn_check;
    const wchar_t *btn_chan;
    const wchar_t *btn_lang;
    const wchar_t *btn_join;
    const wchar_t *checking;
    const wchar_t *err_network;
    const wchar_t *initial;
    const wchar_t *st_live;
    const wchar_t *st_notlive;
    const wchar_t *up_suffix;     /* printf %d */
    const wchar_t *none_txt;
    const wchar_t *chat_label;
    const wchar_t *cover_live;
    const wchar_t *cover_soon;
    const wchar_t *no_cover;
    const wchar_t *hint;
    const wchar_t *beta;          /* "still in beta" reminder */
    const wchar_t *tm_jst;        /* time menu: Japan time    */
    const wchar_t *tm_sys;        /* time menu: system time   */
    const wchar_t *tz_local;      /* per-row label in JST mode (Japan local) */
    const wchar_t *weekday[7];    /* Sun..Sat, for the date by the clock     */
} LangPack;

static const LangPack LANGS[LANG_COUNT] = {
    {   /* English */
        L"Usada Pekora Live Stream Checker  v0.3.7", L"PEKORA STREAM BOARD",
        L"Check", L"Channel", L"English", L"Join",
        L"CONNECTING TO YOUTUBE ...",
        L"CONNECTION ERROR - YouTube unreachable.\nOffline, or blocked here "
        L"(e.g. mainland China) - a working VPN / proxy is required.",
        L"Press  CHECK  to load Pekora's live & upcoming schedule.",
        L"ON AIR NOW", L"OFFLINE", L"  (+%d upcoming)",
        L"No live or upcoming streams. Pekora is on a break, peko!",
        L"chat", L"LIVE", L"SOON", L"NO COVER",
        L"Click \"watch\" for the stream, or \"chat\" for live chat.",
        L"Beta version (v0.3.7) - still under development, peko!",
        L"Japan time (JST)", L"My computer's time",
        L"Japan local time",
        { L"Sunday", L"Monday", L"Tuesday", L"Wednesday",
          L"Thursday", L"Friday", L"Saturday" }
    },
    {   /* Japanese */
        L"兎田ぺこら ライブ配信チェッカー  v0.3.7", L"ぺこら 配信ボード",
        L"確認", L"チャンネル", L"日本語", L"メンバー",
        L"YouTube に接続中 ...",
        L"接続エラー - YouTube に接続できません。\nオフライン、または規制中 "
        L"(例: 中国本土)。VPN / プロキシが必要です。",
        L"「確認」を押すと配信中・配信予定を読み込みます。",
        L"配信中", L"オフライン", L"  (ほか予定 %d 件)",
        L"配信中・配信予定はありません。ぺこらは休憩中ぺこ!",
        L"チャット", L"LIVE", L"予定", L"カバーなし",
        L"「watch」で配信ページ、「チャット」でライブチャットへ。",
        L"ベータ版 (v0.3.7) - まだ開発中ぺこ!",
        L"日本時間 (JST)", L"このPCの時間",
        L"現地時間",
        { L"日曜日", L"月曜日", L"火曜日", L"水曜日",
          L"木曜日", L"金曜日", L"土曜日" }
    },
    {   /* Traditional Chinese */
        L"兔田佩克拉 直播檢查器  v0.3.7", L"佩克拉 直播看板",
        L"檢查", L"頻道", L"繁體中文", L"加入會員",
        L"正在連線 YouTube ...",
        L"連線錯誤 - 無法連線 YouTube。\n離線,或此處被封鎖 "
        L"(例如中國大陸),需要可用的 VPN / 代理。",
        L"按「檢查」載入佩克拉的直播與預定開播。",
        L"直播中", L"未開播", L"  (另有預定 %d 場)",
        L"目前沒有直播或預定開播。佩克拉休息中 peko!",
        L"聊天室", L"LIVE", L"預定", L"無封面",
        L"點「watch」開啟直播頁面,「聊天室」開啟即時聊天。",
        L"Beta 測試版 (v0.3.7) - 仍在開發中 peko!",
        L"日本時間 (JST)", L"本機時間",
        L"當地時間",
        { L"星期日", L"星期一", L"星期二", L"星期三",
          L"星期四", L"星期五", L"星期六" }
    }
};

/* ---------- runtime state & globals (single-window app, so all static) ---------- */
typedef enum { ST_NONE, ST_CHECKING, ST_OK, ST_ERROR } StatusState;

static int         g_lang  = LANG_JA;         /* default UI language          */
static StatusState g_state = ST_NONE;

/* Lang button cycle order: Japanese -> English -> Traditional Chinese.
 * (LANGS[] stays index-stable; only the visiting order changes.) */
static const int LANG_ORDER[LANG_COUNT] = { LANG_JA, LANG_EN, LANG_ZH };
static int lang_next(int cur)
{
    for (int i = 0; i < LANG_COUNT; ++i)
        if (LANG_ORDER[i] == cur)
            return LANG_ORDER[(i + 1) % LANG_COUNT];
    return LANG_ORDER[0];
}
static int         g_coverIndex = 0;

/* animation timing (all millisecond timestamps from GetTickCount) */
#define ANIM_MS   16                  /* ~60 fps timer (smooth)              */
#define FLIP_MS   300                 /* clock digit card-flip               */
#define SLIDE_MS  280                 /* cover slide                         */
#define REVEAL_STAGGER 70             /* delay between schedule rows          */

/* Split-flap board text effect: brightness/alpha jitter only, no flip or
 * scroll. One path drives two states:
 *   reveal - a row appearing: hidden until its turn, then a brief flicker
 *            (occasional dropped frame, brightness dip, <=1px jitter), settle.
 *   relock - visible text whose content changed (timezone/language switch):
 *            a shorter flicker, settle.
 * Tunables: */
#define FX_REVEAL_MS   130            /* reveal flicker length   (50-150 ms)  */
#define FX_RELOCK_MS   110            /* content-change relock   (80-120 ms)  */
#define FX_FRAME_MS    34             /* re-roll the random state ~every 2 frames */
#define FX_JITTER_PX   1              /* max micro-offset, device px          */
#define C_FX_BG  RGB(245, 243, 250)   /* tone the text fades toward when dimmed */

/* per-element result for one frame: whether to draw, a tiny offset, and how
 * far to fade the text toward the board background (0 = full colour). */
typedef struct { int hidden; int dx, dy; double dim; } FxState;

static wchar_t     g_clockStr[16] = L"00:00:00";
static wchar_t     g_clockOld[16] = L"00:00:00";
static DWORD       g_clockChg[16];            /* per-digit flip start time   */
static int         g_phrase = 0;              /* current kawaii phrase       */
static DWORD       g_phraseMs, g_coverMs;     /* last phrase / cover switch  */

static int         g_coverFrom = 0;           /* slide source index          */
static int         g_coverDir = 1;            /* slide direction (+1/-1)     */
static DWORD       g_coverSlideMs = 0;        /* slide start (0 = none)      */
static DWORD       g_revealMs = 0;            /* schedule reveal start       */

static HWND  g_hMain, g_hCheck, g_hChan, g_hLang, g_hJoin, g_hPrev, g_hNext, g_hSet, g_hGit, g_hTime;
static int   g_timeMode = 0;                  /* 0 = JST, 1 = system time     */
static HFONT g_fTitle, g_fUI, g_fSmall, g_fBoard, g_fClock, g_fTag, g_fDate;
static HBITMAP g_hPekora; static int g_pkW, g_pkH;
static ULONG_PTR g_gdip;

/* running-Pekora loading GIF: decoded once into per-frame bitmaps, advanced on
 * the animation timer, and shown on the board only after the first Check.   */
static HBITMAP g_gifFrames[MAX_GIF_FRAMES];
static int     g_gifCount = 0, g_gifW = 0, g_gifH = 0;
static int     g_gifFrame = 0;                /* current frame index          */
static DWORD   g_gifMs = 0;                   /* last frame-advance time      */
static int     g_showGif = 0;                 /* 1 after Check is pressed     */
#define GIF_FRAME_MS 90                       /* ~the GIF's own frame delay   */

static int     g_pixelOn = 0;                 /* a pixel.ttf was loaded       */
static wchar_t g_pixelFace[64] = L"";         /* its family name              */
static DWORD   g_fontQual = CLEARTYPE_QUALITY;

static RECT R_title, R_board, R_cover, R_peko, R_bottom;
static RECT g_clockRect;                      /* clock tiles (window coords)  */
static int  g_clkX, g_clkY, g_clkW, g_clkH, g_clkGap;  /* clock geometry      */
static RECT g_betaRect;                       /* beta-notice strip            */
static RECT g_gifRect;                         /* loading-GIF strip on board   */

/* UI scale: driven by monitor DPI, floored so text is comfortably large.
 * S(v) scales a base pixel value; every size below goes through it. */
static double g_ui = 1.2;
#define S(v) ((int)((v) * g_ui + 0.5))

/* kawaii Pekora catchphrases, shown in a speech bubble (random) */
static const wchar_t *PHRASES[] = {
    L"こんぺこ〜！", L"ぺこ ❤", L"Yo! Yo! Yo!", L"HA↑HA↑HA↑",
    L"野うさぎ集合〜！", L"ぺこらだぴょん", L"おつぺこ〜！",
    L"がんばルビー！", L"だいじょうV！", L"ぺこ？"
};
#define PHRASE_COUNT ((int)(sizeof(PHRASES) / sizeof(PHRASES[0])))

/* clickable links on the board (window coords) */
typedef struct { RECT rect; wchar_t url[160]; } LinkHit;
static LinkHit g_links[24];
static int     g_linkCount;

/* ---------- cover thumbnails ----------
 * ensure_cover() lazily downloads one stream's thumbnail (highest resolution
 * available) and caches it on the item; set_cover() switches which one shows. */

static void ensure_cover(int i)
{
    if (i < 0 || i >= g_itemCount) return;
    StreamItem *it = &g_items[i];
    if (it->thumbState != 0 || !it->id[0]) return;
    it->thumbState = 2;
    /* prefer the highest resolution available */
    static const wchar_t *qual[] = { L"maxresdefault", L"sddefault", L"hqdefault" };
    for (int q = 0; q < 3; ++q) {
        wchar_t url[160];
        _snwprintf(url, 160, L"https://i.ytimg.com/vi/%hs/%ls.jpg", it->id, qual[q]);
        size_t sz = 0;
        char *d = fetch_url(url, &sz);
        if (d && sz > 2500) {
            HBITMAP hb = bitmap_from_memory(d, (DWORD)sz, &it->tw, &it->th, 0xFF101418);
            if (hb) { it->thumb = hb; it->thumbState = 1; free(d); return; }
        }
        if (d) free(d);
    }
}

static void set_cover(int idx)
{
    if (g_itemCount <= 0) { g_coverIndex = 0; return; }
    int old = g_coverIndex;
    int ni  = ((idx % g_itemCount) + g_itemCount) % g_itemCount;
    if (ni != old) {                 /* kick off the slide animation */
        g_coverFrom = old;
        g_coverDir  = (idx >= old) ? 1 : -1;
        g_coverSlideMs = GetTickCount();
        ensure_cover(ni);
    }
    g_coverIndex = ni;
    g_coverMs = GetTickCount();      /* restart the auto-switch countdown */
    InvalidateRect(g_hMain, &R_cover, FALSE);
}

/* ===== PART 4 - DRAWING: fonts, helpers, then each visual area ===== */

static const wchar_t *face_for_lang(void)
{
    if (g_pixelOn) return g_pixelFace;      /* a cute pixel font, if supplied */
    switch (g_lang) {
    case LANG_JA: return L"Yu Gothic UI";
    case LANG_ZH: return L"Microsoft JhengHei UI";
    default:      return L"Segoe UI";
    }
}

static HFONT mkfont(int h, int weight, const wchar_t *face)
{
    return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        g_fontQual, DEFAULT_PITCH | FF_DONTCARE, face);
}

/* Read a TrueType/OpenType font's family name (name table, nameID 1) from a
 * memory buffer, so we can use any pixel font without knowing its face name. */
static unsigned rd16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static unsigned rd32(const unsigned char *p) { return ((unsigned)p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }

static int read_ttf_family_mem(const unsigned char *b, long sz, wchar_t *out, int cap)
{
    out[0] = 0;
    if (!b || sz < 12) return 0;
    unsigned off = 0;
    if (rd32(b) == 0x74746366u && sz >= 16) off = rd32(b + 12);   /* 'ttcf' */
    if (off + 12 > (unsigned)sz) return 0;
    unsigned numT = rd16(b + off + 4), nameOff = 0;
    for (unsigned i = 0; i < numT; ++i) {
        const unsigned char *rec = b + off + 12 + i * 16;
        if (rec + 16 > b + sz) break;
        if (memcmp(rec, "name", 4) == 0) { nameOff = rd32(rec + 8); break; }
    }
    int ok = 0;
    if (nameOff && nameOff + 6 <= (unsigned)sz) {
        const unsigned char *nt = b + nameOff;
        unsigned count = rd16(nt + 2), strOff = rd16(nt + 4), best = 0;
        for (unsigned i = 0; i < count; ++i) {
            const unsigned char *r = nt + 6 + i * 12;
            if (r + 12 > b + sz) break;
            unsigned plat = rd16(r), nameID = rd16(r + 6), len = rd16(r + 8), so = rd16(r + 10);
            if (nameID != 1) continue;
            const unsigned char *s = nt + strOff + so;
            if (s + len > b + sz || len == 0) continue;
            unsigned score = (plat == 3) ? 3 : (plat == 0) ? 2 : 1;
            if (score <= best) continue;
            if (plat == 3 || plat == 0) {           /* UTF-16BE */
                int n = len / 2; if (n >= cap) n = cap - 1;
                for (int k = 0; k < n; ++k) out[k] = (wchar_t)((s[k*2] << 8) | s[k*2+1]);
                out[n] = 0;
            } else {                                 /* Mac ASCII */
                int n = len; if (n >= cap) n = cap - 1;
                for (int k = 0; k < n; ++k) out[k] = (wchar_t)s[k];
                out[n] = 0;
            }
            best = score; ok = 1;
        }
    }
    return ok && out[0];
}

/* Use the embedded pixel font (or a pixel.ttf the user drops next to the exe
 * to override it). Sets g_pixelOn + g_pixelFace so create_fonts() uses it. */
static void load_pixel_font(void)
{
    /* 1) user override: pixel.ttf next to the exe */
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t *s = wcsrchr(path, L'\\');
        if (s) {
            wcscpy(s + 1, L"pixel.ttf");
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
                AddFontResourceExW(path, FR_PRIVATE, NULL) > 0) {
                FILE *f = _wfopen(path, L"rb");
                if (f) {
                    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                    unsigned char *b = (sz > 0 && sz < 40000000) ? malloc(sz) : NULL;
                    if (b && fread(b, 1, sz, f) == (size_t)sz &&
                        read_ttf_family_mem(b, sz, g_pixelFace, 64)) {
                        g_pixelOn = 1; g_fontQual = NONANTIALIASED_QUALITY;
                    }
                    free(b); fclose(f);
                }
                if (g_pixelOn) return;
            }
        }
    }
    /* 2) embedded pixel font */
    HRSRC hr = FindResourceW(NULL, MAKEINTRESOURCEW(IMG_PIXEL), RT_RCDATA);
    if (!hr) return;
    DWORD sz = SizeofResource(NULL, hr);
    void *data = LockResource(LoadResource(NULL, hr));
    if (data && sz && read_ttf_family_mem(data, sz, g_pixelFace, 64)) {
        DWORD cnt = 0;
        if (AddFontMemResourceEx(data, sz, NULL, &cnt) && cnt > 0) {
            g_pixelOn = 1; g_fontQual = NONANTIALIASED_QUALITY;
        }
    }
}

static void create_fonts(void)
{
    if (g_fTitle) { DeleteObject(g_fTitle); DeleteObject(g_fUI);
                    DeleteObject(g_fSmall); DeleteObject(g_fBoard);
                    DeleteObject(g_fClock); DeleteObject(g_fTag);
                    DeleteObject(g_fDate); }
    const wchar_t *f = face_for_lang();
    g_fTitle = mkfont(S(30), FW_BOLD, f);
    g_fUI    = mkfont(S(20), FW_BOLD, f);
    g_fSmall = mkfont(S(16), FW_SEMIBOLD, f);
    g_fBoard = mkfont(S(21), FW_BOLD, f);
    g_fClock = mkfont(S(30), FW_BOLD, f);
    g_fTag   = mkfont(S(20), FW_BOLD, f);
    g_fDate  = mkfont(S(24), FW_BOLD, f);   /* board header date (bigger)     */
}

/* ---------- reusable drawing helpers: rounded fill, cover-fit blit,
 * sakura blossom, split-flap tiles, and a one-line styled text helper ---------- */

static void fill_round(HDC dc, int l, int t, int r, int b, int rad, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, l, t, r, b, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br);
}

/* smoothstep easing, clamped to 0..1 */
static double ease(double p)
{
    if (p <= 0) return 0;
    if (p >= 1) return 1;
    return p * p * (3.0 - 2.0 * p);
}

/* single-call vertical gradient (GradientFill, no per-band loop) */
static void grad_v(HDC dc, RECT *r, COLORREF top, COLORREF bot)
{
    TRIVERTEX v[2] = {
        { r->left,  r->top,    (COLOR16)(GetRValue(top)<<8), (COLOR16)(GetGValue(top)<<8), (COLOR16)(GetBValue(top)<<8), 0 },
        { r->right, r->bottom, (COLOR16)(GetRValue(bot)<<8), (COLOR16)(GetGValue(bot)<<8), (COLOR16)(GetBValue(bot)<<8), 0 }
    };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

static void cover_blit(HDC dc, HBITMAP bmp, int iw, int ih,
                       int dx, int dy, int dw, int dh)
{
    HDC m = CreateCompatibleDC(dc);
    HGDIOBJ ob = SelectObject(m, bmp);
    double sx = (double)dw / iw, sy = (double)dh / ih;
    double sc = sx > sy ? sx : sy;
    int sw = (int)(dw / sc), sh = (int)(dh / sc);
    int sxo = (iw - sw) / 2, syo = (ih - sh) / 2;
    SetStretchBltMode(dc, HALFTONE);
    StretchBlt(dc, dx, dy, dw, dh, m, sxo, syo, sw, sh, SRCCOPY);
    SelectObject(m, ob);
    DeleteDC(m);
}

/* A little 5-petal sakura blossom. */
static void draw_sakura(HDC dc, int cx, int cy, int r, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    for (int k = 0; k < 5; ++k) {
        double a = k * 6.28318 / 5.0 - 1.5708;
        int px = cx + (int)(r * cos(a)), py = cy + (int)(r * sin(a));
        int pr = r * 3 / 5;
        Ellipse(dc, px - pr, py - pr, px + pr, py + pr);
    }
    HBRUSH ce = CreateSolidBrush(RGB(255, 232, 120));
    SelectObject(dc, ce);
    Ellipse(dc, cx - r / 4, cy - r / 4, cx + r / 4, cy + r / 4);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(br); DeleteObject(ce);
}

/* Draw a string as split-flap tiles (clean, no flicker). */
static void draw_text(HDC dc, const wchar_t *s, RECT *r, HFONT f,
                      COLORREF c, UINT fmt)
{
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, r, fmt);
    SelectObject(dc, of);
}

/* Board text FX, GDI only. Each element has a start tick and a salt (so
 * neighbours flicker out of phase). Time is quantised into FX_FRAME_MS slots:
 * within a slot the hashed result is constant, so repaints are cheap and the
 * jitter steps only a few times across the effect rather than every frame. */
static unsigned fx_hash(unsigned v)
{
    v ^= v >> 16; v *= 0x7feb352dU; v ^= v >> 15; v *= 0x846ca68bU; v ^= v >> 16;
    return v;
}

/* col toward C_FX_BG by t in 0..1 (t=0 keeps col, t=1 == background) */
static COLORREF fx_fade(COLORREF col, double t)
{
    if (t <= 0) return col;
    if (t > 1) t = 1;
    int r = (int)(GetRValue(col) + (GetRValue(C_FX_BG) - GetRValue(col)) * t);
    int g = (int)(GetGValue(col) + (GetGValue(C_FX_BG) - GetGValue(col)) * t);
    int b = (int)(GetBValue(col) + (GetBValue(C_FX_BG) - GetBValue(col)) * t);
    return RGB(r, g, b);
}

/* Compute this element's FX state. start=0 -> no animation (fully stable).
 * isReveal picks the longer "appear" flicker vs the shorter "relock" one. */
static FxState fx_eval(DWORD now, DWORD start, unsigned salt, int isReveal)
{
    FxState s = { 0, 0, 0, 0.0 };
    if (!start) return s;
    DWORD dur = isReveal ? FX_REVEAL_MS : FX_RELOCK_MS;
    DWORD el  = now - start;
    if (el >= dur) return s;                 /* settled: steady display      */

    unsigned slot = el / FX_FRAME_MS;
    unsigned h = fx_hash(salt * 2654435761U + slot * 40503U + (isReveal ? 7u : 19u));

    /* reveal starts blank for one slot; both states drop the odd frame in the
     * first ~60% so the glyph appears to settle rather than snap on */
    if (isReveal && el < (DWORD)FX_FRAME_MS) s.hidden = 1;
    else if ((h & 7u) == 0u && el < dur * 3 / 5) s.hidden = 1;

    if (!s.hidden) {
        /* brightness dip: stronger/earlier in the effect, easing out */
        double prog = (double)el / dur;          /* 0..1 */
        double envelope = 1.0 - prog;            /* fade the instability out */
        double base = isReveal ? 0.55 : 0.35;    /* max dim toward bg        */
        s.dim = base * envelope * (0.45 + 0.55 * ((h >> 3) & 0xFF) / 255.0);
        /* <=1px micro-jitter, mostly in the first half */
        if (FX_JITTER_PX > 0 && prog < 0.6) {
            s.dx = (int)((h >> 11) % (2u * FX_JITTER_PX + 1u)) - FX_JITTER_PX;
            s.dy = (int)((h >> 17) % (2u * FX_JITTER_PX + 1u)) - FX_JITTER_PX;
        }
    }
    return s;
}

/* draw_text with the departure-board FX applied (offset + fade + blank). */
static void draw_text_fx(HDC dc, const wchar_t *s, RECT *r, HFONT f,
                         COLORREF c, UINT fmt, FxState fx)
{
    if (fx.hidden) return;
    RECT rr = *r;
    if (fx.dx | fx.dy) OffsetRect(&rr, fx.dx, fx.dy);
    draw_text(dc, s, &rr, f, fx_fade(c, fx.dim), fmt);
}

/* Tiles always render (so the board frame stays steady); only the glyph
 * flickers - dropped on a blank frame, otherwise faded toward the bg, with the
 * strip jogged up to FX_JITTER_PX. */
static void draw_flaps_fx(HDC dc, int x, int y, int cw, int ch, int gap,
                          const wchar_t *s, HFONT f, FxState fx)
{
    int ox = fx.dx, oy = fx.dy;
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    int len = (int)wcslen(s);
    COLORREF ink = fx_fade(C_NAVY, fx.dim);
    for (int i = 0; i < len; ++i) {
        int cx = x + i * (cw + gap) + ox;
        int ty = y + oy;
        fill_round(dc, cx, ty, cx + cw, ty + ch, 5, C_TILEBG);
        HPEN pen = CreatePen(PS_SOLID, 1, C_TILEBORD);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, cx, ty, cx + cw, ty + ch, 5, 5);
        MoveToEx(dc, cx + 2, ty + ch / 2, NULL);     /* split-flap seam */
        LineTo(dc, cx + cw - 2, ty + ch / 2);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
        if (!fx.hidden && s[i] != L' ') {
            wchar_t g[2] = { s[i], 0 };
            RECT rc = { cx, ty, cx + cw, ty + ch };
            SetTextColor(dc, ink);
            DrawTextW(dc, g, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(dc, of);
}

/* g_relockMs: set when persistent text changes content (timezone or language
 * switch) so visible characters do the short "lock back into place" flicker. */
static DWORD g_relockMs = 0;

/* FX for a schedule element that is revealing (staggered appear) and may also
 * be relocking. salt keeps neighbours independent. */
static FxState fx_row(DWORD now, DWORD revealStart, unsigned salt)
{
    if (revealStart && now < revealStart + FX_REVEAL_MS)
        return fx_eval(now, revealStart, salt, 1);
    if (g_relockMs && now < g_relockMs + FX_RELOCK_MS)
        return fx_eval(now, g_relockMs, salt, 0);
    FxState s = { 0, 0, 0, 0.0 }; return s;
}

/* FX for always-present text (header, clock-adjacent): relock flicker only. */
static FxState fx_persist(DWORD now, unsigned salt)
{
    if (g_relockMs && now < g_relockMs + FX_RELOCK_MS)
        return fx_eval(now, g_relockMs, salt, 0);
    FxState s = { 0, 0, 0, 0.0 }; return s;
}

/* ---------- LEFT PANEL: the split-flap schedule board ----------
 * A rounded sky-to-pink card holding the live clock, ON AIR/OFFLINE status and
 * one row per stream (time tag + title + clickable "watch" and "chat" links).
 * Clickable text regions are recorded in g_links[] for hit-testing on click. */

/* render one digit, centered, onto a cw x ch tile DC (filled tile background) */
static void glyph_tile(HDC m, int cw, int ch, wchar_t g)
{
    RECT r = { 0, 0, cw, ch };
    HBRUSH b = CreateSolidBrush(C_TILEBG);
    FillRect(m, &r, b); DeleteObject(b);
    SetBkMode(m, TRANSPARENT);
    SetTextColor(m, C_NAVY);
    HGDIOBJ of = SelectObject(m, g_fClock);
    wchar_t s[2] = { g, 0 };
    if (g != L' ') DrawTextW(m, s, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(m, of);
}

/* one split-flap tile: a real card flip from oldg to newg, p in 0..1.
 * 0..0.5  the old top half folds down to the centre;
 * 0.5..1  the new bottom half unfolds from the centre down.            */
static void draw_flap_tile(HDC dc, int cx, int y, int cw, int ch,
                           wchar_t oldg, wchar_t newg, double p)
{
    int hh = ch / 2;

    /* fast path: a settled digit needs no temp bitmaps / flip compositing */
    if (p >= 1.0) {
        fill_round(dc, cx, y, cx + cw, y + ch, 5, C_TILEBG);
        HPEN pn = CreatePen(PS_SOLID, 1, C_TILEBORD);
        HGDIOBJ ope = SelectObject(dc, pn), obe = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, cx, y, cx + cw, y + ch, 5, 5);
        HPEN se = CreatePen(PS_SOLID, 1, RGB(120, 160, 205));
        SelectObject(dc, se);
        MoveToEx(dc, cx + 1, y + hh, NULL); LineTo(dc, cx + cw - 1, y + hh);
        SelectObject(dc, ope); SelectObject(dc, obe); DeleteObject(pn); DeleteObject(se);
        if (newg != L' ') {
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, C_NAVY);
            HGDIOBJ of = SelectObject(dc, g_fClock);
            wchar_t s[2] = { newg, 0 }; RECT r = { cx, y, cx + cw, y + ch };
            DrawTextW(dc, s, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, of);
        }
        return;
    }

    HDC d1 = CreateCompatibleDC(dc), d2 = CreateCompatibleDC(dc);
    HBITMAP b1 = CreateCompatibleBitmap(dc, cw, ch), b2 = CreateCompatibleBitmap(dc, cw, ch);
    HGDIOBJ o1 = SelectObject(d1, b1), o2 = SelectObject(d2, b2);
    glyph_tile(d1, cw, ch, oldg);
    glyph_tile(d2, cw, ch, newg);

    /* NB: no clip region here - SelectClipRgn uses device coords and would
     * break under paint_clock_only's shifted viewport. The blits stay inside
     * the tile anyway, so none is needed. */
    SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);

    BitBlt(dc, cx, y, cw, hh, d2, 0, 0, SRCCOPY);              /* new top    */
    BitBlt(dc, cx, y + hh, cw, ch - hh, d1, 0, hh, SRCCOPY);   /* old bottom */
    if (p < 0.5) {
        double t = ease(p / 0.5);
        int h1 = (int)(hh * (1.0 - t));
        if (h1 > 0) StretchBlt(dc, cx, y + hh - h1, cw, h1, d1, 0, 0, cw, hh, SRCCOPY);
    } else {
        double t = ease((p - 0.5) / 0.5);
        int h2 = (int)(hh * t);
        if (h2 > 0) StretchBlt(dc, cx, y + hh, cw, h2, d2, 0, hh, cw, ch - hh, SRCCOPY);
    }

    /* tile border + centre seam */
    HPEN pen = CreatePen(PS_SOLID, 1, C_TILEBORD);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, cx, y, cx + cw, y + ch, 5, 5);
    HPEN sp = CreatePen(PS_SOLID, 1, RGB(120, 160, 205));
    SelectObject(dc, sp);
    MoveToEx(dc, cx + 1, y + hh, NULL); LineTo(dc, cx + cw - 1, y + hh);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(sp);

    SelectObject(d1, o1); SelectObject(d2, o2);
    DeleteObject(b1); DeleteObject(b2); DeleteDC(d1); DeleteDC(d2);
}

/* the live clock: each changed digit does a real card-flip over FLIP_MS */
static void draw_clock(HDC dc)
{
    DWORD now = GetTickCount();
    for (int i = 0; g_clockStr[i]; ++i) {
        int cx = g_clkX + i * (g_clkW + g_clkGap);
        double p = 1.0;
        DWORD el = now - g_clockChg[i];
        if (g_clockStr[i] != L':' && g_clockChg[i] && el < FLIP_MS)
            p = (double)el / FLIP_MS;
        draw_flap_tile(dc, cx, g_clkY, g_clkW, g_clkH, g_clockOld[i], g_clockStr[i], p);
    }
}

/* repaint just the clock strip directly (cheap, flicker-free) for 60 fps flips */
static void paint_clock_only(HWND hWnd)
{
    int w = g_clockRect.right - g_clockRect.left, h = g_clockRect.bottom - g_clockRect.top;
    if (w <= 0 || h <= 0) return;
    HDC hdc = GetDC(hWnd);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ obm = SelectObject(mem, bm);
    BitBlt(mem, 0, 0, w, h, hdc, g_clockRect.left, g_clockRect.top, SRCCOPY);
    SetViewportOrgEx(mem, -g_clockRect.left, -g_clockRect.top, NULL);
    draw_clock(mem);
    SetViewportOrgEx(mem, 0, 0, NULL);
    BitBlt(hdc, g_clockRect.left, g_clockRect.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, obm); DeleteObject(bm); DeleteDC(mem);
    ReleaseDC(hWnd, hdc);
}

/* add a clickable link region (already in client coords) */
static void add_link(int lx, int ty, int rx, int by, const wchar_t *url)
{
    if (g_linkCount >= 24) return;
    g_links[g_linkCount].rect = (RECT){ lx, ty, rx, by };
    wcsncpy(g_links[g_linkCount].url, url, 159);
    g_links[g_linkCount].url[159] = 0;
    g_linkCount++;
}

/* Build a "GMT+9" / "GMT+5:30" style label for the offset (in seconds) of the
 * user's own timezone at epoch e (uses localtime vs gmtime, so DST is honored
 * for that actual date). Falls back to "GMT" if the offset can't be derived. */
static void tz_offset_label(time_t e, wchar_t *out, int cap)
{
    struct tm g, l;
    struct tm *pg = gmtime(&e);
    if (!pg) { wcsncpy(out, L"GMT", cap); out[cap-1]=0; return; }
    g = *pg;
    struct tm *pl = localtime(&e);
    if (!pl) { wcsncpy(out, L"GMT", cap); out[cap-1]=0; return; }
    l = *pl;
    /* minutes east of UTC, accounting for a possible day/year rollover */
    int dmin = (l.tm_hour - g.tm_hour) * 60 + (l.tm_min - g.tm_min);
    int dday = l.tm_yday - g.tm_yday;
    if (l.tm_year != g.tm_year) dday = (l.tm_year > g.tm_year) ? 1 : -1;
    dmin += dday * 24 * 60;
    int hh = dmin / 60, mm = dmin % 60; if (mm < 0) mm = -mm;
    if (mm) _snwprintf(out, cap, L"GMT%+d:%02d", hh, mm);
    else    _snwprintf(out, cap, L"GMT%+d", hh);
    out[cap-1] = 0;
}

/* format a stream's scheduled time into "MM/DD" + "HH:MM" for the current
 * timezone mode (0 = JST, 1 = the user's system time), plus a short timezone
 * label (tzp): JST mode shows the localized "local time" note, system mode
 * shows the detected user offset (e.g. "GMT+8"). */
static void fmt_when(StreamItem *it, wchar_t *datep, wchar_t *timep, wchar_t *tzp)
{
    datep[0] = timep[0] = 0;
    if (tzp) tzp[0] = 0;
    if (it->hasTime) {
        time_t e = it->utc;
        struct tm *lt;
        if (g_timeMode == 0) { time_t j = e + 9 * 3600; lt = gmtime(&j); }
        else                   lt = localtime(&e);
        if (lt) {
            _snwprintf(datep, 8, L"%02d/%02d", lt->tm_mon + 1, lt->tm_mday);
            _snwprintf(timep, 8, L"%02d:%02d", lt->tm_hour, lt->tm_min);
            if (tzp) {
                if (g_timeMode == 0) wcscpy(tzp, LANGS[g_lang].tz_local);
                else                 tz_offset_label(e, tzp, 16);
            }
            return;
        }
    }
    swscanf(it->when, L"%7[^ ] %7s", datep, timep);   /* fallback: raw text */
}

/* displayed size of the loading GIF inside the board, scaled to "contain" the
 * available width and a height cap (0,0 if the GIF isn't being shown yet). */
static void gif_disp_size(int availW, int *dw, int *dh)
{
    *dw = *dh = 0;
    if (!(g_showGif && g_gifCount > 0 && g_gifW > 0 && g_gifH > 0)) return;
    int maxH = S(84);
    double sc = (double)availW / g_gifW;
    double s2 = (double)maxH    / g_gifH;
    if (s2 < sc) sc = s2;
    if (sc <= 0) return;
    *dw = (int)(g_gifW * sc);
    *dh = (int)(g_gifH * sc);
}

/* board footer: the running-Pekora loading GIF (shown once Check is pressed)
 * anchored to the board bottom, with the hint note just below it. Records
 * g_gifRect so the animation timer can repaint just that strip. */
static void draw_board_footer(HDC hdc, int x, int pad)
{
    const LangPack *L = &LANGS[g_lang];
    int noteH = S(20);
    int noteTop = R_board.bottom - S(4) - noteH;

    SetRectEmpty(&g_gifRect);
    int dw, dh;
    gif_disp_size(R_board.right - pad - x, &dw, &dh);
    if (dw > 0 && dh > 0) {
        int gx = x + (R_board.right - pad - x - dw) / 2;
        int gy = noteTop - S(6) - dh;
        HBITMAP frame = g_gifFrames[g_gifFrame % g_gifCount];
        if (frame) {
            HDC m = CreateCompatibleDC(hdc);
            HGDIOBJ ob = SelectObject(m, frame);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            SetStretchBltMode(hdc, HALFTONE); SetBrushOrgEx(hdc, 0, 0, NULL);
            AlphaBlend(hdc, gx, gy, dw, dh, m, 0, 0, g_gifW, g_gifH, bf);
            SelectObject(m, ob); DeleteDC(m);
        }
        g_gifRect = (RECT){ gx, gy, gx + dw, gy + dh };
    }

    RECT fr = { x, noteTop, R_board.right - pad - S(24), noteTop + noteH };
    draw_text(hdc, L->hint, &fr, g_fSmall, C_NAVY_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void draw_board(HDC hdc)
{
    int L0 = R_board.left, T0 = R_board.top;
    int bw = R_board.right - L0, bh = R_board.bottom - T0;
    if (bw < 40 || bh < 40) return;
    const LangPack *L = &LANGS[g_lang];
    g_linkCount = 0;
    DWORD now = GetTickCount();

    /* rounded card with a sky-blue -> sakura-pink vertical gradient */
    HRGN rgn = CreateRoundRectRgn(L0, T0, R_board.right + 1, R_board.bottom + 1, 20, 20);
    SelectClipRgn(hdc, rgn);
    grad_v(hdc, &R_board, C_SKY_L, C_PINK_L);
    SelectClipRgn(hdc, NULL); DeleteObject(rgn);
    /* soft border */
    HPEN bp = CreatePen(PS_SOLID, 2, C_SKY);
    HGDIOBJ obp = SelectObject(hdc, bp);
    HGDIOBJ obb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, L0, T0, R_board.right, R_board.bottom, 20, 20);
    SelectObject(hdc, obp); SelectObject(hdc, obb); DeleteObject(bp);

    int pad = S(18), x = L0 + pad, y = T0 + pad;
    draw_sakura(hdc, R_board.right - S(16), R_board.bottom - S(16), S(11), C_SAKURA);

    /* header: live clock on the right, current date + weekday (bigger) on the
     * left. The old "PEKORA STREAM BOARD" caption was redundant with the
     * window title, so the date takes its place. Clock geometry is computed
     * first so the date can be bounded to its left edge (never overlaps). */
    g_clkW = S(22); g_clkH = S(34); g_clkGap = S(3);
    int nclk = (int)wcslen(g_clockStr);
    g_clkX = R_board.right - pad - nclk * (g_clkW + g_clkGap);
    g_clkY = y - S(4);
    draw_clock(hdc);
    g_clockRect = (RECT){ g_clkX - S(3), g_clkY - S(3),
                          g_clkX + nclk * (g_clkW + g_clkGap) + S(3), g_clkY + g_clkH + S(3) };
    {
        time_t tt = time(NULL);
        struct tm *lt = localtime(&tt);
        wchar_t ds[64];
        int availW = g_clkX - S(10) - x;             /* room left of the clock */
        if (lt) {
            _snwprintf(ds, 64, L"%04d/%02d/%02d (%ls)",
                       lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                       L->weekday[lt->tm_wday % 7]);
            /* if the weekday won't fit, drop it rather than ellipsize the date */
            SIZE sz; HGDIOBJ of = SelectObject(hdc, g_fDate);
            GetTextExtentPoint32W(hdc, ds, (int)wcslen(ds), &sz);
            if (sz.cx > availW)
                _snwprintf(ds, 64, L"%04d/%02d/%02d",
                           lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
            SelectObject(hdc, of);
        } else wcscpy(ds, L"----/--/--");
        RECT hr = { x, y - S(2), g_clkX - S(10), y + S(34) };
        draw_text_fx(hdc, ds, &hr, g_fDate, C_PEKODK,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                     fx_persist(now, 1));
    }
    y += S(44);   /* pink divider moved up (was title + date = two lines)     */

    HPEN dpen = CreatePen(PS_SOLID, 2, C_SAKURA);
    HGDIOBJ opn = SelectObject(hdc, dpen);
    MoveToEx(hdc, x, y, NULL); LineTo(hdc, R_board.right - pad, y);
    SelectObject(hdc, opn); DeleteObject(dpen);
    y += S(14);

    if (g_state != ST_OK) {
        const wchar_t *msg = g_state == ST_CHECKING ? L->checking :
                             g_state == ST_ERROR    ? L->err_network : L->initial;
        int gdw0, gdh0;
        gif_disp_size(R_board.right - pad - x, &gdw0, &gdh0);
        int footH = S(22) + (gdh0 > 0 ? gdh0 + S(6) : 0);
        RECT mr = { x, y, R_board.right - pad, R_board.bottom - pad - footH };
        draw_text(hdc, msg, &mr, g_fBoard, g_state == ST_ERROR ? C_RED : C_NAVY,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
        draw_board_footer(hdc, x, pad);
        return;
    }

    /* status row (departure-board reveal: appears first, then locks in) */
    {
        FxState fx = fx_row(now, g_revealMs, 101);
        int sx = x;
        int live = (g_itemCount > 0 && g_items[0].isLive);
        int upc = 0;
        for (int i = 0; i < g_itemCount; ++i) if (!g_items[i].isLive) upc++;
        const wchar_t *tag = live ? L"ON AIR" : L"OFFLINE";
        draw_flaps_fx(hdc, sx, y, S(17), S(26), S(2), tag, g_fTag, fx);
        wchar_t st[96], suf[40] = L"";
        if (upc > 0) _snwprintf(suf, 40, L->up_suffix, upc);
        _snwprintf(st, 96, L"%ls%ls", live ? L->st_live : L->st_notlive, suf);
        RECT sr = { sx + (int)wcslen(tag) * S(19) + S(14), y, R_board.right - pad, y + S(26) };
        draw_text_fx(hdc, st, &sr, g_fBoard, live ? C_RED : C_PEKODK,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE, fx);
        y += S(38);
    }

    if (g_itemCount == 0) {
        int gdw0, gdh0;
        gif_disp_size(R_board.right - pad - x, &gdw0, &gdh0);
        int footH = S(22) + (gdh0 > 0 ? gdh0 + S(6) : 0);
        RECT nr = { x, y, R_board.right - pad, R_board.bottom - pad - footH };
        draw_text(hdc, L->none_txt, &nr, g_fBoard, C_NAVY_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK);
        draw_board_footer(hdc, x, pad);
        return;
    }

    /* reserve room at the board bottom for the loading GIF + hint note so the
     * schedule rows never overlap them */
    int gdw, gdh;
    gif_disp_size(R_board.right - pad - x, &gdw, &gdh);
    int footerH = S(22) + (gdh > 0 ? gdh + S(6) : 0);

    /* entries: TIME tag + title + [watch] [chat] links */
    int rowH = S(52);
    for (int i = 0; i < g_itemCount; ++i) {
        if (y + rowH > R_board.bottom - pad - footerH) {
            wchar_t more[32]; _snwprintf(more, 32, L"... +%d", g_itemCount - i);
            RECT mr = { x, y, R_board.right - pad, y + S(22) };
            draw_text(hdc, more, &mr, g_fSmall, C_NAVY_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        /* staggered departure-board reveal: row i lights up after (i+1) steps;
         * before its turn it stays blank (the slot is reserved). */
        DWORD rowStart = 0;
        if (g_revealMs) {
            long el = (long)(now - g_revealMs) - (long)(i + 1) * REVEAL_STAGGER;
            if (el < 0) { y += rowH; continue; }     /* not on the board yet */
            rowStart = g_revealMs + (DWORD)(i + 1) * REVEAL_STAGGER;
        }
        FxState fx = fx_row(now, rowStart, (unsigned)(i + 1));
        int rx = x;
        StreamItem *it = &g_items[i];
        wchar_t tag[8], datep[8] = L"", timep[8] = L"", tzp[16] = L"";
        if (it->isLive) wcscpy(tag, L"LIVE");
        else {
            fmt_when(it, datep, timep, tzp);
            wcsncpy(tag, timep[0] ? timep : L"--:--", 7); tag[7] = 0;
        }
        draw_flaps_fx(hdc, rx, y, S(17), S(26), S(2), tag, g_fTag, fx);

        int tx = rx + (int)wcslen(tag) * S(19) + S(14);
        wchar_t line[360];
        if (datep[0]) _snwprintf(line, 360, L"%ls  %ls", datep, it->title);
        else          wcsncpy(line, it->title, 359), line[359] = 0;
        RECT tr = { tx, y - S(3), R_board.right - pad, y + S(22) };
        draw_text_fx(hdc, line, &tr, g_fBoard, it->isLive ? C_RED : C_NAVY,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, fx);

        /* timezone note under the time/date tag: "現地時間" in JST mode, the
         * detected user offset (e.g. GMT+8) in system-time mode */
        if (!it->isLive && tzp[0]) {
            RECT zr = { rx + fx.dx, y + S(27) + fx.dy, tx - S(6), y + S(27) + S(18) };
            draw_text_fx(hdc, tzp, &zr, g_fSmall, C_NAVY_DIM,
                         DT_LEFT | DT_TOP | DT_SINGLELINE, fx);
        }

        /* links row: watch (the live/stream) + chat. These are clickable, so
         * keep their hit-rects at the settled position (FX only tweaks paint). */
        if (it->id[0]) {
            int ly = y + S(24);
            SIZE ws; HGDIOBJ of = SelectObject(hdc, g_fSmall);
            const wchar_t *wl = it->isLive ? L"\x25B6 LIVE" : L"\x25B6 watch";
            GetTextExtentPoint32W(hdc, wl, (int)wcslen(wl), &ws);
            SelectObject(hdc, of);
            RECT wr = { tx, ly, tx + ws.cx + S(6), ly + S(22) };
            draw_text_fx(hdc, wl, &wr, g_fSmall, C_PEKODK, DT_LEFT | DT_VCENTER | DT_SINGLELINE, fx);
            add_link(wr.left, wr.top, wr.right, wr.bottom, it->url);

            int cx2 = wr.right + S(20);
            SIZE cs; of = SelectObject(hdc, g_fSmall);
            GetTextExtentPoint32W(hdc, L->chat_label, (int)wcslen(L->chat_label), &cs);
            SelectObject(hdc, of);
            RECT cr = { cx2, ly, cx2 + cs.cx + S(6), ly + S(22) };
            draw_text_fx(hdc, L->chat_label, &cr, g_fSmall, C_SAKURA_D, DT_LEFT | DT_VCENTER | DT_SINGLELINE, fx);
            wchar_t curl[160];
            _snwprintf(curl, 160, L"https://www.youtube.com/live_chat?is_popout=1&v=%hs", it->id);
            add_link(cr.left, cr.top, cr.right, cr.bottom, curl);
        }
        y += rowH;
    }

    draw_board_footer(hdc, x, pad);
}

/* ---------- TOP-RIGHT: cover carousel ----------
 * The selected stream's thumbnail inside a white photo frame, with a LIVE/SOON
 * badge, title caption and "n / total" index. Auto-rotates; < > switch manually.
 * Switching slides the old photo out and the new one in (linear/eased). */

/* draw one stream's photo into img at a horizontal pixel offset (caller clips) */
static void blit_cover_photo(HDC dc, StreamItem *it, RECT *img, int ox)
{
    int iw = img->right - img->left, ih = img->bottom - img->top;
    if (it && it->thumbState == 1 && it->thumb) {
        cover_blit(dc, it->thumb, it->tw, it->th, img->left + ox, img->top, iw, ih);
    } else {
        RECT r = { img->left + ox, img->top, img->left + ox + iw, img->bottom };
        HBRUSH b = CreateSolidBrush(C_SKY_L); FillRect(dc, &r, b); DeleteObject(b);
        draw_text(dc, it ? LANGS[g_lang].no_cover : L"YouTube", &r, g_fUI, C_PEKO,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void draw_cover(HDC dc)
{
    int l = R_cover.left, t = R_cover.top;
    const LangPack *L = &LANGS[g_lang];
    const int FR = S(11), CAP = S(40);   /* frame width, bottom caption height */

    /* outer photo frame: white card with a sakura-pink edge */
    fill_round(dc, l, t, R_cover.right, R_cover.bottom, 16, C_SAKURA);
    fill_round(dc, l + 3, t + 3, R_cover.right - 3, R_cover.bottom - 3, 13, RGB(255,255,255));

    RECT img = { l + FR, t + FR, R_cover.right - FR, R_cover.bottom - FR - CAP };
    int iw = img.right - img.left, ih = img.bottom - img.top;

    StreamItem *it = (g_state == ST_OK && g_coverIndex < g_itemCount)
                     ? &g_items[g_coverIndex] : NULL;

    /* photo (with horizontal slide while switching), clipped to the frame */
    int sv = SaveDC(dc);
    IntersectClipRect(dc, img.left, img.top, img.right, img.bottom);
    DWORD nowc = GetTickCount();
    if (g_coverSlideMs && (nowc - g_coverSlideMs) < SLIDE_MS &&
        g_coverFrom >= 0 && g_coverFrom < g_itemCount) {
        int off = (int)(ease((double)(nowc - g_coverSlideMs) / SLIDE_MS) * iw);
        StreamItem *from = &g_items[g_coverFrom];
        if (g_coverDir >= 0) { blit_cover_photo(dc, from, &img, -off);
                               blit_cover_photo(dc, it,   &img, iw - off); }
        else                 { blit_cover_photo(dc, from, &img, off);
                               blit_cover_photo(dc, it,   &img, off - iw); }
    } else {
        blit_cover_photo(dc, it, &img, 0);
    }
    RestoreDC(dc, sv);
    (void)ih;

    /* inner frame line around the photo */
    HPEN pen = CreatePen(PS_SOLID, 2, C_PEKO);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, img.left - 1, img.top - 1, img.right + 1, img.bottom + 1);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);

    /* badge over the photo */
    if (it) {
        const wchar_t *bdg = it->isLive ? L->cover_live : L->cover_soon;
        COLORREF bc = it->isLive ? C_RED : C_PEKODK;
        SIZE sz; HGDIOBJ of = SelectObject(dc, g_fSmall);
        GetTextExtentPoint32W(dc, bdg, (int)wcslen(bdg), &sz);
        SelectObject(dc, of);
        int bx0 = img.left + S(8), by0 = img.top + S(8);
        fill_round(dc, bx0, by0, bx0 + sz.cx + S(22), by0 + S(26), 6, bc);
        RECT br = { bx0, by0, bx0 + sz.cx + S(22), by0 + S(26) };
        draw_text(dc, bdg, &br, g_fSmall, RGB(255,255,255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* caption + index inside the white bottom border */
    RECT cap = { l + FR, R_cover.bottom - FR - CAP + S(4), R_cover.right - S(62), R_cover.bottom - S(6) };
    draw_text(dc, it ? it->title : L"-", &cap, g_fSmall, C_NAVY,
              DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    if (g_itemCount > 0) {
        wchar_t idx[16]; _snwprintf(idx, 16, L"%d / %d", g_coverIndex + 1, g_itemCount);
        RECT ir = { R_cover.right - S(60), R_cover.bottom - FR - S(26), R_cover.right - S(10), R_cover.bottom - S(8) };
        draw_text(dc, idx, &ir, g_fSmall, C_PEKODK, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

/* ---------- BOTTOM-RIGHT: character art + speech bubble ----------
 * Pekora's embedded PNG, anchored to the corner like a stream overlay, with a
 * kawaii catchphrase bubble (rotated by the timer) filling the space at her left. */

/* a rounded speech bubble with a little tail pointing right (to Pekora) */
static void draw_bubble(HDC dc, int l, int t, int r, int b, const wchar_t *txt)
{
    fill_round(dc, l, t, r, b, 16, RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, 2, C_SAKURA);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, l, t, r, b, 16, 16);
    /* tail */
    HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ obr = SelectObject(dc, wb);
    POINT tail[3] = { { r - S(14), b - S(20) }, { r + S(10), b - S(8) }, { r - S(14), b - S(4) } };
    Polygon(dc, tail, 3);
    SelectObject(dc, op); SelectObject(dc, ob); SelectObject(dc, obr);
    DeleteObject(pen); DeleteObject(wb);
    RECT tr = { l + S(10), t, r - S(10), b };
    draw_text(dc, txt, &tr, g_fUI, C_SAKURA_D, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void draw_pekora(HDC dc)
{
    if (!g_hPekora) return;
    int rw = R_peko.right - R_peko.left, rh = R_peko.bottom - R_peko.top;
    if (rw < 20 || rh < 20) return;
    int maxh = g_pkH * 9 / 5;                  /* cap upscale (1.8x) to stay crisp */
    int dh = rh > maxh ? maxh : rh;
    int dw = g_pkW * dh / (g_pkH ? g_pkH : 1);
    if (dw > rw) { dw = rw; dh = g_pkH * dw / (g_pkW ? g_pkW : 1); }
    int dx = R_peko.right - dw, dy = R_peko.bottom - dh;   /* anchor bottom-right */

    /* kawaii speech bubble in the blank space to her left */
    int bx2 = dx - S(14);
    if (bx2 - R_peko.left > S(120)) {
        int bw2 = bx2 - R_peko.left, bh2 = S(58);
        if (bw2 > S(280)) bw2 = S(280);
        int bl = bx2 - bw2, bt = dy + S(30);
        draw_bubble(dc, bl, bt, bl + bw2, bt + bh2, PHRASES[g_phrase % PHRASE_COUNT]);
    }

    HDC m = CreateCompatibleDC(dc);
    HGDIOBJ ob = SelectObject(m, g_hPekora);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, dx, dy, dw, dh, m, 0, 0, g_pkW, g_pkH, bf);
    SelectObject(m, ob); DeleteDC(m);
}

/* ---------- window background (washi gradient) + wafu title bar ---------- */

static void draw_background(HDC dc, RECT *rc)
{
    grad_v(dc, rc, C_PAPER_T, C_PAPER_B);
}

static void draw_titlebar(HDC dc)
{
    HBRUSH br = CreateSolidBrush(C_INDIGO);
    FillRect(dc, &R_title, br); DeleteObject(br);
    /* seigaiha-ish wave dots */
    HPEN pen = CreatePen(PS_SOLID, 1, C_INDIGO2);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (int cxp = 10; cxp < R_title.right; cxp += 26)
        for (int r = 6; r <= 18; r += 6)
            Arc(dc, cxp - r, R_title.bottom - 4 - r, cxp + r, R_title.bottom - 4 + r,
                cxp - r, R_title.bottom - 4, cxp + r, R_title.bottom - 4);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
    /* carrot dot + title */
    fill_round(dc, 18, R_title.top + 18, 30, R_title.bottom - 16, 6, C_RED);
    RECT tr = { 42, R_title.top, R_title.right - S(96), R_title.bottom };
    draw_text(dc, LANGS[g_lang].window_title, &tr, g_fTitle, RGB(255,255,255),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

/* ---------- owner-drawn buttons (rounded, themed by control id) ---------- */

/* ===== Lightweight hover highlight (YouTube / Google style) ===============
 * No ripple, no expanding circle, no AlphaBlend - just a flat state-layer
 * tint folded into the button's fill colour, faded in/out by a single opacity
 * value. Dark tint on light buttons, light tint on dark ones. Clipping is
 * implicit (the existing pill/rect fill is the only thing tinted) and text is
 * drawn afterwards, so it stays crisp. One lerp per paint -> negligible CPU. */
#define HOVER_FADE_MS 150       /* fade in/out duration (120-200ms)          */
#define HOVER_OPACITY 0.14      /* state-layer strength (subtle, 0..1)       */
#define INK_MAX_BTN   12

/* per-button hover state (single window app, so static) */
typedef struct {
    HWND   hwnd;
    int    hovering;            /* pointer currently inside                  */
    double alpha;               /* current overlay opacity factor 0..1       */
} HoverFx;
static HoverFx g_btn[INK_MAX_BTN];
static int     g_btnCount = 0;
static WNDPROC g_btnProc = NULL;          /* original BUTTON class proc      */

static HoverFx *hover_for(HWND h)
{
    for (int i = 0; i < g_btnCount; ++i)
        if (g_btn[i].hwnd == h) return &g_btn[i];
    return NULL;
}

/* Blend a button colour toward its hover state-layer (dark tint for light
 * buttons, light tint for dark ones) by the current hover opacity. Returns
 * the colour to actually fill the button with this frame. */
static COLORREF hover_shade(COLORREF bg, double alpha)
{
    if (alpha <= 0.0) return bg;
    if (alpha > 1.0) alpha = 1.0;
    int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
    double t = (lum >= 140 ? 0.0 : 255.0);          /* tint target           */
    double a = alpha * HOVER_OPACITY;
    int r = (int)(GetRValue(bg) + (t - GetRValue(bg)) * a + 0.5);
    int g = (int)(GetGValue(bg) + (t - GetGValue(bg)) * a + 0.5);
    int b = (int)(GetBValue(bg) + (t - GetBValue(bg)) * a + 0.5);
    return RGB(r, g, b);
}

/* Advance every button's hover fade one timer tick; repaint only those that
 * changed (idle buttons cost nothing). Continuous opacity -> no stutter, and
 * rapid re-hovers just reverse the fade smoothly. */
static void hover_tick(void)
{
    double step = (double)ANIM_MS / HOVER_FADE_MS;
    for (int i = 0; i < g_btnCount; ++i) {
        HoverFx *f = &g_btn[i];
        double a0 = f->alpha;
        double tgt = f->hovering ? 1.0 : 0.0;
        if (f->alpha < tgt) { f->alpha += step; if (f->alpha > tgt) f->alpha = tgt; }
        else if (f->alpha > tgt) { f->alpha -= step; if (f->alpha < tgt) f->alpha = tgt; }
        if (f->alpha != a0)
            InvalidateRect(f->hwnd, NULL, FALSE);
    }
}

/* Subclass shared by all functional buttons: detect pointer enter/leave so the
 * highlight knows when to fade in/out. Everything else passes through to the
 * original BUTTON proc, so owner-draw + clicks behave exactly as before. */
static LRESULT CALLBACK btn_subproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    HoverFx *f = hover_for(h);
    if (f) {
        if (m == WM_MOUSEMOVE) {
            if (!f->hovering) {
                f->hovering = 1;
                TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(h, NULL, FALSE);
            }
        } else if (m == WM_MOUSELEAVE) {
            f->hovering = 0;
            InvalidateRect(h, NULL, FALSE);
        }
    }
    return CallWindowProcW(g_btnProc, h, m, w, l);
}

/* Register a button for the hover highlight and install the subclass. */
static void hover_register(HWND h)
{
    if (g_btnCount >= INK_MAX_BTN || !h) return;
    HoverFx *f = &g_btn[g_btnCount++];
    f->hwnd = h; f->hovering = 0; f->alpha = 0;
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)btn_subproc);
    if (!g_btnProc) g_btnProc = prev;     /* same class proc for every button */
}

static void draw_button(LPDRAWITEMSTRUCT d)
{
    COLORREF bg;
    switch (d->CtlID) {
    case ID_BTN_CHECK: bg = RGB(255, 140, 50); break;   /* carrot */
    case ID_BTN_CHAN:  bg = C_PEKO;            break;
    case ID_BTN_LANG:  bg = C_INDIGO2;         break;
    case ID_BTN_JOIN:  bg = C_GREEN;           break;
    case ID_BTN_GIT:   bg = RGB(64, 72, 88);   break;    /* GitHub dark */
    case ID_BTN_SET:   bg = C_INDIGO;          break;    /* gear on title bar */
    case ID_BTN_TIME:  bg = (g_timeMode == 0) ? C_RED : C_PEKODK; break; /* clock */
    default:           bg = C_INDIGO2;         break;    /* prev/next */
    }
    if (d->itemState & ODS_SELECTED)
        bg = RGB(GetRValue(bg)*82/100, GetGValue(bg)*82/100, GetBValue(bg)*82/100);

    /* lightweight hover highlight: tint the fill colour by the hover opacity
     * (clipping is implicit, text drawn afterwards stays readable) */
    HoverFx *hf = hover_for(d->hwndItem);
    if (hf && hf->alpha > 0.0) bg = hover_shade(bg, hf->alpha);

    RECT r = d->rcItem;
    int titlebar = (d->CtlID == ID_BTN_SET || d->CtlID == ID_BTN_TIME);
    int prevnext = (d->CtlID == ID_PREV || d->CtlID == ID_NEXT);

    /* An owner-draw button must paint its whole rectangle, else the rounded
     * corners leave the control's default (white) background showing. Fill the
     * rect with the colour behind the button first, then draw the rounded pill. */
    int rad = prevnext ? 0 : 12;
    if (prevnext) {
        HBRUSH br = CreateSolidBrush(bg);          /* small square over cover */
        FillRect(d->hDC, &r, br); DeleteObject(br);
    } else {
        COLORREF behind = titlebar ? C_INDIGO : C_PAPER_B;
        HBRUSH bb = CreateSolidBrush(behind);
        FillRect(d->hDC, &r, bb); DeleteObject(bb);
        fill_round(d->hDC, r.left, r.top, r.right, r.bottom, rad, bg);
    }

    if (d->CtlID == ID_BTN_TIME) {              /* cute clock icon */
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        int rad = (r.right - r.left) / 2 - S(7); if (rad < 5) rad = 5;
        HPEN pn = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(d->hDC, pn), ob = SelectObject(d->hDC, GetStockObject(NULL_BRUSH));
        Ellipse(d->hDC, cx - rad, cy - rad, cx + rad, cy + rad);
        MoveToEx(d->hDC, cx, cy, NULL); LineTo(d->hDC, cx, cy - rad + S(3));      /* minute hand */
        MoveToEx(d->hDC, cx, cy, NULL); LineTo(d->hDC, cx + rad - S(5), cy);      /* hour hand   */
        SelectObject(d->hDC, op); SelectObject(d->hDC, ob); DeleteObject(pn);
        return;
    }

    wchar_t txt[64]; GetWindowTextW(d->hwndItem, txt, 64);
    draw_text(d->hDC, txt, &r, prevnext ? g_fTitle : g_fUI,
              RGB(255,255,255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* ===== PART 5 - LAYOUT & ACTIONS ===== */

/* recompute area rectangles and reposition buttons for the current size */
static void compute_layout(int W, int H)
{
    int m = S(16);                            /* outer margin            */
    R_title  = (RECT){ 0, 0, W, S(58) };
    R_bottom = (RECT){ 0, H - S(64), W, H };
    int top = R_title.bottom + S(14), bot = R_bottom.top - S(14);

    int lw = W * 42 / 100; if (lw < S(360)) lw = S(360); if (lw > S(700)) lw = S(700);
    R_board = (RECT){ m, top, m + lw, bot };

    int rx = R_board.right + m, rw = W - m - rx;
    int ch = rw * 9 / 16; if (ch > (bot - top) * 6 / 10) ch = (bot - top) * 6 / 10;
    R_cover = (RECT){ rx, top, rx + rw, top + ch };
    R_peko  = (RECT){ rx, R_cover.bottom + S(8), W - S(8), bot };

    /* title-bar top-right: settings gear, and the timezone clock to its left */
    int gw = S(32), gy = (R_title.bottom - gw) / 2;
    MoveWindow(g_hSet,  W - gw - S(14),               gy, gw, gw, TRUE);
    MoveWindow(g_hTime, W - gw - S(14) - gw - S(8),   gy, gw, gw, TRUE);

    /* bottom-bar buttons */
    int by = R_bottom.top + (S(64) - S(40)) / 2, bh = S(40);
    int bw = S(150), gap = S(12), bx = S(20), jw = S(110), gitw = S(96);
    MoveWindow(g_hCheck, bx,              by, bw, bh, TRUE);
    MoveWindow(g_hChan,  bx + (bw+gap),   by, bw, bh, TRUE);
    MoveWindow(g_hLang,  bx + 2*(bw+gap), by, bw, bh, TRUE);
    int joinX = W - m - jw;
    int gitX  = joinX - gap - gitw;
    MoveWindow(g_hGit,   gitX,  by, gitw, bh, TRUE);          /* GitHub link   */
    MoveWindow(g_hJoin,  joinX, by, jw,   bh, TRUE);          /* "little" join */
    g_betaRect = (RECT){ bx + 3*(bw+gap) + S(10), R_bottom.top,
                         gitX - S(10),             R_bottom.bottom };

    /* carousel arrows over the cover */
    int aw = S(34), ah = S(44);
    int my = R_cover.top + (R_cover.bottom - R_cover.top) / 2 - ah / 2;
    MoveWindow(g_hPrev, R_cover.left + S(8),    my, aw, ah, TRUE);
    MoveWindow(g_hNext, R_cover.right - aw - S(8), my, aw, ah, TRUE);
}

/* apply_language: rebuild fonts + relabel widgets for the current g_lang */
static void apply_language(void)
{
    create_fonts();
    SetWindowTextW(g_hMain,  LANGS[g_lang].window_title);
    SetWindowTextW(g_hCheck, LANGS[g_lang].btn_check);
    SetWindowTextW(g_hChan,  LANGS[g_lang].btn_chan);
    SetWindowTextW(g_hLang,  LANGS[g_lang].btn_lang);
    SetWindowTextW(g_hJoin,  LANGS[g_lang].btn_join);
    SetWindowTextW(g_hPrev,  L"\x25C0");   /* ◀ */
    SetWindowTextW(g_hNext,  L"\x25B6");   /* ▶ */
    if (g_state == ST_OK) g_relockMs = GetTickCount();  /* board text changed */
    InvalidateRect(g_hMain, NULL, FALSE);
}

/* do_check: fetch the streams page, parse it, prefetch covers, then repaint.
 * Runs on the UI thread, so the window pauses briefly during the download. */
static void do_check(void)
{
    g_state = ST_CHECKING;
    g_showGif = 1;                          /* reveal the loading GIF on the board */
    g_gifFrame = 0; g_gifMs = GetTickCount();
    InvalidateRect(g_hMain, &R_board, FALSE);
    UpdateWindow(g_hMain);

    /* drop old covers */
    for (int i = 0; i < g_itemCount; ++i)
        if (g_items[i].thumb) { DeleteObject(g_items[i].thumb); g_items[i].thumb = NULL; }

    size_t size = 0;
    char *html = fetch_url(STREAMS_URL, &size);
    if (!html || size < 4096 ||
        (strstr(html, "ytInitialData") == NULL && strstr(html, "lockupViewModel") == NULL)) {
        g_state = ST_ERROR;
        if (html) free(html);
        InvalidateRect(g_hMain, NULL, FALSE);
        return;
    }
    parse_streams(html);
    free(html);
    g_state = ST_OK;
    g_coverIndex = 0;
    g_coverSlideMs = 0;
    g_coverMs = GetTickCount();
    g_revealMs = GetTickCount();            /* trigger the schedule slide-in */
    for (int i = 0; i < g_itemCount; ++i)   /* prefetch covers -> instant switching */
        ensure_cover(i);
    InvalidateRect(g_hMain, NULL, FALSE);
}

/* borderless full screen toggle (classic save/restore-placement technique) */
static void toggle_fullscreen(HWND hWnd)
{
    static WINDOWPLACEMENT prev = { sizeof(prev) };
    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hWnd, &prev) &&
            GetMonitorInfoW(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongPtrW(hWnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLongPtrW(hWnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hWnd, &prev);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

/* resize so the client area becomes cw x ch, then centre on screen */
static void set_resolution(HWND hWnd, int cw, int ch)
{
    WINDOWPLACEMENT wp;
    ZeroMemory(&wp, sizeof wp);
    wp.length = sizeof wp;
    GetWindowPlacement(hWnd, &wp);
    if (wp.showCmd == SW_SHOWMAXIMIZED) ShowWindow(hWnd, SW_RESTORE);
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRect(&r, (DWORD)GetWindowLongPtrW(hWnd, GWL_STYLE), FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    SetWindowPos(hWnd, NULL, sx < 0 ? 0 : sx, sy < 0 ? 0 : sy, ww, wh, SWP_NOZORDER);
}

/* ===== PART 6 - WINDOW PROCEDURE: events, ~60 fps animation timer, painting ===== */

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hMain = hWnd;
        GpStartupInput si; ZeroMemory(&si, sizeof si); si.v = 1;
        GdiplusStartup(&g_gdip, &si, NULL);
        g_hPekora = load_image_res(IMG_PEKORA, &g_pkW, &g_pkH, 0x00000000);
        g_gifCount = load_gif_frames_res(IMG_GIF, g_gifFrames, MAX_GIF_FRAMES,
                                         &g_gifW, &g_gifH);

        /* UI scale from monitor DPI, with a floor so text is comfortably large */
        HDC dc0 = GetDC(hWnd);
        int dpi = GetDeviceCaps(dc0, LOGPIXELSX);
        ReleaseDC(hWnd, dc0);
        g_ui = dpi / 96.0;
        if (g_ui < 1.2) g_ui = 1.2;
        if (g_ui > 2.5) g_ui = 2.5;
        load_pixel_font();              /* use pixel.ttf if the user supplied one */
        create_fonts();

        DWORD bs = WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_TABSTOP;
        g_hCheck = CreateWindowW(L"BUTTON", LANGS[g_lang].btn_check, bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_CHECK, NULL, NULL);
        g_hChan  = CreateWindowW(L"BUTTON", LANGS[g_lang].btn_chan,  bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_CHAN,  NULL, NULL);
        g_hLang  = CreateWindowW(L"BUTTON", LANGS[g_lang].btn_lang,  bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_LANG,  NULL, NULL);
        g_hJoin  = CreateWindowW(L"BUTTON", LANGS[g_lang].btn_join,  bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_JOIN,  NULL, NULL);
        g_hGit   = CreateWindowW(L"BUTTON", L"GitHub", bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_GIT, NULL, NULL);
        g_hPrev  = CreateWindowW(L"BUTTON", L"\x25C0", bs, 0,0,0,0, hWnd, (HMENU)ID_PREV, NULL, NULL);
        g_hNext  = CreateWindowW(L"BUTTON", L"\x25B6", bs, 0,0,0,0, hWnd, (HMENU)ID_NEXT, NULL, NULL);
        g_hSet   = CreateWindowW(L"BUTTON", L"\x2699", bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_SET, NULL, NULL);
        g_hTime  = CreateWindowW(L"BUTTON", L"", bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_TIME, NULL, NULL);

        /* Material hover ink on every functional button (subclass + register) */
        hover_register(g_hCheck); hover_register(g_hChan); hover_register(g_hLang);
        hover_register(g_hJoin);  hover_register(g_hGit);  hover_register(g_hPrev);
        hover_register(g_hNext);  hover_register(g_hSet);  hover_register(g_hTime);

        srand((unsigned)time(NULL));
        g_phrase  = rand() % PHRASE_COUNT;
        g_phraseMs = g_coverMs = GetTickCount();
        SetTimer(hWnd, ID_TIMER, ANIM_MS, NULL);   /* ~30 fps: clock/cover/reveal */
        return 0;
    }
    case WM_SIZE:
        compute_layout(LOWORD(lp), HIWORD(lp));
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 940;
        mmi->ptMinTrackSize.y = 640;
        return 0;
    }
    case WM_DPICHANGED: {                 /* moved to a monitor with another DPI */
        g_ui = LOWORD(wp) / 96.0;
        if (g_ui < 1.2) g_ui = 1.2;
        if (g_ui > 2.5) g_ui = 2.5;
        create_fonts();
        RECT *nr = (RECT *)lp;            /* system-suggested window rect */
        SetWindowPos(hWnd, NULL, nr->left, nr->top,
                     nr->right - nr->left, nr->bottom - nr->top, SWP_NOZORDER);
        return 0;
    }
    case WM_TIMER: {
        DWORD now = GetTickCount();

        /* clock: rebuild HH:MM:SS; mark any changed digit so it flips */
        wchar_t ns[16];
        time_t tt = time(NULL); struct tm *lt = localtime(&tt);
        if (lt) _snwprintf(ns, 16, L"%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
        else    wcscpy(ns, g_clockStr);
        for (int i = 0; ns[i] && i < 15; ++i)
            if (ns[i] != g_clockStr[i]) { g_clockOld[i] = g_clockStr[i]; g_clockChg[i] = now; }
        wcscpy(g_clockStr, ns);

        /* while any digit is mid-flip (plus a couple frames to land on the
         * settled value), repaint just the clock strip at 60 fps */
        int flipping = 0;
        for (int i = 0; g_clockStr[i] && i < 15; ++i)
            if (g_clockChg[i] && now - g_clockChg[i] < FLIP_MS + 80) flipping = 1;
        if (flipping) paint_clock_only(hWnd);

        /* rotate the kawaii catchphrase every ~5s */
        if (now - g_phraseMs >= 5000) {
            int n = g_phrase;
            if (PHRASE_COUNT > 1) while (n == g_phrase) n = rand() % PHRASE_COUNT;
            g_phrase = n; g_phraseMs = now;
            InvalidateRect(hWnd, &R_peko, FALSE);
        }

        /* auto-switch the cover every ~6s */
        if (g_state == ST_OK && g_itemCount > 1 && now - g_coverMs >= 6000)
            set_cover(g_coverIndex + 1);

        /* keep the cover area live while a slide is running */
        if (g_coverSlideMs && now - g_coverSlideMs < SLIDE_MS)
            InvalidateRect(hWnd, &R_cover, FALSE);

        /* keep the board live while any departure-board FX is still flickering:
         * the staggered per-row reveals, or a content-change relock */
        if (g_state == ST_OK) {
            int fxLive = 0;
            if (g_revealMs &&
                now - g_revealMs < (DWORD)(g_itemCount + 1) * REVEAL_STAGGER + FX_REVEAL_MS)
                fxLive = 1;
            if (g_relockMs && now - g_relockMs < FX_RELOCK_MS) fxLive = 1;
            if (fxLive) InvalidateRect(hWnd, &R_board, FALSE);
            else if (g_relockMs) g_relockMs = 0;   /* settled: clear the trigger */
        }

        /* advance the loading GIF and repaint just its strip on the board */
        if (g_showGif && g_gifCount > 1 && now - g_gifMs >= GIF_FRAME_MS) {
            g_gifFrame = (g_gifFrame + 1) % g_gifCount;
            g_gifMs = now;
            if (!IsRectEmpty(&g_gifRect)) InvalidateRect(hWnd, &g_gifRect, FALSE);
        }

        /* advance Material hover ink (repaints only the buttons that changed) */
        hover_tick();

        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   /* fully painted (double-buffered) in WM_PAINT */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        /* compose the whole frame off-screen, then blit -> zero flicker */
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP mb = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ omb = SelectObject(mem, mb);
        draw_background(mem, &rc);
        draw_titlebar(mem);
        draw_cover(mem);
        draw_pekora(mem);
        draw_board(mem);
        /* beta reminder, centered in the bottom bar between the buttons */
        draw_text(mem, LANGS[g_lang].beta, &g_betaRect, g_fSmall, C_RED,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
               ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
               mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, omb); DeleteObject(mb); DeleteDC(mem);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DRAWITEM:
        draw_button((LPDRAWITEMSTRUCT)lp);
        return TRUE;

    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        for (int i = 0; i < g_linkCount; ++i)
            if (PtInRect(&g_links[i].rect, p)) {
                ShellExecuteW(hWnd, L"open", g_links[i].url, NULL, NULL, SW_SHOWNORMAL);
                break;
            }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT p; GetCursorPos(&p); ScreenToClient(hWnd, &p);
        for (int i = 0; i < g_linkCount; ++i)
            if (PtInRect(&g_links[i].rect, p)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_CHECK: do_check(); break;
        case ID_BTN_CHAN:  ShellExecuteW(hWnd, L"open", CHANNEL_HOME, NULL, NULL, SW_SHOWNORMAL); break;
        case ID_BTN_JOIN:  ShellExecuteW(hWnd, L"open", JOIN_URL,     NULL, NULL, SW_SHOWNORMAL); break;
        case ID_BTN_GIT:   ShellExecuteW(hWnd, L"open", GITHUB_URL,   NULL, NULL, SW_SHOWNORMAL); break;
        case ID_FULLSCRN:  toggle_fullscreen(hWnd); break;
        case ID_BTN_LANG:  g_lang = lang_next(g_lang); apply_language(); break;
        case ID_PREV:      if (g_itemCount > 0) set_cover(g_coverIndex - 1); break;
        case ID_NEXT:      if (g_itemCount > 0) set_cover(g_coverIndex + 1); break;
        case ID_BTN_SET: {          /* gear -> resolution popup menu */
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, ID_RES_720,  L"1280 x 720");
            AppendMenuW(m, MF_STRING, ID_RES_1080, L"1920 x 1080");
            AppendMenuW(m, MF_STRING, ID_RES_1440, L"2560 x 1440");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, ID_FULLSCRN, L"Full screen (F11)");
            RECT rb; GetWindowRect(g_hSet, &rb);
            TrackPopupMenu(m, TPM_RIGHTALIGN | TPM_TOPALIGN, rb.right, rb.bottom, 0, hWnd, NULL);
            DestroyMenu(m);
            break;
        }
        case ID_RES_720:  set_resolution(hWnd, 1280, 720);  break;
        case ID_RES_1080: set_resolution(hWnd, 1920, 1080); break;
        case ID_RES_1440: set_resolution(hWnd, 2560, 1440); break;
        case ID_BTN_TIME: {        /* clock -> schedule timezone menu */
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING | (g_timeMode == 0 ? MF_CHECKED : 0),
                        ID_TIME_JST, LANGS[g_lang].tm_jst);
            AppendMenuW(m, MF_STRING | (g_timeMode == 1 ? MF_CHECKED : 0),
                        ID_TIME_SYS, LANGS[g_lang].tm_sys);
            RECT rb; GetWindowRect(g_hTime, &rb);
            TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, rb.left, rb.bottom, 0, hWnd, NULL);
            DestroyMenu(m);
            break;
        }
        case ID_TIME_JST:
        case ID_TIME_SYS:
            if (g_timeMode != (LOWORD(wp) == ID_TIME_SYS) && g_state == ST_OK)
                g_relockMs = GetTickCount();           /* times change -> relock */
            g_timeMode = (LOWORD(wp) == ID_TIME_SYS);
            InvalidateRect(hWnd, &R_board, FALSE);     /* re-render the times   */
            InvalidateRect(g_hTime, NULL, TRUE);       /* recolor the clock btn */
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, ID_TIMER);
        for (int i = 0; i < g_itemCount; ++i) if (g_items[i].thumb) DeleteObject(g_items[i].thumb);
        if (g_fTitle) DeleteObject(g_fTitle);
        if (g_fUI)    DeleteObject(g_fUI);
        if (g_fSmall) DeleteObject(g_fSmall);
        if (g_fBoard) DeleteObject(g_fBoard);
        if (g_fClock) DeleteObject(g_fClock);
        if (g_fTag)   DeleteObject(g_fTag);
        if (g_fDate)  DeleteObject(g_fDate);
        if (g_hPekora) DeleteObject(g_hPekora);
        for (int i = 0; i < g_gifCount; ++i)
            if (g_gifFrames[i]) DeleteObject(g_gifFrames[i]);
        if (g_gdip)   GdiplusShutdown(g_gdip);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hWnd, msg, wp, lp);
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

/* entry point: register the class, create the window, run the message loop */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int nShow)
{
    (void)hPrev; (void)cmd;

    /* Be DPI-aware so Windows renders us at native pixels instead of
     * bitmap-stretching (which is what made fullscreen look low-res). */
    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    SetCtxFn setCtx = (SetCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                               "SetProcessDpiAwarenessContext");
    if (setCtx) setCtx((HANDLE)-4);              /* PER_MONITOR_AWARE_V2 */
    else SetProcessDPIAware();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"PekoraLiveBoardCls";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    RECT r = { 0, 0, 1920, 1080 };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    HWND hWnd = CreateWindowW(L"PekoraLiveBoardCls", LANGS[g_lang].window_title, style,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        NULL, NULL, hInst, NULL);
    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    /* F11 -> toggle full screen, via a one-entry accelerator table */
    ACCEL acc = { FVIRTKEY, VK_F11, ID_FULLSCRN };
    HACCEL hAccel = CreateAcceleratorTableW(&acc, 1);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!TranslateAcceleratorW(hWnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    return (int)msg.wParam;
}

/* ===== SELFTEST: console main() that runs PART 1's parser on a saved HTML
 * file and prints what it found. Build with -DSELFTEST. Not the real app. ===== */
#else

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "_streams_sample.html";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1); fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);
    parse_streams(buf);
    printf("parsed %d item(s)\n", g_itemCount);
    for (int i = 0; i < g_itemCount; ++i)
        printf("[%d] %s id=%s key=%lld\n", i,
               g_items[i].isLive ? "LIVE" : "UP", g_items[i].id, g_items[i].sortKey);
    free(buf);
    return 0;
}

#endif
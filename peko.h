/*
 * Peko Board - shared header
 *
 * A multi-function Win32/GDI fan dashboard for the VTuber Usada Pekora:
 * live/upcoming schedule board, replay (VOD) gallery, local music player,
 * X (Twitter) quick links, and an interactive mascot Pekora.
 *
 * Modules (one concern per file, all sharing this header):
 *   peko_parser.c  streams-page parser (also builds alone with -DSELFTEST)
 *   peko_net.c     HTTPS fetch + worker threads (check / thumbnails)
 *   peko_img.c     GDI+ decode helpers (PNG/JPG/GIF/HICON)
 *   peko_lang.c    EN / JA / ZH language packs
 *   peko_draw.c    fonts, drawing helpers, board-FX engine, hover ink
 *   peko_pages.c   the tab pages: board, replays, music, X card, cover
 *   peko_chara.c   interactive Pekora: reactions, affection, petals, omikuji
 *   peko_music.c   MCI playback engine (music\ folder)
 *   peko_cfg.c     PekoBoard.ini settings persistence
 *   peko_main.c    window, layout, timer, tray, entry point
 *
 * Build: PekoBoard_build.bat (MinGW-w64 gcc + windres).
 * Author: Toriyou1121.  Bundled font: DotGothic16 (SIL OFL 1.1).
 */
#ifndef PEKO_H
#define PEKO_H

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
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <math.h>

#define PEKO_VERSION_W  L"v0.4.0"

#define STREAMS_URL   L"https://www.youtube.com/@usadapekora/streams?hl=ja"
#define CHANNEL_HOME  L"https://www.youtube.com/@usadapekora"
#define JOIN_URL      L"https://www.youtube.com/@usadapekora/join"
#define GITHUB_URL    L"https://github.com/Toriyou1121/Usada-Pekora-Live-Stream-Checker"
#define X_PROFILE_URL L"https://x.com/usadapekora"
#define X_LATEST_URL  L"https://x.com/search?q=from%3Ausadapekora&f=live"
#define X_ART_URL     L"https://x.com/hashtag/%E3%81%BA%E3%81%93%E3%82%89%E3%83%BC%E3%81%A8"

/* ===== parser ===== */

/* One parsed stream / VOD. */
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
    int       thumbState;    /* 0 untried, 1 ok, 2 failed, 3 queued  */
} StreamItem;

#define MAX_ITEMS 64
#define MAX_VODS  12

typedef struct {
    StreamItem items[MAX_ITEMS]; int itemCount;   /* live + upcoming */
    StreamItem vods[MAX_VODS];   int vodCount;    /* finished streams */
} ParseResult;

void parse_streams(const char *html, ParseResult *res);

#ifndef SELFTEST   /* everything below is GUI-only */

/* ===== control / command ids ===== */
#define ID_BTN_CHECK 101
#define ID_BTN_CHAN  102
#define ID_BTN_LANG  103
#define ID_BTN_JOIN  104
#define ID_BTN_SET   105
#define ID_BTN_GIT   106
#define ID_BTN_TIME  107
#define ID_PREV      110
#define ID_NEXT      111
#define ID_TAB0      120      /* BOARD tab                            */
#define ID_TAB1      121      /* REPLAYS tab                          */
#define ID_TAB2      122      /* MUSIC tab                            */
#define ID_TIMER     1

#define ID_RES_720   201
#define ID_RES_1080  202
#define ID_RES_1440  203
#define ID_FULLSCRN  204
#define ID_TIME_JST  205
#define ID_TIME_SYS  206
#define ID_SET_VOL   210
#define ID_AC_OFF    211
#define ID_AC_5      212
#define ID_AC_10     213
#define ID_TGL_NOTIFY 214
#define ID_TGL_PETALS 215
#define ID_TGL_TRAY   216
#define ID_TRAY_OPEN  220
#define ID_TRAY_CHECK 221
#define ID_TRAY_EXIT  222

/* worker-thread -> UI messages */
#define WM_APP_CHECKDONE (WM_APP + 1)   /* wp = ok, lp = isAuto       */
#define WM_APP_THUMBDONE (WM_APP + 2)   /* lp = ThumbResult* (free!)  */
#define WM_APP_TRAY      (WM_APP + 3)   /* tray icon callback         */
#define WM_APP_TRACKEND  (WM_APP + 4)   /* audio thread: track finished */

/* ===== palette (Pekora blue / sakura pink) ===== */
#define C_INDIGO   RGB(74, 132, 200)
#define C_INDIGO2  RGB(126, 176, 226)
#define C_PEKO     RGB(64, 146, 222)
#define C_PEKODK   RGB(40, 110, 188)
#define C_RED      RGB(235, 102, 120)
#define C_GREEN    RGB(74, 192, 150)
#define C_SAKURA   RGB(255, 170, 200)
#define C_SAKURA_D RGB(240, 128, 170)
#define C_SKY      RGB(196, 228, 255)
#define C_SKY_L    RGB(226, 243, 255)
#define C_PINK_L   RGB(255, 233, 243)
#define C_NAVY     RGB(54, 76, 122)
#define C_NAVY_DIM RGB(126, 146, 180)
#define C_TILEBG   RGB(255, 255, 255)
#define C_TILEBORD RGB(150, 200, 240)
#define C_PAPER_T  RGB(230, 244, 255)
#define C_PAPER_B  RGB(255, 238, 246)
#define C_CARROT   RGB(255, 140, 50)
#define C_FX_BG    RGB(245, 243, 250)

/* embedded resource ids */
#define IMG_PEKORA  101
#define IMG_PIXEL   200
#define IMG_GIF     300
#define MAX_GIF_FRAMES 64

/* mascot emotions for the VN-style character presentation system: each one
 * maps to a motion recipe (movement + scale + shake + icon + camera zoom)
 * applied to the single Pekora sprite */
typedef enum {
    EXPR_SHY = 0, EXPR_SMILE, EXPR_HAPPY, EXPR_ANGRY,
    EXPR_POUT, EXPR_EMBARRASSED, EXPR_THINKING, EXPR_SURPRISED,
    EXPR_EXCITED, EXPR_SAD,
    EXPR_COUNT
} ExprId;

/* ===== animation timing ===== */
#define ANIM_MS   16
#define FLIP_MS   300
#define SLIDE_MS  280
#define REVEAL_STAGGER 70
#define FX_REVEAL_MS   130
#define FX_RELOCK_MS   110
#define FX_FRAME_MS    34
#define FX_JITTER_PX   1
#define GIF_FRAME_MS   90

typedef struct { int hidden; int dx, dy; double dim; } FxState;

/* ===== language packs ===== */
typedef enum { LANG_EN = 0, LANG_JA = 1, LANG_ZH = 2, LANG_COUNT } LangId;

typedef struct {
    const wchar_t *window_title;
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
    const wchar_t *beta;
    const wchar_t *tm_jst;
    const wchar_t *tm_sys;
    const wchar_t *tz_local;
    const wchar_t *weekday[7];    /* Sun..Sat */
    /* --- Peko Board additions --- */
    const wchar_t *tab_board, *tab_replays, *tab_music;
    const wchar_t *x_title;       /* X links card caption              */
    const wchar_t *x_profile, *x_latest, *x_art;
    const wchar_t *rp_title;      /* replays page heading              */
    const wchar_t *rp_none;       /* no VODs found                     */
    const wchar_t *rp_hint;       /* "click a card to watch"           */
    const wchar_t *mu_title;      /* music page heading                */
    const wchar_t *mu_none;       /* drop songs into music\ hint       */
    const wchar_t *mu_now;        /* NOW PLAYING                       */
    const wchar_t *mu_idle;       /* nothing playing                   */
    const wchar_t *cd_label;      /* "next stream in"                  */
    const wchar_t *sm_size;       /* settings: window size submenu     */
    const wchar_t *sm_volume, *sm_autochk;
    const wchar_t *sm_ac_off, *sm_ac_5, *sm_ac_10;
    const wchar_t *sm_notify, *sm_petals, *sm_tray;
    const wchar_t *vol_music, *vol_voice;
    const wchar_t *notif_live;    /* tray balloon: she is live          */
    const wchar_t *tray_open, *tray_check, *tray_exit;
    const wchar_t *rank[5];       /* affection rank names               */
    const wchar_t *fortune_pre;   /* omikuji bubble prefix              */
    const wchar_t *talk_label;    /* the "let's chat" chip by Pekora    */
    const wchar_t *dlg_end;       /* dialogue "the end" chip            */
    const wchar_t *dlg_invite;    /* she invites you to chat            */
} LangPack;

extern const LangPack LANGS[LANG_COUNT];
int lang_next(int cur);

/* ===== settings (PekoBoard.ini) ===== */
typedef struct {
    int lang;                 /* LangId                                */
    int timeMode;             /* 0 JST, 1 system                       */
    int musicVol, voiceVol;   /* 0..100                                */
    int autoCheckMin;         /* 0 off / 5 / 10                        */
    int notifyOn, petalsOn, trayMin;
    int loopMode, shuffle;    /* loop: 0 off, 1 list, 2 one            */
    int affection;
    wchar_t lastGreetDay[16]; /* yyyy-mm-dd of last daily greeting     */
    wchar_t fortuneDay[16];   /* yyyy-mm-dd of last omikuji            */
    int fortuneIdx;           /* today's drawn fortune                 */
} Config;
extern Config g_cfg;
void cfg_load(void);
void cfg_save(void);

/* ===== shared app state ===== */
typedef enum { ST_NONE, ST_CHECKING, ST_OK, ST_ERROR } StatusState;
typedef enum { TAB_BOARD = 0, TAB_REPLAYS, TAB_MUSIC, TAB_COUNT } TabId;

extern HWND        g_hMain;
extern StatusState g_state;
extern int         g_lang, g_timeMode;
extern int         g_tab, g_tabFrom, g_tabDir;
extern DWORD       g_tabSlideMs;
extern int         g_wasLive;          /* for OFFLINE->LIVE notification */

extern StreamItem g_items[MAX_ITEMS]; extern int g_itemCount;
extern StreamItem g_vods[MAX_VODS];   extern int g_vodCount;

/* layout rects (window coords) */
extern RECT R_title, R_tabs, R_board, R_cover, R_xcard, R_peko, R_bottom;
extern RECT R_pageClip;                /* clip for sliding page content  */
extern RECT R_grid;                    /* replays grid                   */
extern RECT R_mlist, R_mcard;          /* music playlist + player card   */
extern RECT g_clockRect, g_betaRect, g_gifRect, g_cdRect;
extern int  g_clkX, g_clkY, g_clkW, g_clkH, g_clkGap;

extern wchar_t g_clockStr[16], g_clockOld[16];
extern DWORD   g_clockChg[16];

extern HWND g_hCheck, g_hChan, g_hLang, g_hJoin, g_hPrev, g_hNext,
            g_hSet, g_hGit, g_hTime, g_hTabs[TAB_COUNT];

/* UI scale */
extern double g_ui;
#define S(v) ((int)((v) * g_ui + 0.5))

/* clickable regions; url starting with '!' is an internal action */
typedef struct { RECT rect; wchar_t url[160]; } LinkHit;
#define MAX_LINKS 96
extern LinkHit g_links[MAX_LINKS];
extern int     g_linkCount;
void add_link(int lx, int ty, int rx, int by, const wchar_t *url);

/* ===== peko_net.c ===== */
char *fetch_url(const wchar_t *url, size_t *out_size);
void net_init(HWND hwndMain);
void net_shutdown(void);
int  net_start_check(int isAuto);      /* 0 if a check is already running */
int  net_busy(void);
void net_take_result(ParseResult *out);
void net_request_thumb(int kind, int index);  /* kind 0=items 1=vods */
typedef struct { unsigned gen; int kind, index; HBITMAP bmp; int w, h; char id[16]; } ThumbResult;
unsigned net_thumb_gen(void);
void net_bump_thumb_gen(void);

/* ===== peko_img.c ===== */
int  img_init(void);
void img_shutdown(void);
HBITMAP bitmap_from_memory(const void *data, DWORD size, int *w, int *h, DWORD bg);
HBITMAP load_image_res(WORD id, int *w, int *h, DWORD bg);
HBITMAP bitmap_from_file(const wchar_t *path, int *w, int *h, DWORD bg);
int  load_gif_frames_res(WORD id, HBITMAP *out, int cap, int *w, int *h);
HICON icon_from_res_png(WORD id);

/* ===== peko_draw.c ===== */
extern HFONT g_fTitle, g_fUI, g_fSmall, g_fBoard, g_fClock, g_fTag, g_fDate;
extern DWORD g_relockMs;
void load_pixel_font(void);
void create_fonts(void);
const wchar_t *face_for_lang(void);
void fill_round(HDC dc, int l, int t, int r, int b, int rad, COLORREF c);
double ease(double p);
void grad_v(HDC dc, RECT *r, COLORREF top, COLORREF bot);
void cover_blit(HDC dc, HBITMAP bmp, int iw, int ih, int dx, int dy, int dw, int dh);
void draw_sakura(HDC dc, int cx, int cy, int r, COLORREF c);
void draw_text(HDC dc, const wchar_t *s, RECT *r, HFONT f, COLORREF c, UINT fmt);
FxState fx_eval(DWORD now, DWORD start, unsigned salt, int isReveal);
FxState fx_row(DWORD now, DWORD revealStart, unsigned salt);
FxState fx_persist(DWORD now, unsigned salt);
void draw_text_fx(HDC dc, const wchar_t *s, RECT *r, HFONT f, COLORREF c, UINT fmt, FxState fx);
void draw_flaps_fx(HDC dc, int x, int y, int cw, int ch, int gap, const wchar_t *s, HFONT f, FxState fx);
void draw_flap_tile(HDC dc, int cx, int y, int cw, int ch, wchar_t oldg, wchar_t newg, double p);
void hover_register(HWND h);
void hover_tick(void);
void draw_button(LPDRAWITEMSTRUCT d);

/* ===== peko_pages.c ===== */
extern int   g_coverIndex, g_coverFrom, g_coverDir;
extern DWORD g_coverSlideMs, g_coverMs, g_revealMs, g_vodRevealMs;
extern int   g_vodScrollT;  extern double g_vodScroll;   /* replays scroll  */
extern int   g_musScrollT;  extern double g_musScroll;   /* playlist scroll */
extern RECT  g_seekRect, g_mvolRect;   /* music page drag targets          */
extern HBITMAP g_gifFrames[MAX_GIF_FRAMES];
extern int   g_gifCount, g_gifW, g_gifH, g_gifFrame, g_showGif;
extern DWORD g_gifMs;
void set_cover(int idx);
void pages_draw(HDC dc, int tab);
void draw_clock(HDC dc);
void paint_clock_only(HWND hWnd);
void draw_background(HDC dc, RECT *rc);
void draw_titlebar(HDC dc);
int  pages_scroll_max(int tab);

/* ===== peko_chara.c ===== */
extern HBITMAP g_hPekora; extern int g_pkW, g_pkH;
void chara_init(void);
void chara_draw(HDC dc);                       /* mascot + bubble + hearts */
void petals_draw(HDC dc, RECT *rc);
int  chara_tick(HWND hWnd, DWORD now);         /* returns 1 if petals want a full repaint */
int  chara_click(HWND hWnd, POINT p);          /* 1 = consumed             */
void chara_drag(HWND hWnd, POINT p);
void chara_konami_key(WPARAM vk);
void bubble_say(const wchar_t *txt, DWORD holdMs);
int  chara_fortune_hit(HWND hWnd, POINT p);    /* sakura blossom omikuji   */
void chara_on_live(void);                      /* she just went live       */
void voice_set_volume(int v);                  /* 0..100                   */
void chara_set_expr(int expr, DWORD holdMs);   /* expression + revert timer */
void chara_dlg_draw(HDC dc, RECT *client);     /* galgame dialogue overlay  */
void chara_dlg_action(HWND hWnd, const wchar_t *act);  /* "!talk" "!dlg:N" "!dlgx" "!dlgskip" */
int  chara_dlg_active(void);

/* ===== peko_music.c =====
 * Own decoder (minimp3, CC0) + waveOut streaming: no dependency on the OS
 * media stack, which is broken for MP3 on some Windows editions. */
typedef struct { wchar_t path[MAX_PATH]; wchar_t name[128]; } Track;
#define MAX_TRACKS 256
extern Track g_tracks[MAX_TRACKS];
extern int   g_trackCount, g_curTrack, g_playing;  /* playing: 0/1/2(paused) */
void music_scan(void);
int  music_play(int idx);
void music_toggle(void);                       /* play/pause                */
void music_next(int dir);
void music_stop(void);
void music_shutdown(void);                     /* stop + kill audio thread  */
void music_set_volume(int v);                  /* 0..100                    */
int  music_pos_ms(void);
int  music_len_ms(void);
void music_seek_ms(int ms);
void music_on_trackend(void);                  /* WM_APP_TRACKEND handler   */
HBITMAP music_cover(int *w, int *h);           /* current track's cover art */

/* ===== peko_main.c ===== */
void start_check(int isAuto);                  /* kick a threaded check    */
void apply_title(void);
HDC  scene_dc(void);                           /* composed-scene backbuffer */
void present(HWND hWnd, const RECT *r);        /* scene + petal overlay -> screen */

#endif /* SELFTEST */
#endif /* PEKO_H */

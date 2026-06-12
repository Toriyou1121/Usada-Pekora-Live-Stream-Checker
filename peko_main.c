/*
 * peko_main.c - window, layout, tabs, timer, tray, settings, entry point.
 *
 * The ~60 fps timer drives every animation; WM_PAINT composes the whole frame
 * into a persistent backbuffer (recreated only on resize) and blits the dirty
 * rect. All networking lives on worker threads (peko_net.c) - the UI thread
 * never blocks, so the window stays draggable and the clock keeps flipping
 * even mid-download.
 */
#include "peko.h"

/* ---------- global state (single-window app) ---------- */
HWND        g_hMain;
StatusState g_state = ST_NONE;
int         g_lang = LANG_JA, g_timeMode = 0;
int         g_tab = TAB_BOARD, g_tabFrom = TAB_BOARD, g_tabDir = 1;
DWORD       g_tabSlideMs = 0;
int         g_wasLive = 0;

StreamItem g_items[MAX_ITEMS]; int g_itemCount = 0;
StreamItem g_vods[MAX_VODS];   int g_vodCount = 0;

RECT R_title, R_tabs, R_board, R_cover, R_xcard, R_peko, R_bottom;
RECT R_pageClip, R_grid, R_mlist, R_mcard;
RECT g_clockRect, g_betaRect, g_gifRect, g_cdRect;
int  g_clkX, g_clkY, g_clkW, g_clkH, g_clkGap;

wchar_t g_clockStr[16] = L"00:00:00";
wchar_t g_clockOld[16] = L"00:00:00";
DWORD   g_clockChg[16];

HWND g_hCheck, g_hChan, g_hLang, g_hJoin, g_hPrev, g_hNext,
     g_hSet, g_hGit, g_hTime, g_hTabs[TAB_COUNT];

double g_ui = 1.2;

/* persistent buffers: g_bbDC holds the composed scene (no petals), g_prDC is
 * the present buffer the petal overlay is stamped onto. A petal frame is then
 * just two screen-size blits + 22 ellipses instead of a full scene recompose. */
static HDC     g_bbDC = NULL, g_prDC = NULL;
static HBITMAP g_bbBmp = NULL, g_prBmp = NULL;
static HGDIOBJ g_bbOld = NULL, g_prOld = NULL;
static int     g_bbW = 0, g_bbH = 0;

/* drag state for the music page sliders: 0 none, 1 seek, 2 music volume */
static int g_drag = 0;

/* tray */
static NOTIFYICONDATAW g_nid;
static HICON           g_trayIcon = NULL;
static DWORD           g_lastAutoMs = 0;

/* ---------- backbuffer ---------- */

static void bb_ensure(HWND hWnd, int w, int h)
{
    if (g_bbDC && w == g_bbW && h == g_bbH) return;
    HDC hdc = GetDC(hWnd);
    if (!g_bbDC) g_bbDC = CreateCompatibleDC(hdc);
    if (!g_prDC) g_prDC = CreateCompatibleDC(hdc);
    if (g_bbBmp) { SelectObject(g_bbDC, g_bbOld); DeleteObject(g_bbBmp); }
    if (g_prBmp) { SelectObject(g_prDC, g_prOld); DeleteObject(g_prBmp); }
    g_bbBmp = CreateCompatibleBitmap(hdc, w > 0 ? w : 1, h > 0 ? h : 1);
    g_bbOld = SelectObject(g_bbDC, g_bbBmp);
    g_prBmp = CreateCompatibleBitmap(hdc, w > 0 ? w : 1, h > 0 ? h : 1);
    g_prOld = SelectObject(g_prDC, g_prBmp);
    g_bbW = w; g_bbH = h;
    ReleaseDC(hWnd, hdc);
}

static void bb_destroy(void)
{
    if (g_bbDC) {
        if (g_bbBmp) { SelectObject(g_bbDC, g_bbOld); DeleteObject(g_bbBmp); }
        DeleteDC(g_bbDC);
        g_bbDC = NULL; g_bbBmp = NULL;
    }
    if (g_prDC) {
        if (g_prBmp) { SelectObject(g_prDC, g_prOld); DeleteObject(g_prBmp); }
        DeleteDC(g_prDC);
        g_prDC = NULL; g_prBmp = NULL;
    }
}

HDC scene_dc(void) { return g_bbDC; }

/* blit the scene region to the screen, stamping the petal overlay on the way */
static void present_to(HWND hWnd, HDC target, const RECT *r)
{
    if (!g_bbDC) return;
    int x = r->left, y = r->top, w = r->right - r->left, h = r->bottom - r->top;
    if (w <= 0 || h <= 0) return;
    if (g_cfg.petalsOn && g_prDC) {
        BitBlt(g_prDC, x, y, w, h, g_bbDC, x, y, SRCCOPY);
        int sv = SaveDC(g_prDC);
        IntersectClipRect(g_prDC, x, y, r->right, r->bottom);
        RECT rc; GetClientRect(hWnd, &rc);
        petals_draw(g_prDC, &rc);
        RestoreDC(g_prDC, sv);
        BitBlt(target, x, y, w, h, g_prDC, x, y, SRCCOPY);
    } else {
        BitBlt(target, x, y, w, h, g_bbDC, x, y, SRCCOPY);
    }
}

void present(HWND hWnd, const RECT *r)
{
    HDC hdc = GetDC(hWnd);
    present_to(hWnd, hdc, r);
    ReleaseDC(hWnd, hdc);
}

/* ---------- layout ---------- */

static void compute_layout(int W, int H)
{
    int m = S(16);
    R_title  = (RECT){ 0, 0, W, S(58) };
    R_bottom = (RECT){ 0, H - S(64), W, H };

    /* hanafuda tab strip under the title bar */
    int tabW = S(150), tabH = S(38), tgap = S(8);
    R_tabs = (RECT){ m, R_title.bottom + S(10), W - m, R_title.bottom + S(10) + tabH };
    for (int i = 0; i < TAB_COUNT; ++i)
        MoveWindow(g_hTabs[i], m + i * (tabW + tgap), R_tabs.top, tabW, tabH, TRUE);

    int top = R_tabs.bottom + S(12), bot = R_bottom.top - S(14);
    R_pageClip = (RECT){ 0, R_tabs.bottom + S(2), W, R_bottom.top };

    /* BOARD page: slim X links bar on TOP, then a roomy cover, mascot below */
    int lw = W * 42 / 100; if (lw < S(360)) lw = S(360); if (lw > S(700)) lw = S(700);
    R_board = (RECT){ m, top, m + lw, bot };
    int rx = R_board.right + m, rw = W - m - rx;
    R_xcard = (RECT){ rx, top, W - m, top + S(46) };
    int cy0 = R_xcard.bottom + S(8);
    int ch = rw * 9 / 16;
    if (ch > (bot - cy0) * 64 / 100) ch = (bot - cy0) * 64 / 100;
    R_cover = (RECT){ rx, cy0, rx + rw, cy0 + ch };

    /* REPLAYS page */
    R_grid = (RECT){ m, top, W * 62 / 100, bot };

    /* MUSIC page */
    R_mlist = (RECT){ m, top, W * 52 / 100, bot };
    int cardH = (bot - top) * 78 / 100; if (cardH > S(430)) cardH = S(430);
    R_mcard = (RECT){ R_mlist.right + m, top, W - m, top + cardH };

    /* the mascot lives bottom-right; her zone depends on the active tab */
    int pekoLeft, pekoTop;
    switch (g_tab) {
    case TAB_REPLAYS: pekoLeft = R_grid.right + m;  pekoTop = top;                    break;
    case TAB_MUSIC:   pekoLeft = R_mlist.right + m; pekoTop = R_mcard.bottom + S(6);  break;
    default:          pekoLeft = rx;                pekoTop = R_cover.bottom + S(6);  break;
    }
    R_peko = (RECT){ pekoLeft, pekoTop, W - S(8), bot };

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
    MoveWindow(g_hGit,   gitX,  by, gitw, bh, TRUE);
    MoveWindow(g_hJoin,  joinX, by, jw,   bh, TRUE);
    g_betaRect = (RECT){ bx + 3*(bw+gap) + S(10), R_bottom.top,
                         gitX - S(10),             R_bottom.bottom };

    /* carousel arrows over the cover (BOARD tab only) */
    int aw = S(34), ah = S(44);
    int my = R_cover.top + (R_cover.bottom - R_cover.top) / 2 - ah / 2;
    MoveWindow(g_hPrev, R_cover.left + S(8),    my, aw, ah, TRUE);
    MoveWindow(g_hNext, R_cover.right - aw - S(8), my, aw, ah, TRUE);
    int showArrows = (g_tab == TAB_BOARD) ? SW_SHOW : SW_HIDE;
    ShowWindow(g_hPrev, showArrows);
    ShowWindow(g_hNext, showArrows);
}

/* ---------- titles / language ---------- */

void apply_title(void)
{
    wchar_t t[256];
    int live = (g_state == ST_OK && g_itemCount > 0 && g_items[0].isLive);
    _snwprintf(t, 256, L"%ls%ls", live ? L"\x25CF LIVE  " : L"",
               LANGS[g_lang].window_title);
    SetWindowTextW(g_hMain, t);
}

static void apply_language(void)
{
    create_fonts();
    apply_title();
    SetWindowTextW(g_hCheck, LANGS[g_lang].btn_check);
    SetWindowTextW(g_hChan,  LANGS[g_lang].btn_chan);
    SetWindowTextW(g_hLang,  LANGS[g_lang].btn_lang);
    SetWindowTextW(g_hJoin,  LANGS[g_lang].btn_join);
    SetWindowTextW(g_hTabs[TAB_BOARD],   LANGS[g_lang].tab_board);
    SetWindowTextW(g_hTabs[TAB_REPLAYS], LANGS[g_lang].tab_replays);
    SetWindowTextW(g_hTabs[TAB_MUSIC],   LANGS[g_lang].tab_music);
    SetWindowTextW(g_hPrev,  L"\x25C0");
    SetWindowTextW(g_hNext,  L"\x25B6");
    if (g_state == ST_OK) g_relockMs = GetTickCount();
    g_cfg.lang = g_lang;
    cfg_save();
    InvalidateRect(g_hMain, NULL, FALSE);
}

/* ---------- tabs ---------- */

static void tab_switch(HWND hWnd, int tab)
{
    if (tab == g_tab || tab < 0 || tab >= TAB_COUNT) return;
    g_tabFrom = g_tab;
    g_tabDir = (tab > g_tab) ? 1 : -1;
    g_tab = tab;
    g_tabSlideMs = GetTickCount();
    RECT rc; GetClientRect(hWnd, &rc);
    compute_layout(rc.right, rc.bottom);
    for (int i = 0; i < TAB_COUNT; ++i) InvalidateRect(g_hTabs[i], NULL, FALSE);
    InvalidateRect(hWnd, NULL, FALSE);
}

/* ---------- threaded check ---------- */

void start_check(int isAuto)
{
    if (!net_start_check(isAuto)) return;        /* one at a time */
    if (!isAuto) {
        g_state = ST_CHECKING;
        g_showGif = 1;
        g_gifFrame = 0; g_gifMs = GetTickCount();
        chara_set_expr(EXPR_THINKING, 8000);     /* she ponders along */
        InvalidateRect(g_hMain, &R_board, FALSE);
    }
}

static void tray_balloon(const wchar_t *txt);

static void on_check_done(HWND hWnd, int ok, int isAuto)
{
    if (!ok) {
        if (!isAuto) {
            g_state = ST_ERROR;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return;
    }
    static ParseResult res;
    net_take_result(&res);
    net_bump_thumb_gen();                        /* stale thumbs: discard */

    /* drop old covers, swap the new data in */
    for (int i = 0; i < g_itemCount; ++i)
        if (g_items[i].thumb) { DeleteObject(g_items[i].thumb); g_items[i].thumb = NULL; }
    for (int i = 0; i < g_vodCount; ++i)
        if (g_vods[i].thumb) { DeleteObject(g_vods[i].thumb); g_vods[i].thumb = NULL; }
    memcpy(g_items, res.items, sizeof g_items);
    memcpy(g_vods,  res.vods,  sizeof g_vods);
    g_itemCount = res.itemCount;
    g_vodCount  = res.vodCount;
    g_state = ST_OK;

    DWORD now = GetTickCount();
    if (!isAuto) {
        g_coverIndex = 0;
        g_coverSlideMs = 0;
        g_coverMs = now;
        g_revealMs = now;                        /* board reveal FX */
        g_vodRevealMs = now;                     /* replay cards too */
        g_vodScrollT = 0; g_vodScroll = 0;
    } else {
        if (g_coverIndex >= g_itemCount) g_coverIndex = 0;
        g_relockMs = now;                        /* quiet refresh flicker */
    }

    /* queue thumbnails (covers first, then the visible replay cards) */
    for (int i = 0; i < g_itemCount; ++i) net_request_thumb(0, i);
    for (int i = 0; i < g_vodCount && i < 6; ++i) net_request_thumb(1, i);

    /* OFFLINE -> LIVE transition: celebrate + notify */
    int nowLive = (g_itemCount > 0 && g_items[0].isLive);
    if (nowLive && !g_wasLive) {
        chara_on_live();
        if (g_cfg.notifyOn) {
            tray_balloon(LANGS[g_lang].notif_live);
            FlashWindowEx(&(FLASHWINFO){ sizeof(FLASHWINFO), hWnd,
                                         FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 });
        }
    }
    g_wasLive = nowLive;
    apply_title();
    InvalidateRect(hWnd, NULL, FALSE);
}

static void on_thumb_done(HWND hWnd, ThumbResult *tr)
{
    if (!tr) return;
    if (tr->gen != net_thumb_gen()) {            /* from before a re-check */
        if (tr->bmp) DeleteObject(tr->bmp);
        free(tr);
        return;
    }
    StreamItem *it = NULL;
    if (tr->kind == 0 && tr->index >= 0 && tr->index < g_itemCount) it = &g_items[tr->index];
    if (tr->kind == 1 && tr->index >= 0 && tr->index < g_vodCount)  it = &g_vods[tr->index];
    if (it && strncmp(it->id, tr->id, 15) == 0) {
        if (it->thumb) DeleteObject(it->thumb);
        it->thumb = tr->bmp;
        it->tw = tr->w; it->th = tr->h;
        it->thumbState = tr->bmp ? 1 : 2;
        if (tr->kind == 0 && g_tab == TAB_BOARD)
            InvalidateRect(hWnd, &R_cover, FALSE);
        if (tr->kind == 1 && g_tab == TAB_REPLAYS)
            InvalidateRect(hWnd, &R_grid, FALSE);
    } else if (tr->bmp) DeleteObject(tr->bmp);
    free(tr);
}

/* ---------- tray ---------- */

static void tray_add(HWND hWnd)
{
    g_trayIcon = icon_from_res_png(IMG_PEKORA);
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_trayIcon ? g_trayIcon : LoadIcon(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"Peko Board");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_remove(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
}

static void tray_balloon(const wchar_t *txt)
{
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    wcsncpy(g_nid.szInfo, txt, 255); g_nid.szInfo[255] = 0;
    wcscpy(g_nid.szInfoTitle, L"Peko Board");
    g_nid.dwInfoFlags = NIIF_USER;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

static void tray_restore(HWND hWnd)
{
    ShowWindow(hWnd, SW_SHOW);
    if (IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
    SetForegroundWindow(hWnd);
}

/* ---------- volume popup (two custom sliders) ---------- */

static RECT g_vpMusic, g_vpVoice;     /* track rects, popup-client coords */
static int  g_vpDrag = 0;             /* 1 music, 2 voice                  */

static void vp_slider(HDC dc, RECT *track, int val, const wchar_t *label, COLORREF fillc)
{
    RECT lr = { track->left, track->top - S(22), track->right, track->top - S(2) };
    wchar_t lt[48];
    _snwprintf(lt, 48, L"%ls  %d", label, val);
    draw_text(dc, lt, &lr, g_fSmall, C_NAVY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int h = track->bottom - track->top;
    fill_round(dc, track->left, track->top, track->right, track->bottom, h / 2, RGB(228, 234, 246));
    int fw = (track->right - track->left) * val / 100;
    fill_round(dc, track->left, track->top, track->left + fw, track->bottom, h / 2, fillc);
    HBRUSH kb = CreateSolidBrush(C_CARROT);
    HGDIOBJ ob = SelectObject(dc, kb);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    int kx = track->left + fw, ky = (track->top + track->bottom) / 2;
    Ellipse(dc, kx - S(7), ky - S(7), kx + S(7), ky + S(7));
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(kb);
}

static int vp_apply(HWND h, POINT p, int startDrag)
{
    RECT *tr = NULL; int which = 0;
    RECT mw = g_vpMusic, vw = g_vpVoice;
    InflateRect(&mw, 0, S(8)); InflateRect(&vw, 0, S(8));
    if (g_vpDrag == 1 || (startDrag && PtInRect(&mw, p))) { tr = &g_vpMusic; which = 1; }
    else if (g_vpDrag == 2 || (startDrag && PtInRect(&vw, p))) { tr = &g_vpVoice; which = 2; }
    if (!tr) return 0;
    int w = tr->right - tr->left;
    int v = w > 0 ? (p.x - tr->left) * 100 / w : 0;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (which == 1) { g_cfg.musicVol = v; music_set_volume(v); }
    else            { g_cfg.voiceVol = v; voice_set_volume(v); }
    InvalidateRect(h, NULL, FALSE);
    return which;
}

static LRESULT CALLBACK VolProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &rc, bg); DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 2, C_SAKURA);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, rc.right, rc.bottom);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
        int mlx = S(16), mrx = rc.right - S(16);
        g_vpMusic = (RECT){ mlx, S(34), mrx, S(34) + S(8) };
        g_vpVoice = (RECT){ mlx, S(86), mrx, S(86) + S(8) };
        vp_slider(dc, &g_vpMusic, g_cfg.musicVol, LANGS[g_lang].vol_music, C_PEKO);
        vp_slider(dc, &g_vpVoice, g_cfg.voiceVol, LANGS[g_lang].vol_voice, C_SAKURA_D);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        g_vpDrag = vp_apply(h, p, 1);
        if (g_vpDrag) SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_vpDrag) {
            POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            vp_apply(h, p, 0);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_vpDrag) { g_vpDrag = 0; ReleaseCapture(); cfg_save(); }
        return 0;
    case WM_KILLFOCUS:
        cfg_save();
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
    (void)w;
}

static void volume_popup(HWND owner)
{
    static int registered = 0;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = VolProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"PekoVolCls";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        registered = 1;
    }
    RECT rb; GetWindowRect(g_hSet, &rb);
    int w = S(300), h = S(116);
    HWND hv = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"PekoVolCls", L"",
        WS_POPUP | WS_VISIBLE, rb.right - w, rb.bottom + S(6), w, h,
        owner, NULL, GetModuleHandleW(NULL), NULL);
    if (hv) SetFocus(hv);
}

/* ---------- settings menu ---------- */

static void settings_menu(HWND hWnd)
{
    const LangPack *L = &LANGS[g_lang];
    HMENU size = CreatePopupMenu();
    AppendMenuW(size, MF_STRING, ID_RES_720,  L"1280 x 720");
    AppendMenuW(size, MF_STRING, ID_RES_1080, L"1920 x 1080");
    AppendMenuW(size, MF_STRING, ID_RES_1440, L"2560 x 1440");
    AppendMenuW(size, MF_SEPARATOR, 0, NULL);
    AppendMenuW(size, MF_STRING, ID_FULLSCRN, L"Full screen (F11)");

    HMENU ac = CreatePopupMenu();
    AppendMenuW(ac, MF_STRING | (g_cfg.autoCheckMin == 0  ? MF_CHECKED : 0), ID_AC_OFF, L->sm_ac_off);
    AppendMenuW(ac, MF_STRING | (g_cfg.autoCheckMin == 5  ? MF_CHECKED : 0), ID_AC_5,   L->sm_ac_5);
    AppendMenuW(ac, MF_STRING | (g_cfg.autoCheckMin == 10 ? MF_CHECKED : 0), ID_AC_10,  L->sm_ac_10);

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_POPUP, (UINT_PTR)size, L->sm_size);
    AppendMenuW(m, MF_STRING, ID_SET_VOL, L->sm_volume);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ac, L->sm_autochk);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_cfg.notifyOn ? MF_CHECKED : 0), ID_TGL_NOTIFY, L->sm_notify);
    AppendMenuW(m, MF_STRING | (g_cfg.petalsOn ? MF_CHECKED : 0), ID_TGL_PETALS, L->sm_petals);
    AppendMenuW(m, MF_STRING | (g_cfg.trayMin  ? MF_CHECKED : 0), ID_TGL_TRAY,   L->sm_tray);

    RECT rb; GetWindowRect(g_hSet, &rb);
    TrackPopupMenu(m, TPM_RIGHTALIGN | TPM_TOPALIGN, rb.right, rb.bottom, 0, hWnd, NULL);
    DestroyMenu(m);
}

/* ---------- misc actions ---------- */

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

/* internal "!..." link actions from the drawn pages */
static void do_action(HWND hWnd, const wchar_t *act)
{
    if (act[1] == L't' || act[1] == L'd') {           /* !talk / !dlg...   */
        chara_dlg_action(hWnd, act);
        return;
    }
    if (wcscmp(act, L"!mplay") == 0)      music_toggle();
    else if (wcscmp(act, L"!mprev") == 0) music_next(-1);
    else if (wcscmp(act, L"!mnext") == 0) music_next(+1);
    else if (wcscmp(act, L"!mloop") == 0) { g_cfg.loopMode = (g_cfg.loopMode + 1) % 3; cfg_save(); }
    else if (wcscmp(act, L"!mshuf") == 0) { g_cfg.shuffle = !g_cfg.shuffle; cfg_save(); }
    else if (wcsncmp(act, L"!mtrack:", 8) == 0) music_play(_wtoi(act + 8));
    InvalidateRect(hWnd, &R_mcard, FALSE);
    InvalidateRect(hWnd, &R_mlist, FALSE);
}

/* music slider drag (seek / volume on the music page) */
static void music_drag_apply(HWND hWnd, POINT p)
{
    if (g_drag == 1) {
        int w = g_seekRect.right - g_seekRect.left;
        int len = music_len_ms();
        if (w > 0 && len > 0) {
            int v = (p.x - g_seekRect.left);
            if (v < 0) v = 0;
            if (v > w) v = w;
            music_seek_ms((int)((long long)v * len / w));
        }
    } else if (g_drag == 2) {
        int w = g_mvolRect.right - g_mvolRect.left;
        if (w > 0) {
            int v = (p.x - g_mvolRect.left) * 100 / w;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            g_cfg.musicVol = v;
            music_set_volume(v);
        }
    }
    InvalidateRect(hWnd, &R_mcard, FALSE);
}

/* ---------- window procedure ---------- */

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hMain = hWnd;
        img_init();
        cfg_load();
        g_lang = g_cfg.lang;
        g_timeMode = g_cfg.timeMode;

        g_gifCount = load_gif_frames_res(IMG_GIF, g_gifFrames, MAX_GIF_FRAMES,
                                         &g_gifW, &g_gifH);

        HDC dc0 = GetDC(hWnd);
        int dpi = GetDeviceCaps(dc0, LOGPIXELSX);
        ReleaseDC(hWnd, dc0);
        g_ui = dpi / 96.0;
        if (g_ui < 1.2) g_ui = 1.2;
        if (g_ui > 2.5) g_ui = 2.5;
        load_pixel_font();
        create_fonts();

        DWORD bs = WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_TABSTOP;
        g_hCheck = CreateWindowW(L"BUTTON", L"", bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_CHECK, NULL, NULL);
        g_hChan  = CreateWindowW(L"BUTTON", L"", bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_CHAN,  NULL, NULL);
        g_hLang  = CreateWindowW(L"BUTTON", L"", bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_LANG,  NULL, NULL);
        g_hJoin  = CreateWindowW(L"BUTTON", L"", bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_JOIN,  NULL, NULL);
        g_hGit   = CreateWindowW(L"BUTTON", L"GitHub", bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_GIT, NULL, NULL);
        g_hPrev  = CreateWindowW(L"BUTTON", L"\x25C0", bs, 0,0,0,0, hWnd, (HMENU)ID_PREV, NULL, NULL);
        g_hNext  = CreateWindowW(L"BUTTON", L"\x25B6", bs, 0,0,0,0, hWnd, (HMENU)ID_NEXT, NULL, NULL);
        g_hSet   = CreateWindowW(L"BUTTON", L"\x2699", bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_SET, NULL, NULL);
        g_hTime  = CreateWindowW(L"BUTTON", L"", bs, 0,0,0,0, hWnd, (HMENU)ID_BTN_TIME, NULL, NULL);
        for (int i = 0; i < TAB_COUNT; ++i)
            g_hTabs[i] = CreateWindowW(L"BUTTON", L"", bs, 0,0,0,0, hWnd,
                                       (HMENU)(UINT_PTR)(ID_TAB0 + i), NULL, NULL);

        hover_register(g_hCheck); hover_register(g_hChan); hover_register(g_hLang);
        hover_register(g_hJoin);  hover_register(g_hGit);  hover_register(g_hPrev);
        hover_register(g_hNext);  hover_register(g_hSet);  hover_register(g_hTime);
        for (int i = 0; i < TAB_COUNT; ++i) hover_register(g_hTabs[i]);

        srand((unsigned)time(NULL));
        chara_init();
        music_scan();
        net_init(hWnd);
        tray_add(hWnd);
        apply_language();
        g_lastAutoMs = GetTickCount();
        SetTimer(hWnd, ID_TIMER, ANIM_MS, NULL);
        return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            if (g_cfg.trayMin) ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        compute_layout(LOWORD(lp), HIWORD(lp));
        bb_ensure(hWnd, LOWORD(lp), HIWORD(lp));
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 940;
        mmi->ptMinTrackSize.y = 640;
        return 0;
    }
    case WM_DPICHANGED: {
        g_ui = LOWORD(wp) / 96.0;
        if (g_ui < 1.2) g_ui = 1.2;
        if (g_ui > 2.5) g_ui = 2.5;
        create_fonts();
        RECT *nr = (RECT *)lp;
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
        int secTick = 0;
        for (int i = 0; ns[i] && i < 15; ++i)
            if (ns[i] != g_clockStr[i]) {
                g_clockOld[i] = g_clockStr[i]; g_clockChg[i] = now; secTick = 1;
            }
        wcscpy(g_clockStr, ns);

        if (g_tab == TAB_BOARD && !g_tabSlideMs) {
            int flipping = 0;
            for (int i = 0; g_clockStr[i] && i < 15; ++i)
                if (g_clockChg[i] && now - g_clockChg[i] < FLIP_MS + 80) flipping = 1;
            if (flipping) paint_clock_only(hWnd);
            /* countdown row ticks once a second */
            if (secTick && !IsRectEmpty(&g_cdRect))
                InvalidateRect(hWnd, &g_cdRect, FALSE);
        }

        /* tab slide */
        if (g_tabSlideMs) {
            if (now - g_tabSlideMs < SLIDE_MS) InvalidateRect(hWnd, &R_pageClip, FALSE);
            else { g_tabSlideMs = 0; InvalidateRect(hWnd, &R_pageClip, FALSE); }
        }

        /* mascot: phrases, bob, squash, particles, petals */
        if (chara_tick(hWnd, now)) {
            /* petal frame (~30 fps): re-present the cached scene with the
             * overlay - two blits, no scene recompose */
            RECT rc; GetClientRect(hWnd, &rc);
            present(hWnd, &rc);
        }

        if (g_tab == TAB_BOARD) {
            /* auto-switch the cover every ~6s */
            if (g_state == ST_OK && g_itemCount > 1 && now - g_coverMs >= 6000)
                set_cover(g_coverIndex + 1);
            if (g_coverSlideMs && now - g_coverSlideMs < SLIDE_MS)
                InvalidateRect(hWnd, &R_cover, FALSE);

            if (g_state == ST_OK) {
                int fxLive = 0;
                if (g_revealMs &&
                    now - g_revealMs < (DWORD)(g_itemCount + 1) * REVEAL_STAGGER + FX_REVEAL_MS)
                    fxLive = 1;
                if (g_relockMs && now - g_relockMs < FX_RELOCK_MS) fxLive = 1;
                if (fxLive) InvalidateRect(hWnd, &R_board, FALSE);
                else if (g_relockMs) g_relockMs = 0;
            }
            if (g_showGif && g_gifCount > 1 && now - g_gifMs >= GIF_FRAME_MS) {
                g_gifFrame = (g_gifFrame + 1) % g_gifCount;
                g_gifMs = now;
                if (!IsRectEmpty(&g_gifRect)) InvalidateRect(hWnd, &g_gifRect, FALSE);
            }
        }

        if (g_tab == TAB_REPLAYS) {
            if (g_vodRevealMs &&
                now - g_vodRevealMs < (DWORD)(g_vodCount + 1) * REVEAL_STAGGER + FX_REVEAL_MS)
                InvalidateRect(hWnd, &R_grid, FALSE);
            /* eased scrolling */
            if (g_vodScroll != (double)g_vodScrollT) {
                double d = g_vodScrollT - g_vodScroll;
                if (d > -0.6 && d < 0.6) g_vodScroll = g_vodScrollT;
                else g_vodScroll += d * 0.30;
                InvalidateRect(hWnd, &R_grid, FALSE);
            }
        }

        if (g_tab == TAB_MUSIC) {
            if (g_musScroll != (double)g_musScrollT) {
                double d = g_musScrollT - g_musScroll;
                if (d > -0.6 && d < 0.6) g_musScroll = g_musScrollT;
                else g_musScroll += d * 0.30;
                InvalidateRect(hWnd, &R_mlist, FALSE);
            }
            /* spinning vinyl + progress (~30 fps while playing) */
            static DWORD lastCard = 0;
            if (g_playing == 1 && now - lastCard >= 33) {
                lastCard = now;
                InvalidateRect(hWnd, &R_mcard, FALSE);
            }
        }

        /* auto check */
        if (g_cfg.autoCheckMin > 0 &&
            now - g_lastAutoMs >= (DWORD)g_cfg.autoCheckMin * 60000 && !net_busy()) {
            g_lastAutoMs = now;
            start_check(1);
        }

        hover_tick();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        bb_ensure(hWnd, rc.right, rc.bottom);
        HDC mem = g_bbDC;

        /* clip the compose to the dirty rect: drawing code still runs (so the
         * link hit-rects stay complete) but GDI skips rasterizing everything
         * outside it - tiny invalidations (clock, bob, vinyl) stay tiny */
        int svAll = SaveDC(mem);
        IntersectClipRect(mem, ps.rcPaint.left, ps.rcPaint.top,
                          ps.rcPaint.right, ps.rcPaint.bottom);

        g_linkCount = 0;
        draw_background(mem, &rc);
        draw_titlebar(mem);

        /* page content, clipped, with the horizontal tab slide */
        DWORD now = GetTickCount();
        int sv = SaveDC(mem);
        IntersectClipRect(mem, R_pageClip.left, R_pageClip.top,
                          R_pageClip.right, R_pageClip.bottom);
        if (g_tabSlideMs && now - g_tabSlideMs < SLIDE_MS) {
            double p = ease((double)(now - g_tabSlideMs) / SLIDE_MS);
            int W = rc.right;
            int offNew = (int)(g_tabDir * W * (1.0 - p));
            int offOld = offNew - g_tabDir * W;
            SetViewportOrgEx(mem, offOld, 0, NULL);
            pages_draw(mem, g_tabFrom);
            SetViewportOrgEx(mem, offNew, 0, NULL);
            g_linkCount = 0;                  /* only the new page is clickable */
            pages_draw(mem, g_tab);
            SetViewportOrgEx(mem, 0, 0, NULL);
        } else {
            pages_draw(mem, g_tab);
        }
        RestoreDC(mem, sv);

        chara_draw(mem);   /* petals are overlaid at present time, not here */
        chara_dlg_draw(mem, &rc);   /* galgame dialogue box, topmost */

        /* bottom strip: now-playing chip while music runs elsewhere, else beta */
        if (g_playing == 1 && g_curTrack >= 0 && g_tab != TAB_MUSIC) {
            wchar_t np[160];
            _snwprintf(np, 160, L"♪ %ls", g_tracks[g_curTrack].name);
            draw_text(mem, np, &g_betaRect, g_fSmall, C_PEKODK,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            draw_text(mem, LANGS[g_lang].beta, &g_betaRect, g_fSmall, C_RED,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        RestoreDC(mem, svAll);
        present_to(hWnd, hdc, &ps.rcPaint);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DRAWITEM:
        draw_button((LPDRAWITEMSTRUCT)lp);
        return TRUE;

    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        /* music page sliders first (they want drag, not click) */
        if (g_tab == TAB_MUSIC) {
            if (!IsRectEmpty(&g_seekRect) && PtInRect(&g_seekRect, p)) {
                g_drag = 1; SetCapture(hWnd); music_drag_apply(hWnd, p); return 0;
            }
            if (!IsRectEmpty(&g_mvolRect) && PtInRect(&g_mvolRect, p)) {
                g_drag = 2; SetCapture(hWnd); music_drag_apply(hWnd, p); return 0;
            }
        }
        /* links first, scanned in REVERSE draw order so whatever was drawn
         * last (dialogue box, talk chip) wins over the page beneath it */
        for (int i = g_linkCount - 1; i >= 0; --i)
            if (PtInRect(&g_links[i].rect, p)) {
                if (g_links[i].url[0] == L'!') do_action(hWnd, g_links[i].url);
                else ShellExecuteW(hWnd, L"open", g_links[i].url, NULL, NULL, SW_SHOWNORMAL);
                return 0;
            }
        if (chara_fortune_hit(hWnd, p)) return 0;     /* sakura omikuji */
        if (chara_click(hWnd, p)) return 0;           /* poke Pekora    */
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_drag) music_drag_apply(hWnd, p);
        else if (wp & MK_LBUTTON) chara_drag(hWnd, p);   /* petting */
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag) { g_drag = 0; ReleaseCapture(); cfg_save(); }
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int step = -delta / WHEEL_DELTA * S(64);
        if (g_tab == TAB_REPLAYS) {
            g_vodScrollT += step;
            int mx = pages_scroll_max(TAB_REPLAYS);
            if (g_vodScrollT < 0) g_vodScrollT = 0;
            if (g_vodScrollT > mx) g_vodScrollT = mx;
        } else if (g_tab == TAB_MUSIC) {
            g_musScrollT += step;
            int mx = pages_scroll_max(TAB_MUSIC);
            if (g_musScrollT < 0) g_musScrollT = 0;
            if (g_musScrollT > mx) g_musScrollT = mx;
        }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT p; GetCursorPos(&p); ScreenToClient(hWnd, &p);
        for (int i = 0; i < g_linkCount; ++i)
            if (PtInRect(&g_links[i].rect, p)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
        if (g_tab == TAB_MUSIC &&
            ((!IsRectEmpty(&g_seekRect) && PtInRect(&g_seekRect, p)) ||
             (!IsRectEmpty(&g_mvolRect) && PtInRect(&g_mvolRect, p)))) {
            SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE;
        }
        break;
    }
    case WM_APP_TRACKEND:
        music_on_trackend();
        return 0;

    case WM_APP_CHECKDONE:
        on_check_done(hWnd, (int)wp, (int)lp);
        return 0;
    case WM_APP_THUMBDONE:
        on_thumb_done(hWnd, (ThumbResult *)lp);
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case NIN_BALLOONUSERCLICK:
            tray_restore(hWnd);
            if (LOWORD(lp) == NIN_BALLOONUSERCLICK &&
                g_itemCount > 0 && g_items[0].isLive)
                ShellExecuteW(hWnd, L"open", g_items[0].url, NULL, NULL, SW_SHOWNORMAL);
            break;
        case WM_RBUTTONUP: {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, ID_TRAY_OPEN,  LANGS[g_lang].tray_open);
            AppendMenuW(m, MF_STRING, ID_TRAY_CHECK, LANGS[g_lang].tray_check);
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, ID_TRAY_EXIT,  LANGS[g_lang].tray_exit);
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(m);
            break;
        } }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_CHECK: start_check(0); break;
        case ID_BTN_CHAN:  ShellExecuteW(hWnd, L"open", CHANNEL_HOME, NULL, NULL, SW_SHOWNORMAL); break;
        case ID_BTN_JOIN:  ShellExecuteW(hWnd, L"open", JOIN_URL,     NULL, NULL, SW_SHOWNORMAL); break;
        case ID_BTN_GIT:   ShellExecuteW(hWnd, L"open", GITHUB_URL,   NULL, NULL, SW_SHOWNORMAL); break;
        case ID_FULLSCRN:  toggle_fullscreen(hWnd); break;
        case ID_BTN_LANG:  g_lang = lang_next(g_lang); apply_language(); break;
        case ID_PREV:      if (g_itemCount > 0) set_cover(g_coverIndex - 1); break;
        case ID_NEXT:      if (g_itemCount > 0) set_cover(g_coverIndex + 1); break;
        case ID_TAB0: case ID_TAB1: case ID_TAB2:
            tab_switch(hWnd, LOWORD(wp) - ID_TAB0);
            break;
        case ID_BTN_SET:   settings_menu(hWnd); break;
        case ID_SET_VOL:   volume_popup(hWnd); break;
        case ID_RES_720:  set_resolution(hWnd, 1280, 720);  break;
        case ID_RES_1080: set_resolution(hWnd, 1920, 1080); break;
        case ID_RES_1440: set_resolution(hWnd, 2560, 1440); break;
        case ID_AC_OFF: g_cfg.autoCheckMin = 0;  cfg_save(); break;
        case ID_AC_5:   g_cfg.autoCheckMin = 5;  g_lastAutoMs = GetTickCount(); cfg_save(); break;
        case ID_AC_10:  g_cfg.autoCheckMin = 10; g_lastAutoMs = GetTickCount(); cfg_save(); break;
        case ID_TGL_NOTIFY: g_cfg.notifyOn = !g_cfg.notifyOn; cfg_save(); break;
        case ID_TGL_PETALS:
            g_cfg.petalsOn = !g_cfg.petalsOn; cfg_save();
            InvalidateRect(hWnd, NULL, FALSE);
            break;
        case ID_TGL_TRAY: g_cfg.trayMin = !g_cfg.trayMin; cfg_save(); break;
        case ID_TRAY_OPEN:  tray_restore(hWnd); break;
        case ID_TRAY_CHECK: start_check(0); tray_restore(hWnd); break;
        case ID_TRAY_EXIT:  DestroyWindow(hWnd); break;
        case ID_BTN_TIME: {
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
                g_relockMs = GetTickCount();
            g_timeMode = (LOWORD(wp) == ID_TIME_SYS);
            g_cfg.timeMode = g_timeMode;
            cfg_save();
            InvalidateRect(hWnd, &R_board, FALSE);
            InvalidateRect(g_hTime, NULL, TRUE);
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, ID_TIMER);
        cfg_save();
        tray_remove();
        music_shutdown();
        net_shutdown();
        for (int i = 0; i < g_itemCount; ++i) if (g_items[i].thumb) DeleteObject(g_items[i].thumb);
        for (int i = 0; i < g_vodCount; ++i)  if (g_vods[i].thumb)  DeleteObject(g_vods[i].thumb);
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
        bb_destroy();
        img_shutdown();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hWnd, msg, wp, lp);
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

/* entry point */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int nShow)
{
    (void)hPrev; (void)cmd;

    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    SetCtxFn setCtx = (SetCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                               "SetProcessDpiAwarenessContext");
    if (setCtx) setCtx((HANDLE)-4);              /* PER_MONITOR_AWARE_V2 */
    else SetProcessDPIAware();

    timeBeginPeriod(1);                          /* tight SetTimer ticks */

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"PekoBoardCls";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    /* WS_CLIPCHILDREN: parent painting (incl. the 30 fps petal present pass)
     * never draws over the child buttons, so they don't flicker-repaint */
    RECT r = { 0, 0, 1920, 1080 };
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    HWND hWnd = CreateWindowW(L"PekoBoardCls", L"Peko Board", style,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        NULL, NULL, hInst, NULL);
    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    ACCEL acc = { FVIRTKEY, VK_F11, ID_FULLSCRN };
    HACCEL hAccel = CreateAcceleratorTableW(&acc, 1);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            chara_konami_key(msg.wParam);        /* ↑↑↓↓←→←→BA */
            /* galgame keys: T = talk, 1-3 = choices, Space/Enter = next,
             * Esc = close (handled here so they work whatever has focus) */
            if (msg.wParam == 'T' && !chara_dlg_active())
                chara_dlg_action(hWnd, L"!talk");
            else if (chara_dlg_active()) {
                if (msg.wParam >= '1' && msg.wParam <= '3') {
                    wchar_t a[8];
                    _snwprintf(a, 8, L"!dlg:%d", (int)(msg.wParam - '1'));
                    chara_dlg_action(hWnd, a);
                } else if (msg.wParam == VK_ESCAPE)
                    chara_dlg_action(hWnd, L"!dlgx");
                else if (msg.wParam == VK_SPACE || msg.wParam == VK_RETURN)
                    chara_dlg_action(hWnd, L"!dlgskip");
            }
        }
        if (!TranslateAcceleratorW(hWnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    timeEndPeriod(1);
    return (int)msg.wParam;
}

/*
 * peko_draw.c - fonts, reusable drawing helpers, the departure-board FX
 * engine, the lightweight button hover ink, and owner-drawn button painting
 * (including the hanafuda-card tab strip).
 */
#include "peko.h"

HFONT g_fTitle, g_fUI, g_fSmall, g_fBoard, g_fClock, g_fTag, g_fDate;
DWORD g_relockMs = 0;

static int     g_pixelOn = 0;
static wchar_t g_pixelFace[64] = L"";
static DWORD   g_fontQual = CLEARTYPE_QUALITY;

/* ---------- fonts ---------- */

const wchar_t *face_for_lang(void)
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
void load_pixel_font(void)
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

void create_fonts(void)
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
    g_fDate  = mkfont(S(24), FW_BOLD, f);
}

/* ---------- basic helpers ---------- */

void fill_round(HDC dc, int l, int t, int r, int b, int rad, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, l, t, r, b, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br);
}

/* smoothstep easing, clamped to 0..1 */
double ease(double p)
{
    if (p <= 0) return 0;
    if (p >= 1) return 1;
    return p * p * (3.0 - 2.0 * p);
}

/* single-call vertical gradient (GradientFill, no per-band loop) */
void grad_v(HDC dc, RECT *r, COLORREF top, COLORREF bot)
{
    TRIVERTEX v[2] = {
        { r->left,  r->top,    (COLOR16)(GetRValue(top)<<8), (COLOR16)(GetGValue(top)<<8), (COLOR16)(GetBValue(top)<<8), 0 },
        { r->right, r->bottom, (COLOR16)(GetRValue(bot)<<8), (COLOR16)(GetGValue(bot)<<8), (COLOR16)(GetBValue(bot)<<8), 0 }
    };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

void cover_blit(HDC dc, HBITMAP bmp, int iw, int ih,
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
void draw_sakura(HDC dc, int cx, int cy, int r, COLORREF c)
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

void draw_text(HDC dc, const wchar_t *s, RECT *r, HFONT f,
               COLORREF c, UINT fmt)
{
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, r, fmt);
    SelectObject(dc, of);
}

/* ---------- departure-board FX engine ---------- */

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

/* Compute this element's FX state. start=0 -> no animation (fully stable). */
FxState fx_eval(DWORD now, DWORD start, unsigned salt, int isReveal)
{
    FxState s = { 0, 0, 0, 0.0 };
    if (!start) return s;
    DWORD dur = isReveal ? FX_REVEAL_MS : FX_RELOCK_MS;
    DWORD el  = now - start;
    if (el >= dur) return s;

    unsigned slot = el / FX_FRAME_MS;
    unsigned h = fx_hash(salt * 2654435761U + slot * 40503U + (isReveal ? 7u : 19u));

    if (isReveal && el < (DWORD)FX_FRAME_MS) s.hidden = 1;
    else if ((h & 7u) == 0u && el < dur * 3 / 5) s.hidden = 1;

    if (!s.hidden) {
        double prog = (double)el / dur;
        double envelope = 1.0 - prog;
        double base = isReveal ? 0.55 : 0.35;
        s.dim = base * envelope * (0.45 + 0.55 * ((h >> 3) & 0xFF) / 255.0);
        if (FX_JITTER_PX > 0 && prog < 0.6) {
            s.dx = (int)((h >> 11) % (2u * FX_JITTER_PX + 1u)) - FX_JITTER_PX;
            s.dy = (int)((h >> 17) % (2u * FX_JITTER_PX + 1u)) - FX_JITTER_PX;
        }
    }
    return s;
}

/* FX for a schedule element that is revealing and may also be relocking. */
FxState fx_row(DWORD now, DWORD revealStart, unsigned salt)
{
    if (revealStart && now < revealStart + FX_REVEAL_MS)
        return fx_eval(now, revealStart, salt, 1);
    if (g_relockMs && now < g_relockMs + FX_RELOCK_MS)
        return fx_eval(now, g_relockMs, salt, 0);
    FxState s = { 0, 0, 0, 0.0 }; return s;
}

/* FX for always-present text: relock flicker only. */
FxState fx_persist(DWORD now, unsigned salt)
{
    if (g_relockMs && now < g_relockMs + FX_RELOCK_MS)
        return fx_eval(now, g_relockMs, salt, 0);
    FxState s = { 0, 0, 0, 0.0 }; return s;
}

void draw_text_fx(HDC dc, const wchar_t *s, RECT *r, HFONT f,
                  COLORREF c, UINT fmt, FxState fx)
{
    if (fx.hidden) return;
    RECT rr = *r;
    if (fx.dx | fx.dy) OffsetRect(&rr, fx.dx, fx.dy);
    draw_text(dc, s, &rr, f, fx_fade(c, fx.dim), fmt);
}

/* Tiles always render; only the glyph flickers. */
void draw_flaps_fx(HDC dc, int x, int y, int cw, int ch, int gap,
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

/* ---------- split-flap tile with a real card flip ---------- */

/* render one digit, centered, onto a cw x ch tile DC */
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

/* one split-flap tile: a real card flip from oldg to newg, p in 0..1. */
void draw_flap_tile(HDC dc, int cx, int y, int cw, int ch,
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

/* ---------- lightweight button hover ink ---------- */

#define HOVER_FADE_MS 150
#define HOVER_OPACITY 0.14
#define INK_MAX_BTN   16

typedef struct {
    HWND   hwnd;
    int    hovering;
    double alpha;
} HoverFx;
static HoverFx g_btn[INK_MAX_BTN];
static int     g_btnCount = 0;
static WNDPROC g_btnProc = NULL;

static HoverFx *hover_for(HWND h)
{
    for (int i = 0; i < g_btnCount; ++i)
        if (g_btn[i].hwnd == h) return &g_btn[i];
    return NULL;
}

static COLORREF hover_shade(COLORREF bg, double alpha)
{
    if (alpha <= 0.0) return bg;
    if (alpha > 1.0) alpha = 1.0;
    int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
    double t = (lum >= 140 ? 0.0 : 255.0);
    double a = alpha * HOVER_OPACITY;
    int r = (int)(GetRValue(bg) + (t - GetRValue(bg)) * a + 0.5);
    int g = (int)(GetGValue(bg) + (t - GetGValue(bg)) * a + 0.5);
    int b = (int)(GetBValue(bg) + (t - GetBValue(bg)) * a + 0.5);
    return RGB(r, g, b);
}

void hover_tick(void)
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

static LRESULT CALLBACK btn_subproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    HoverFx *f = hover_for(h);
    if (f && m == WM_ERASEBKGND)
        return 1;   /* draw_button paints the full rect - no white pre-erase */
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

void hover_register(HWND h)
{
    if (g_btnCount >= INK_MAX_BTN || !h) return;
    HoverFx *f = &g_btn[g_btnCount++];
    f->hwnd = h; f->hovering = 0; f->alpha = 0;
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)btn_subproc);
    if (!g_btnProc) g_btnProc = prev;
}

/* ---------- owner-drawn buttons ---------- */

void draw_button(LPDRAWITEMSTRUCT d)
{
    /* --- hanafuda-card tab buttons --- */
    if (d->CtlID >= ID_TAB0 && d->CtlID <= ID_TAB2) {
        int idx = (int)d->CtlID - ID_TAB0;
        int active = (g_tab == idx);
        RECT r = d->rcItem;
        /* the strip behind the tabs is the washi gradient; approximate with
         * the top paper colour so corners blend in */
        HBRUSH bb = CreateSolidBrush(C_PAPER_T);
        FillRect(d->hDC, &r, bb); DeleteObject(bb);

        COLORREF bg = active ? RGB(255, 255, 255) : C_INDIGO2;
        HoverFx *hf = hover_for(d->hwndItem);
        if (hf && hf->alpha > 0.0 && !active) bg = hover_shade(bg, hf->alpha);
        int lift = active ? 0 : S(3);              /* active card sits higher */
        fill_round(d->hDC, r.left, r.top + lift, r.right, r.bottom + S(8), 10, bg);
        if (active) {                               /* sakura edge on the card */
            HPEN pn = CreatePen(PS_SOLID, 2, C_SAKURA);
            HGDIOBJ op = SelectObject(d->hDC, pn);
            HGDIOBJ ob = SelectObject(d->hDC, GetStockObject(NULL_BRUSH));
            RoundRect(d->hDC, r.left, r.top, r.right, r.bottom + S(8), 10, 10);
            SelectObject(d->hDC, op); SelectObject(d->hDC, ob); DeleteObject(pn);
            draw_sakura(d->hDC, r.left + S(14), (r.top + r.bottom) / 2, S(6), C_SAKURA);
        }
        wchar_t txt[64]; GetWindowTextW(d->hwndItem, txt, 64);
        RECT tr = { r.left + (active ? S(24) : S(6)), r.top + lift, r.right, r.bottom };
        draw_text(d->hDC, txt, &tr, g_fUI,
                  active ? C_NAVY : RGB(255, 255, 255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    COLORREF bg;
    switch (d->CtlID) {
    case ID_BTN_CHECK: bg = C_CARROT;          break;
    case ID_BTN_CHAN:  bg = C_PEKO;            break;
    case ID_BTN_LANG:  bg = C_INDIGO2;         break;
    case ID_BTN_JOIN:  bg = C_GREEN;           break;
    case ID_BTN_GIT:   bg = RGB(64, 72, 88);   break;
    case ID_BTN_SET:   bg = C_INDIGO;          break;
    case ID_BTN_TIME:  bg = (g_timeMode == 0) ? C_RED : C_PEKODK; break;
    default:           bg = C_INDIGO2;         break;    /* prev/next */
    }
    if (d->itemState & ODS_SELECTED)
        bg = RGB(GetRValue(bg)*82/100, GetGValue(bg)*82/100, GetBValue(bg)*82/100);

    HoverFx *hf = hover_for(d->hwndItem);
    if (hf && hf->alpha > 0.0) bg = hover_shade(bg, hf->alpha);

    RECT r = d->rcItem;
    int titlebar = (d->CtlID == ID_BTN_SET || d->CtlID == ID_BTN_TIME);
    int prevnext = (d->CtlID == ID_PREV || d->CtlID == ID_NEXT);

    int rad = prevnext ? 0 : 12;
    if (prevnext) {
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(d->hDC, &r, br); DeleteObject(br);
    } else {
        COLORREF behind = titlebar ? C_INDIGO : C_PAPER_B;
        HBRUSH bb = CreateSolidBrush(behind);
        FillRect(d->hDC, &r, bb); DeleteObject(bb);
        fill_round(d->hDC, r.left, r.top, r.right, r.bottom, rad, bg);
    }

    if (d->CtlID == ID_BTN_TIME) {              /* cute clock icon */
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        int crad = (r.right - r.left) / 2 - S(7); if (crad < 5) crad = 5;
        HPEN pn = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(d->hDC, pn), ob = SelectObject(d->hDC, GetStockObject(NULL_BRUSH));
        Ellipse(d->hDC, cx - crad, cy - crad, cx + crad, cy + crad);
        MoveToEx(d->hDC, cx, cy, NULL); LineTo(d->hDC, cx, cy - crad + S(3));
        MoveToEx(d->hDC, cx, cy, NULL); LineTo(d->hDC, cx + crad - S(5), cy);
        SelectObject(d->hDC, op); SelectObject(d->hDC, ob); DeleteObject(pn);
        return;
    }

    wchar_t txt[64]; GetWindowTextW(d->hwndItem, txt, 64);
    draw_text(d->hDC, txt, &r, prevnext ? g_fTitle : g_fUI,
              RGB(255,255,255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

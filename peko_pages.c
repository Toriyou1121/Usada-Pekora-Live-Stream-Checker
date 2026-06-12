/*
 * peko_pages.c - everything drawn inside the tab area:
 *   BOARD    split-flap schedule board + live clock + countdown,
 *            cover carousel, X (Twitter) quick-links card
 *   REPLAYS  2-column grid of recent VOD cards (click -> browser)
 *   MUSIC    playlist + now-playing card with a spinning vinyl
 *
 * Clickable regions are recorded in g_links[]; a url starting with '!' is an
 * internal action dispatched by peko_main.c (e.g. "!mplay", "!mtrack:3").
 */
#include "peko.h"

/* ---------- shared page state ---------- */
LinkHit g_links[MAX_LINKS];
int     g_linkCount = 0;

int   g_coverIndex = 0, g_coverFrom = 0, g_coverDir = 1;
DWORD g_coverSlideMs = 0, g_coverMs = 0, g_revealMs = 0, g_vodRevealMs = 0;

int    g_vodScrollT = 0;  double g_vodScroll = 0;
int    g_musScrollT = 0;  double g_musScroll = 0;
RECT   g_seekRect, g_mvolRect;

HBITMAP g_gifFrames[MAX_GIF_FRAMES];
int     g_gifCount = 0, g_gifW = 0, g_gifH = 0, g_gifFrame = 0, g_showGif = 0;
DWORD   g_gifMs = 0;

void add_link(int lx, int ty, int rx, int by, const wchar_t *url)
{
    if (g_linkCount >= MAX_LINKS) return;
    g_links[g_linkCount].rect = (RECT){ lx, ty, rx, by };
    wcsncpy(g_links[g_linkCount].url, url, 159);
    g_links[g_linkCount].url[159] = 0;
    g_linkCount++;
}

/* ---------- background + title bar ---------- */

void draw_background(HDC dc, RECT *rc)
{
    grad_v(dc, rc, C_PAPER_T, C_PAPER_B);
}

void draw_titlebar(HDC dc)
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
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* ---------- clock ---------- */

void draw_clock(HDC dc)
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

/* repaint just the clock strip (cheap, flicker-free) for 60 fps flips: draw
 * the new digits straight into the cached scene, then re-present that strip */
void paint_clock_only(HWND hWnd)
{
    HDC mem = scene_dc();
    if (!mem || g_clockRect.right <= g_clockRect.left) return;
    draw_clock(mem);
    present(hWnd, &g_clockRect);
}

/* ---------- time formatting ---------- */

static void tz_offset_label(time_t e, wchar_t *out, int cap)
{
    struct tm g, l;
    struct tm *pg = gmtime(&e);
    if (!pg) { wcsncpy(out, L"GMT", cap); out[cap-1]=0; return; }
    g = *pg;
    struct tm *pl = localtime(&e);
    if (!pl) { wcsncpy(out, L"GMT", cap); out[cap-1]=0; return; }
    l = *pl;
    int dmin = (l.tm_hour - g.tm_hour) * 60 + (l.tm_min - g.tm_min);
    int dday = l.tm_yday - g.tm_yday;
    if (l.tm_year != g.tm_year) dday = (l.tm_year > g.tm_year) ? 1 : -1;
    dmin += dday * 24 * 60;
    int hh = dmin / 60, mm = dmin % 60; if (mm < 0) mm = -mm;
    if (mm) _snwprintf(out, cap, L"GMT%+d:%02d", hh, mm);
    else    _snwprintf(out, cap, L"GMT%+d", hh);
    out[cap-1] = 0;
}

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

/* ---------- board footer (loading GIF + hint) ---------- */

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

/* ---------- BOARD: the split-flap schedule board ---------- */

static void draw_board(HDC hdc)
{
    int L0 = R_board.left, T0 = R_board.top;
    int bw = R_board.right - L0, bh = R_board.bottom - T0;
    if (bw < 40 || bh < 40) return;
    const LangPack *L = &LANGS[g_lang];
    DWORD now = GetTickCount();
    SetRectEmpty(&g_cdRect);

    /* rounded card with a sky-blue -> sakura-pink vertical gradient */
    HRGN rgn = CreateRoundRectRgn(L0, T0, R_board.right + 1, R_board.bottom + 1, 20, 20);
    SelectClipRgn(hdc, rgn);
    grad_v(hdc, &R_board, C_SKY_L, C_PINK_L);
    SelectClipRgn(hdc, NULL); DeleteObject(rgn);
    HPEN bp = CreatePen(PS_SOLID, 2, C_SKY);
    HGDIOBJ obp = SelectObject(hdc, bp);
    HGDIOBJ obb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, L0, T0, R_board.right, R_board.bottom, 20, 20);
    SelectObject(hdc, obp); SelectObject(hdc, obb); DeleteObject(bp);

    int pad = S(18), x = L0 + pad, y = T0 + pad;
    draw_sakura(hdc, R_board.right - S(16), R_board.bottom - S(16), S(11), C_SAKURA);

    /* header: live clock right, date + weekday left */
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
        int availW = g_clkX - S(10) - x;
        if (lt) {
            _snwprintf(ds, 64, L"%04d/%02d/%02d (%ls)",
                       lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                       L->weekday[lt->tm_wday % 7]);
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
    y += S(44);

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

    /* status row (departure-board reveal) */
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

    /* countdown to the next upcoming stream (live ticking mini flaps) */
    {
        time_t bestUtc = 0; time_t nowt = time(NULL);
        for (int i = 0; i < g_itemCount; ++i)
            if (!g_items[i].isLive && g_items[i].hasTime && g_items[i].utc > nowt &&
                (!bestUtc || g_items[i].utc < bestUtc))
                bestUtc = g_items[i].utc;
        if (bestUtc) {
            long long d = (long long)(bestUtc - nowt);
            long long hh = d / 3600; int mm = (int)((d / 60) % 60), ss = (int)(d % 60);
            if (hh > 99) { hh = 99; mm = 59; ss = 59; }
            wchar_t cd[16];
            _snwprintf(cd, 16, L"%02lld:%02d:%02d", hh, mm, ss);
            RECT lr = { x, y, x + S(190), y + S(22) };
            draw_text(hdc, L->cd_label, &lr, g_fSmall, C_NAVY_DIM,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SIZE lsz; HGDIOBJ of = SelectObject(hdc, g_fSmall);
            GetTextExtentPoint32W(hdc, L->cd_label, (int)wcslen(L->cd_label), &lsz);
            SelectObject(hdc, of);
            int cx0 = x + lsz.cx + S(12);
            FxState fxn = { 0, 0, 0, 0.0 };
            draw_flaps_fx(hdc, cx0, y - S(2), S(13), S(22), S(2), cd, g_fSmall, fxn);
            g_cdRect = (RECT){ x, y - S(2), cx0 + 8 * (S(13) + S(2)) + S(4), y + S(22) };
            y += S(30);
        }
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
        DWORD rowStart = 0;
        if (g_revealMs) {
            long el = (long)(now - g_revealMs) - (long)(i + 1) * REVEAL_STAGGER;
            if (el < 0) { y += rowH; continue; }
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

        if (!it->isLive && tzp[0]) {
            RECT zr = { rx + fx.dx, y + S(27) + fx.dy, tx - S(6), y + S(27) + S(18) };
            draw_text_fx(hdc, tzp, &zr, g_fSmall, C_NAVY_DIM,
                         DT_LEFT | DT_TOP | DT_SINGLELINE, fx);
        }

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

/* ---------- BOARD: cover carousel ---------- */

static void blit_cover_photo(HDC dc, StreamItem *it, RECT *img, int ox)
{
    int iw = img->right - img->left;
    if (it && it->thumbState == 1 && it->thumb) {
        cover_blit(dc, it->thumb, it->tw, it->th, img->left + ox, img->top,
                   iw, img->bottom - img->top);
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
    const int FR = S(11), CAP = S(40);

    fill_round(dc, l, t, R_cover.right, R_cover.bottom, 16, C_SAKURA);
    fill_round(dc, l + 3, t + 3, R_cover.right - 3, R_cover.bottom - 3, 13, RGB(255,255,255));

    RECT img = { l + FR, t + FR, R_cover.right - FR, R_cover.bottom - FR - CAP };
    int iw = img.right - img.left;

    StreamItem *it = (g_state == ST_OK && g_coverIndex < g_itemCount)
                     ? &g_items[g_coverIndex] : NULL;

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

    HPEN pen = CreatePen(PS_SOLID, 2, C_PEKO);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, img.left - 1, img.top - 1, img.right + 1, img.bottom + 1);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);

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

    RECT cap = { l + FR, R_cover.bottom - FR - CAP + S(4), R_cover.right - S(62), R_cover.bottom - S(6) };
    draw_text(dc, it ? it->title : L"-", &cap, g_fSmall, C_NAVY,
              DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    if (g_itemCount > 0) {
        wchar_t idx[16]; _snwprintf(idx, 16, L"%d / %d", g_coverIndex + 1, g_itemCount);
        RECT ir = { R_cover.right - S(60), R_cover.bottom - FR - S(26), R_cover.right - S(10), R_cover.bottom - S(8) };
        draw_text(dc, idx, &ir, g_fSmall, C_PEKODK, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void set_cover(int idx)
{
    if (g_itemCount <= 0) { g_coverIndex = 0; return; }
    int old = g_coverIndex;
    int ni  = ((idx % g_itemCount) + g_itemCount) % g_itemCount;
    if (ni != old) {
        g_coverFrom = old;
        g_coverDir  = (idx >= old) ? 1 : -1;
        g_coverSlideMs = GetTickCount();
        net_request_thumb(0, ni);
    }
    g_coverIndex = ni;
    g_coverMs = GetTickCount();
    InvalidateRect(g_hMain, &R_cover, FALSE);
}

/* ---------- BOARD: X (Twitter) quick-links card ---------- */

static void draw_xcard(HDC dc)
{
    int l = R_xcard.left, t = R_xcard.top, r = R_xcard.right, b = R_xcard.bottom;
    if (r - l < S(120) || b - t < S(40)) return;
    fill_round(dc, l, t, r, b, 14, RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, 2, C_SKY);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, l, t, r, b, 14, 14);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);

    /* black X badge */
    int bx = l + S(12), bs = S(28), by = t + ((b - t) - bs) / 2;
    fill_round(dc, bx, by, bx + bs, by + bs, 8, RGB(20, 22, 28));
    RECT xr = { bx, by, bx + bs, by + bs };
    draw_text(dc, L"X", &xr, g_fUI, RGB(255,255,255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* caption above the chips when there is room, else chips only */
    const LangPack *L = &LANGS[g_lang];
    const wchar_t *labels[3] = { L->x_profile, L->x_latest, L->x_art };
    const wchar_t *urls[3]   = { X_PROFILE_URL, X_LATEST_URL, X_ART_URL };
    COLORREF cols[3]         = { C_PEKODK, C_PEKO, C_SAKURA_D };

    int cx = bx + bs + S(12);
    int availW = r - S(12) - cx;
    int chipW = (availW - 2 * S(8)) / 3;
    int chipH = S(30), cy = t + ((b - t) - chipH) / 2;
    for (int i = 0; i < 3; ++i) {
        int x0 = cx + i * (chipW + S(8));
        fill_round(dc, x0, cy, x0 + chipW, cy + chipH, 10, cols[i]);
        RECT cr = { x0 + S(4), cy, x0 + chipW - S(4), cy + chipH };
        draw_text(dc, labels[i], &cr, g_fSmall, RGB(255,255,255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        add_link(x0, cy, x0 + chipW, cy + chipH, urls[i]);
    }
}

/* ---------- REPLAYS page ---------- */

static int vod_card_h(void)
{
    int gap = S(14);
    int cw = (R_grid.right - R_grid.left - gap) / 2;
    return (cw - 2 * S(8)) * 9 / 16 + S(58);
}

int pages_scroll_max(int tab)
{
    if (tab == TAB_REPLAYS) {
        int rows = (g_vodCount + 1) / 2;
        int content = rows * (vod_card_h() + S(14)) + S(54);
        int view = R_grid.bottom - R_grid.top;
        return content > view ? content - view : 0;
    }
    if (tab == TAB_MUSIC) {
        int content = g_trackCount * S(36) + S(60);
        int view = R_mlist.bottom - R_mlist.top;
        return content > view ? content - view : 0;
    }
    return 0;
}

static void draw_replays(HDC dc)
{
    const LangPack *L = &LANGS[g_lang];
    DWORD now = GetTickCount();
    int x = R_grid.left, y0 = R_grid.top;

    RECT hr = { x, y0, R_grid.right, y0 + S(30) };
    draw_text(dc, L->rp_title, &hr, g_fDate, C_PEKODK, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT hh = { x, y0 + S(30), R_grid.right, y0 + S(50) };
    draw_text(dc, L->rp_hint, &hh, g_fSmall, C_NAVY_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int top = y0 + S(54);
    if (g_vodCount == 0) {
        RECT nr = { x, top + S(10), R_grid.right, top + S(120) };
        draw_text(dc, L->rp_none, &nr, g_fBoard, C_NAVY_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK);
        return;
    }

    int sv = SaveDC(dc);
    IntersectClipRect(dc, R_grid.left, top, R_grid.right, R_grid.bottom);

    int gap = S(14);
    int cw = (R_grid.right - R_grid.left - gap) / 2;
    int ch = vod_card_h();
    int scroll = (int)g_vodScroll;

    for (int i = 0; i < g_vodCount; ++i) {
        int col = i % 2, row = i / 2;
        int cl = x + col * (cw + gap);
        int ct = top + row * (ch + gap) - scroll;
        if (ct + ch < top || ct > R_grid.bottom) continue;

        /* staggered reveal like the board rows */
        if (g_vodRevealMs) {
            long el = (long)(now - g_vodRevealMs) - (long)(i + 1) * REVEAL_STAGGER;
            if (el < 0) continue;
        }
        FxState fx = fx_row(now,
            g_vodRevealMs ? g_vodRevealMs + (DWORD)(i + 1) * REVEAL_STAGGER : 0,
            (unsigned)(40 + i));

        StreamItem *it = &g_vods[i];
        if (it->thumbState == 0) net_request_thumb(1, i);

        /* mini photo frame */
        fill_round(dc, cl, ct, cl + cw, ct + ch, 12, C_SAKURA);
        fill_round(dc, cl + 2, ct + 2, cl + cw - 2, ct + ch - 2, 10, RGB(255,255,255));
        RECT img = { cl + S(8), ct + S(8), cl + cw - S(8),
                     ct + S(8) + (cw - 2 * S(8)) * 9 / 16 };
        if (it->thumbState == 1 && it->thumb)
            cover_blit(dc, it->thumb, it->tw, it->th,
                       img.left, img.top, img.right - img.left, img.bottom - img.top);
        else {
            HBRUSH b = CreateSolidBrush(C_SKY_L);
            FillRect(dc, &img, b); DeleteObject(b);
            draw_text(dc, L->no_cover, &img, g_fSmall, C_PEKO,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        /* play badge */
        int pb = S(22);
        fill_round(dc, img.left + S(6), img.bottom - pb - S(6),
                   img.left + S(6) + pb + S(20), img.bottom - S(6), 6, C_PEKODK);
        RECT pr = { img.left + S(6), img.bottom - pb - S(6),
                    img.left + S(6) + pb + S(20), img.bottom - S(6) };
        draw_text(dc, L"\x25B6", &pr, g_fSmall, RGB(255,255,255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT cap = { cl + S(10), img.bottom + S(4), cl + cw - S(10), ct + ch - S(6) };
        draw_text_fx(dc, it->title, &cap, g_fSmall, C_NAVY,
                     DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS, fx);

        add_link(cl, ct, cl + cw, ct + ch, it->url);
    }
    RestoreDC(dc, sv);
}

/* ---------- MUSIC page ---------- */

static void chip(HDC dc, int x, int y, int w, int h, const wchar_t *txt,
                 COLORREF bg, const wchar_t *action)
{
    fill_round(dc, x, y, x + w, y + h, 10, bg);
    RECT r = { x, y, x + w, y + h };
    draw_text(dc, txt, &r, g_fUI, RGB(255,255,255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    add_link(x, y, x + w, y + h, action);
}

static void fmt_mmss(int ms, wchar_t *out, int cap)
{
    int s = ms / 1000;
    _snwprintf(out, cap, L"%02d:%02d", s / 60, s % 60);
}

static void draw_music(HDC dc)
{
    const LangPack *L = &LANGS[g_lang];
    SetRectEmpty(&g_seekRect);
    SetRectEmpty(&g_mvolRect);

    /* ---- playlist (left) ---- */
    int x = R_mlist.left, y0 = R_mlist.top;
    RECT hr = { x, y0, R_mlist.right, y0 + S(30) };
    draw_text(dc, L->mu_title, &hr, g_fDate, C_PEKODK, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int top = y0 + S(40);
    if (g_trackCount == 0) {
        RECT nr = { x, top + S(8), R_mlist.right, top + S(140) };
        draw_text(dc, L->mu_none, &nr, g_fBoard, C_NAVY_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        int sv = SaveDC(dc);
        IntersectClipRect(dc, R_mlist.left, top, R_mlist.right, R_mlist.bottom);
        int rowH = S(36), scroll = (int)g_musScroll;
        for (int i = 0; i < g_trackCount; ++i) {
            int ry = top + i * rowH - scroll;
            if (ry + rowH < top || ry > R_mlist.bottom) continue;
            int cur = (i == g_curTrack);
            if (cur)
                fill_round(dc, x, ry + S(2), R_mlist.right - S(4), ry + rowH - S(2), 8,
                           g_playing == 1 ? C_SAKURA : RGB(255, 222, 235));
            wchar_t num[8]; _snwprintf(num, 8, L"%02d", i + 1);
            RECT nrr = { x + S(8), ry, x + S(40), ry + rowH };
            draw_text(dc, num, &nrr, g_fSmall, cur ? RGB(255,255,255) : C_NAVY_DIM,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT trr = { x + S(46), ry, R_mlist.right - S(34), ry + rowH };
            draw_text(dc, g_tracks[i].name, &trr, cur ? g_fUI : g_fSmall,
                      cur ? RGB(255,255,255) : C_NAVY,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (cur && g_playing == 1) {
                RECT mr = { R_mlist.right - S(30), ry, R_mlist.right - S(8), ry + rowH };
                draw_text(dc, L"♪", &mr, g_fUI, RGB(255,255,255),
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            wchar_t act[24]; _snwprintf(act, 24, L"!mtrack:%d", i);
            add_link(x, ry, R_mlist.right - S(4), ry + rowH, act);
        }
        RestoreDC(dc, sv);
    }

    /* ---- now-playing card (right) ---- */
    int l = R_mcard.left, t = R_mcard.top, r = R_mcard.right, b = R_mcard.bottom;
    if (r - l < S(180) || b - t < S(200)) return;
    fill_round(dc, l, t, r, b, 16, RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, 2, C_SKY);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, l, t, r, b, 16, 16);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);

    int cw = r - l;
    int cx = l + cw / 2;

    /* spinning vinyl */
    int rr = (cw - S(60)) / 2; if (rr > S(74)) rr = S(74); if (rr < S(30)) rr = S(30);
    int cyv = t + S(20) + rr;
    {
        HBRUSH disc = CreateSolidBrush(RGB(40, 44, 60));
        HGDIOBJ obb2 = SelectObject(dc, disc);
        HGDIOBJ opp = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - rr, cyv - rr, cx + rr, cyv + rr);
        SelectObject(dc, opp); SelectObject(dc, obb2); DeleteObject(disc);
        /* grooves */
        HPEN gp = CreatePen(PS_SOLID, 1, RGB(70, 76, 96));
        HGDIOBJ og = SelectObject(dc, gp);
        HGDIOBJ onb = SelectObject(dc, GetStockObject(NULL_BRUSH));
        for (int g = rr - S(8); g > rr / 2; g -= S(7))
            Ellipse(dc, cx - g, cyv - g, cx + g, cyv + g);
        SelectObject(dc, og); SelectObject(dc, onb); DeleteObject(gp);
        /* label: the track's cover art (cover\<name>.png/jpg), SPINNING with
         * the record via PlgBlt; carrot-orange label when no art exists */
        int cw2, ch2;
        HBITMAP art = music_cover(&cw2, &ch2);
        int lr = art ? rr * 7 / 10 : rr * 2 / 5;
        if (art && cw2 > 0 && ch2 > 0) {
            int sv2 = SaveDC(dc);
            HRGN circ = CreateEllipticRgn(cx - lr, cyv - lr, cx + lr + 1, cyv + lr + 1);
            ExtSelectClipRgn(dc, circ, RGN_AND);
            DeleteObject(circ);
            double a = (g_playing == 1)
                       ? (GetTickCount() % 4800) * 6.28318 / 4800.0 : 0.0;
            double ca = cos(a), sa = sin(a);
            POINT pts[3] = {
                { cx + (int)(-lr * ca + lr * sa), cyv + (int)(-lr * sa - lr * ca) },
                { cx + (int)( lr * ca + lr * sa), cyv + (int)( lr * sa - lr * ca) },
                { cx + (int)(-lr * ca - lr * sa), cyv + (int)(-lr * sa + lr * ca) },
            };
            HDC am = CreateCompatibleDC(dc);
            HGDIOBJ oam = SelectObject(am, art);
            int side = cw2 < ch2 ? cw2 : ch2;       /* center-square crop */
            PlgBlt(dc, pts, am, (cw2 - side) / 2, (ch2 - side) / 2,
                   side, side, NULL, 0, 0);
            SelectObject(am, oam); DeleteDC(am);
            RestoreDC(dc, sv2);
        } else {
            HBRUSH lb = CreateSolidBrush(C_CARROT);
            HGDIOBJ olb = SelectObject(dc, lb);
            HGDIOBJ opp2 = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, cx - lr, cyv - lr, cx + lr, cyv + lr);
            SelectObject(dc, olb); SelectObject(dc, opp2);
            DeleteObject(lb);
        }
        /* label rim + spindle */
        {
            HPEN rim = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ orim = SelectObject(dc, rim);
            HGDIOBJ onb2 = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, cx - lr, cyv - lr, cx + lr, cyv + lr);
            SelectObject(dc, orim); SelectObject(dc, onb2); DeleteObject(rim);
            HBRUSH sb = CreateSolidBrush(RGB(255, 255, 255));
            HGDIOBJ osb = SelectObject(dc, sb);
            HGDIOBJ opn2 = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, cx - S(4), cyv - S(4), cx + S(4), cyv + S(4));
            SelectObject(dc, osb); SelectObject(dc, opn2); DeleteObject(sb);
        }
        /* orbiting sparkle shows rotation while playing (no-art records) */
        if (g_playing == 1 && !art) {
            double a = (GetTickCount() % 2400) * 6.28318 / 2400.0;
            int px = cx + (int)((lr + (rr - lr) / 2) * cos(a));
            int py = cyv + (int)((lr + (rr - lr) / 2) * sin(a));
            HBRUSH hb = CreateSolidBrush(RGB(255, 232, 120));
            HGDIOBJ ohb = SelectObject(dc, hb);
            HGDIOBJ opn3 = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, px - S(4), py - S(4), px + S(4), py + S(4));
            SelectObject(dc, ohb); SelectObject(dc, opn3);
            DeleteObject(hb);
        }
    }

    int y = cyv + rr + S(12);
    RECT npr = { l + S(12), y, r - S(12), y + S(20) };
    draw_text(dc, g_playing ? L->mu_now : L->mu_idle, &npr, g_fSmall,
              g_playing == 1 ? C_SAKURA_D : C_NAVY_DIM,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    y += S(22);
    RECT tnr = { l + S(12), y, r - S(12), y + S(46) };
    draw_text(dc, g_curTrack >= 0 ? g_tracks[g_curTrack].name : L"-", &tnr, g_fUI,
              C_NAVY, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    y += S(50);

    /* progress bar + time */
    int pos = music_pos_ms(), len = music_len_ms();
    {
        int bx = l + S(16), bw2 = r - S(16) - bx, bh = S(8);
        fill_round(dc, bx, y, bx + bw2, y + bh, 4, RGB(228, 234, 246));
        if (len > 0) {
            int fw = (int)((double)pos / len * bw2);
            if (fw > bw2) fw = bw2;
            fill_round(dc, bx, y, bx + fw, y + bh, 4, C_PEKO);
            int kx = bx + fw;
            HBRUSH kb = CreateSolidBrush(C_CARROT);
            HGDIOBJ okb = SelectObject(dc, kb);
            HGDIOBJ opk = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, kx - S(6), y + bh / 2 - S(6), kx + S(6), y + bh / 2 + S(6));
            SelectObject(dc, okb); SelectObject(dc, opk); DeleteObject(kb);
        }
        g_seekRect = (RECT){ bx, y - S(6), bx + bw2, y + bh + S(6) };
        y += bh + S(6);
        wchar_t ts[32], p1[16], p2[16];
        fmt_mmss(pos, p1, 16); fmt_mmss(len, p2, 16);
        _snwprintf(ts, 32, L"%ls / %ls", p1, p2);
        RECT tr2 = { l + S(12), y, r - S(12), y + S(18) };
        draw_text(dc, ts, &tr2, g_fSmall, C_NAVY_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        y += S(24);
    }

    /* transport chips (skip what no longer fits at small window heights) */
    if (y + S(34) < b) {
        int chw = S(46), chh = S(34), gap2 = S(10);
        int total = 3 * chw + 2 * gap2;
        int x0 = cx - total / 2;
        chip(dc, x0, y, chw, chh, L"\x25C0\x25C0", C_INDIGO2, L"!mprev");
        /* pause is plain "||" so the pixel font is guaranteed to have it */
        chip(dc, x0 + chw + gap2, y, chw, chh,
             g_playing == 1 ? L"||" : L"\x25B6", C_CARROT, L"!mplay");
        chip(dc, x0 + 2 * (chw + gap2), y, chw, chh, L"\x25B6\x25B6", C_INDIGO2, L"!mnext");
        y += chh + S(8);
        /* loop + shuffle */
        const wchar_t *loopTxt = g_cfg.loopMode == 0 ? L"LOOP: OFF" :
                                 g_cfg.loopMode == 1 ? L"LOOP: ALL" : L"LOOP: ONE";
        int lw = S(104), sw = S(104);
        int x1 = cx - (lw + sw + gap2) / 2;
        fill_round(dc, x1, y, x1 + lw, y + S(26), 8,
                   g_cfg.loopMode ? C_PEKODK : RGB(190, 200, 216));
        RECT lr2 = { x1, y, x1 + lw, y + S(26) };
        draw_text(dc, loopTxt, &lr2, g_fSmall, RGB(255,255,255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_link(x1, y, x1 + lw, y + S(26), L"!mloop");
        int x2 = x1 + lw + gap2;
        fill_round(dc, x2, y, x2 + sw, y + S(26), 8,
                   g_cfg.shuffle ? C_SAKURA_D : RGB(190, 200, 216));
        RECT sr2 = { x2, y, x2 + sw, y + S(26) };
        draw_text(dc, L"SHUFFLE", &sr2, g_fSmall, RGB(255,255,255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_link(x2, y, x2 + sw, y + S(26), L"!mshuf");
        y += S(34);
    }

    /* volume slider */
    if (y + S(28) < b) {
        wchar_t vl[48];
        _snwprintf(vl, 48, L"%ls  %d", L->vol_music, g_cfg.musicVol);
        RECT vr = { l + S(16), y, l + S(140), y + S(20) };
        draw_text(dc, vl, &vr, g_fSmall, C_NAVY_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        int bx = l + S(146), bw2 = r - S(20) - bx, bh = S(6);
        int by = y + S(7);
        if (bw2 > S(60)) {
            fill_round(dc, bx, by, bx + bw2, by + bh, 3, RGB(228, 234, 246));
            int fw = g_cfg.musicVol * bw2 / 100;
            fill_round(dc, bx, by, bx + fw, by + bh, 3, C_SAKURA_D);
            HBRUSH kb = CreateSolidBrush(C_CARROT);
            HGDIOBJ okb = SelectObject(dc, kb);
            HGDIOBJ opk = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, bx + fw - S(5), by + bh / 2 - S(7), bx + fw + S(5), by + bh / 2 + S(7));
            SelectObject(dc, okb); SelectObject(dc, opk); DeleteObject(kb);
            g_mvolRect = (RECT){ bx, by - S(8), bx + bw2, by + bh + S(8) };
        }
    }
}

/* ---------- dispatcher ---------- */

void pages_draw(HDC dc, int tab)
{
    switch (tab) {
    case TAB_BOARD:
        draw_cover(dc);
        draw_xcard(dc);
        draw_board(dc);
        break;
    case TAB_REPLAYS:
        draw_replays(dc);
        break;
    case TAB_MUSIC:
        draw_music(dc);
        break;
    }
}

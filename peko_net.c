/*
 * peko_net.c - HTTPS GET (WinINet) + the worker-thread layer.
 *
 * Nothing here ever blocks the UI thread:
 *   - net_start_check() spawns a one-shot thread that fetches + parses the
 *     streams page into a staging ParseResult, then posts WM_APP_CHECKDONE.
 *     The UI thread swaps the staging data in with net_take_result().
 *   - A single long-lived thumbnail thread services a small queue; each
 *     finished JPG is decoded off-thread and posted as WM_APP_THUMBDONE with
 *     a malloc'd ThumbResult (the UI handler owns/frees it).
 *
 * A generation counter guards against stale thumbnails: every new check bumps
 * it, and results carrying an old generation are discarded on arrival.
 */
#include "peko.h"

/* HTTPS GET into a malloc'd buffer (works for HTML text and binary JPGs).
 * Uses the machine's configured proxy, so a system VPN/proxy is honored.
 * The PREF=tz cookie makes YouTube render schedule times in JST server-side. */
char *fetch_url(const wchar_t *url, size_t *out_size)
{
    *out_size = 0;
    HINTERNET hInet = InternetOpenW(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) PekoBoard/4.0",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInet) return NULL;
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

/* ---------- check thread (one-shot) ---------- */

static HWND          g_notifyWnd;
static volatile LONG g_checkBusy;
static ParseResult   g_staging;          /* filled by the worker, swapped by UI */
static volatile LONG g_quitting;

typedef struct { int isAuto; } CheckArgs;

static DWORD WINAPI check_thread(LPVOID arg)
{
    int isAuto = ((CheckArgs *)arg)->isAuto;
    free(arg);

    size_t size = 0;
    char *html = fetch_url(STREAMS_URL, &size);
    int ok = 0;
    if (html && size >= 4096 &&
        (strstr(html, "ytInitialData") || strstr(html, "lockupViewModel"))) {
        parse_streams(html, &g_staging);
        ok = 1;
    }
    if (html) free(html);
    if (!ok) InterlockedExchange(&g_checkBusy, 0);   /* nothing to take */
    if (!g_quitting)
        PostMessageW(g_notifyWnd, WM_APP_CHECKDONE, (WPARAM)ok, (LPARAM)isAuto);
    return 0;
}

int net_busy(void) { return g_checkBusy != 0; }

int net_start_check(int isAuto)
{
    if (InterlockedCompareExchange(&g_checkBusy, 1, 0) != 0) return 0;
    CheckArgs *a = (CheckArgs *)malloc(sizeof *a);
    if (!a) { InterlockedExchange(&g_checkBusy, 0); return 0; }
    a->isAuto = isAuto;
    HANDLE h = CreateThread(NULL, 0, check_thread, a, 0, NULL);
    if (!h) { free(a); InterlockedExchange(&g_checkBusy, 0); return 0; }
    CloseHandle(h);
    return 1;
}

/* UI thread: copy the staging result out and release the busy flag */
void net_take_result(ParseResult *out)
{
    *out = g_staging;
    InterlockedExchange(&g_checkBusy, 0);
}

/* ---------- thumbnail thread (long-lived, small queue) ---------- */

typedef struct { unsigned gen; int kind, index; char id[16]; } ThumbReq;

#define TQ_CAP 128
static ThumbReq         g_tq[TQ_CAP];
static int              g_tqHead, g_tqTail;
static CRITICAL_SECTION g_tqLock;
static HANDLE           g_tqEvent;
static HANDLE           g_thumbThread;
static volatile LONG    g_thumbGen = 1;

unsigned net_thumb_gen(void)  { return (unsigned)g_thumbGen; }
void net_bump_thumb_gen(void) { InterlockedIncrement(&g_thumbGen); }

static int tq_pop(ThumbReq *out)
{
    int got = 0;
    EnterCriticalSection(&g_tqLock);
    if (g_tqHead != g_tqTail) {
        *out = g_tq[g_tqHead];
        g_tqHead = (g_tqHead + 1) % TQ_CAP;
        got = 1;
    }
    LeaveCriticalSection(&g_tqLock);
    return got;
}

static DWORD WINAPI thumb_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_tqEvent, INFINITE);
        if (g_quitting) return 0;
        ThumbReq rq;
        while (tq_pop(&rq)) {
            if (g_quitting) return 0;
            if (rq.gen != (unsigned)g_thumbGen) continue;      /* stale */
            /* prefer the highest resolution available */
            static const wchar_t *qual[] = { L"maxresdefault", L"sddefault", L"hqdefault" };
            HBITMAP hb = NULL; int w = 0, h = 0;
            for (int q = 0; q < 3 && !hb; ++q) {
                wchar_t url[160];
                _snwprintf(url, 160, L"https://i.ytimg.com/vi/%hs/%ls.jpg", rq.id, qual[q]);
                size_t sz = 0;
                char *d = fetch_url(url, &sz);
                if (d && sz > 2500)
                    hb = bitmap_from_memory(d, (DWORD)sz, &w, &h, 0xFF101418);
                if (d) free(d);
            }
            ThumbResult *res = (ThumbResult *)malloc(sizeof *res);
            if (!res) { if (hb) DeleteObject(hb); continue; }
            res->gen = rq.gen; res->kind = rq.kind; res->index = rq.index;
            res->bmp = hb; res->w = w; res->h = h;
            memcpy(res->id, rq.id, 16);
            if (g_quitting ||
                !PostMessageW(g_notifyWnd, WM_APP_THUMBDONE, 0, (LPARAM)res)) {
                if (hb) DeleteObject(hb);
                free(res);
            }
        }
    }
}

/* UI thread: queue one stream's thumbnail download (kind 0=items, 1=vods) */
void net_request_thumb(int kind, int index)
{
    StreamItem *it = NULL;
    if (kind == 0 && index >= 0 && index < g_itemCount) it = &g_items[index];
    if (kind == 1 && index >= 0 && index < g_vodCount)  it = &g_vods[index];
    if (!it || !it->id[0] || it->thumbState != 0) return;
    it->thumbState = 3;                                  /* queued */

    EnterCriticalSection(&g_tqLock);
    int next = (g_tqTail + 1) % TQ_CAP;
    if (next != g_tqHead) {
        ThumbReq *rq = &g_tq[g_tqTail];
        rq->gen = (unsigned)g_thumbGen;
        rq->kind = kind; rq->index = index;
        memcpy(rq->id, it->id, 16);
        g_tqTail = next;
    }
    LeaveCriticalSection(&g_tqLock);
    SetEvent(g_tqEvent);
}

void net_init(HWND hwndMain)
{
    g_notifyWnd = hwndMain;
    InitializeCriticalSection(&g_tqLock);
    g_tqEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thumbThread = CreateThread(NULL, 0, thumb_thread, NULL, 0, NULL);
}

void net_shutdown(void)
{
    g_quitting = 1;
    if (g_tqEvent) SetEvent(g_tqEvent);
    if (g_thumbThread) {
        /* give the worker a moment; if it is mid-download just let process
         * teardown reclaim it (WinINet handles close with the process) */
        WaitForSingleObject(g_thumbThread, 500);
        CloseHandle(g_thumbThread);
        g_thumbThread = NULL;
    }
}

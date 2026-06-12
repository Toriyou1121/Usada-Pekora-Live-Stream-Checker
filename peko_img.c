/*
 * peko_img.c - GDI+ image decode: PNG/JPG -> HBITMAP, animated GIF -> frame
 * list, and embedded PNG -> HICON (for the tray). The normal gdiplus.h header
 * is C++-only, so the "flat" C entry points are declared here by hand.
 */
#include "peko.h"

typedef struct { UINT32 v; void *cb; BOOL sb; BOOL se; } GpStartupInput;
__declspec(dllimport) int  WINAPI GdiplusStartup(ULONG_PTR*, const GpStartupInput*, void*);
__declspec(dllimport) void WINAPI GdiplusShutdown(ULONG_PTR);
__declspec(dllimport) int  WINAPI GdipCreateBitmapFromStream(void*, void**);
__declspec(dllimport) int  WINAPI GdipCreateBitmapFromFile(const WCHAR*, void**);
__declspec(dllimport) int  WINAPI GdipCreateHBITMAPFromBitmap(void*, HBITMAP*, DWORD);
__declspec(dllimport) int  WINAPI GdipCreateHICONFromBitmap(void*, HICON*);
__declspec(dllimport) int  WINAPI GdipGetImageWidth(void*, UINT*);
__declspec(dllimport) int  WINAPI GdipGetImageHeight(void*, UINT*);
__declspec(dllimport) int  WINAPI GdipDisposeImage(void*);
__declspec(dllimport) int  WINAPI GdipImageGetFrameCount(void*, const GUID*, UINT*);
__declspec(dllimport) int  WINAPI GdipImageSelectActiveFrame(void*, const GUID*, UINT);

/* the GIF's per-frame "time" dimension GUID (FrameDimensionTime) */
static const GUID GIF_DIM_TIME =
    { 0x6aedbd6d, 0x3fb5, 0x418a, { 0x83,0xa6,0x7f,0x45,0x22,0x9d,0xc8,0x72 } };

static ULONG_PTR g_gdip;

int img_init(void)
{
    GpStartupInput si; ZeroMemory(&si, sizeof si); si.v = 1;
    return GdiplusStartup(&g_gdip, &si, NULL) == 0;
}

void img_shutdown(void)
{
    if (g_gdip) { GdiplusShutdown(g_gdip); g_gdip = 0; }
}

/* wrap a memory blob in an IStream and hand it to a GDI+ consumer */
static IStream *stream_from_memory(const void *data, DWORD size)
{
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return NULL;
    void *mp = GlobalLock(hMem);
    memcpy(mp, data, size);
    GlobalUnlock(hMem);
    IStream *stm = NULL;
    if (CreateStreamOnHGlobal(hMem, TRUE, &stm) != S_OK) { GlobalFree(hMem); return NULL; }
    return stm;   /* releasing the stream frees hMem */
}

HBITMAP bitmap_from_memory(const void *data, DWORD size, int *w, int *h, DWORD bg)
{
    IStream *stm = stream_from_memory(data, size);
    if (!stm) return NULL;
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
    stm->lpVtbl->Release(stm);
    return hb;
}

static void *res_blob(WORD id, DWORD *size)
{
    HRSRC hr = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!hr) return NULL;
    *size = SizeofResource(NULL, hr);
    void *data = LockResource(LoadResource(NULL, hr));
    return (*size && data) ? data : NULL;
}

HBITMAP load_image_res(WORD id, int *w, int *h, DWORD bg)
{
    DWORD sz; void *data = res_blob(id, &sz);
    return data ? bitmap_from_memory(data, sz, w, h, bg) : NULL;
}

/* decode an image file from disk (cover art, sprite-sheet override) */
HBITMAP bitmap_from_file(const wchar_t *path, int *w, int *h, DWORD bg)
{
    void *bmp = NULL;
    HBITMAP hb = NULL;
    if (GdipCreateBitmapFromFile(path, &bmp) == 0 && bmp) {
        UINT uw = 0, uh = 0;
        GdipGetImageWidth(bmp, &uw);
        GdipGetImageHeight(bmp, &uh);
        GdipCreateHBITMAPFromBitmap(bmp, &hb, bg);
        GdipDisposeImage(bmp);
        if (hb) { *w = (int)uw; *h = (int)uh; }
    }
    return hb;
}

/* embedded PNG -> HICON (tray icon); the shell scales it as needed */
HICON icon_from_res_png(WORD id)
{
    DWORD sz; void *data = res_blob(id, &sz);
    if (!data) return NULL;
    IStream *stm = stream_from_memory(data, sz);
    if (!stm) return NULL;
    void *bmp = NULL;
    HICON ico = NULL;
    if (GdipCreateBitmapFromStream(stm, &bmp) == 0 && bmp) {
        GdipCreateHICONFromBitmap(bmp, &ico);
        GdipDisposeImage(bmp);
    }
    stm->lpVtbl->Release(stm);
    return ico;
}

/* Decode an embedded animated GIF into one 32-bit HBITMAP per frame (alpha
 * kept, so the frames AlphaBlend cleanly over the board gradient). */
int load_gif_frames_res(WORD id, HBITMAP *out, int cap, int *w, int *h)
{
    *w = *h = 0;
    DWORD sz; void *data = res_blob(id, &sz);
    if (!data) return 0;
    IStream *stm = stream_from_memory(data, sz);
    if (!stm) return 0;

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
    stm->lpVtbl->Release(stm);
    return n;
}

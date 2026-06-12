/*
 * peko_cfg.c - settings persistence in PekoBoard.ini next to the exe.
 * Plain GetPrivateProfileIntW / WritePrivateProfileStringW; loaded once at
 * startup, saved whenever something changes (cheap, tiny file).
 */
#include "peko.h"

Config g_cfg;

static wchar_t g_iniPath[MAX_PATH];

static const wchar_t *ini_path(void)
{
    if (!g_iniPath[0]) {
        DWORD n = GetModuleFileNameW(NULL, g_iniPath, MAX_PATH);
        wchar_t *s = (n && n < MAX_PATH) ? wcsrchr(g_iniPath, L'\\') : NULL;
        if (s) wcscpy(s + 1, L"PekoBoard.ini");
        else   wcscpy(g_iniPath, L"PekoBoard.ini");
    }
    return g_iniPath;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void cfg_load(void)
{
    const wchar_t *p = ini_path();
    g_cfg.lang         = clampi(GetPrivateProfileIntW(L"ui", L"lang", LANG_JA, p), 0, LANG_COUNT - 1);
    g_cfg.timeMode     = clampi(GetPrivateProfileIntW(L"ui", L"timeMode", 0, p), 0, 1);
    g_cfg.petalsOn     = clampi(GetPrivateProfileIntW(L"ui", L"petals", 1, p), 0, 1);
    g_cfg.trayMin      = clampi(GetPrivateProfileIntW(L"ui", L"trayMin", 0, p), 0, 1);
    g_cfg.notifyOn     = clampi(GetPrivateProfileIntW(L"ui", L"notify", 1, p), 0, 1);
    g_cfg.autoCheckMin = GetPrivateProfileIntW(L"ui", L"autoCheckMin", 5, p);
    if (g_cfg.autoCheckMin != 0 && g_cfg.autoCheckMin != 5 && g_cfg.autoCheckMin != 10)
        g_cfg.autoCheckMin = 5;
    g_cfg.musicVol     = clampi(GetPrivateProfileIntW(L"audio", L"musicVol", 70, p), 0, 100);
    g_cfg.voiceVol     = clampi(GetPrivateProfileIntW(L"audio", L"voiceVol", 80, p), 0, 100);
    g_cfg.loopMode     = clampi(GetPrivateProfileIntW(L"audio", L"loop", 1, p), 0, 2);
    g_cfg.shuffle      = clampi(GetPrivateProfileIntW(L"audio", L"shuffle", 0, p), 0, 1);
    g_cfg.affection    = clampi(GetPrivateProfileIntW(L"chara", L"affection", 0, p), 0, 1000000);
    g_cfg.fortuneIdx   = GetPrivateProfileIntW(L"chara", L"fortuneIdx", -1, p);
    GetPrivateProfileStringW(L"chara", L"lastGreetDay", L"", g_cfg.lastGreetDay, 16, p);
    GetPrivateProfileStringW(L"chara", L"fortuneDay",   L"", g_cfg.fortuneDay, 16, p);
}

static void wri(const wchar_t *sec, const wchar_t *key, int v)
{
    wchar_t b[24];
    _snwprintf(b, 24, L"%d", v);
    WritePrivateProfileStringW(sec, key, b, ini_path());
}

void cfg_save(void)
{
    wri(L"ui", L"lang", g_cfg.lang);
    wri(L"ui", L"timeMode", g_cfg.timeMode);
    wri(L"ui", L"petals", g_cfg.petalsOn);
    wri(L"ui", L"trayMin", g_cfg.trayMin);
    wri(L"ui", L"notify", g_cfg.notifyOn);
    wri(L"ui", L"autoCheckMin", g_cfg.autoCheckMin);
    wri(L"audio", L"musicVol", g_cfg.musicVol);
    wri(L"audio", L"voiceVol", g_cfg.voiceVol);
    wri(L"audio", L"loop", g_cfg.loopMode);
    wri(L"audio", L"shuffle", g_cfg.shuffle);
    wri(L"chara", L"affection", g_cfg.affection);
    wri(L"chara", L"fortuneIdx", g_cfg.fortuneIdx);
    WritePrivateProfileStringW(L"chara", L"lastGreetDay", g_cfg.lastGreetDay, ini_path());
    WritePrivateProfileStringW(L"chara", L"fortuneDay",   g_cfg.fortuneDay,   ini_path());
}

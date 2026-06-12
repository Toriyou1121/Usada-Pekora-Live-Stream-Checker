/*
 * peko_parser.c - streams-page parser (builds standalone with -DSELFTEST)
 *
 * No JSON library: we find each field by literal anchor substrings in the
 * page's embedded ytInitialData, then read the value that follows. Each
 * "lockupViewModel" block is classified three ways:
 *   live      -> the LIVE thumbnail badge is present
 *   upcoming  -> the "公開予定" marker is present
 *   VOD       -> neither (a finished stream; the page lists them newest-first)
 */
#include "peko.h"

#define A_REGION   "\"lockupViewModel\":{"      /* start of a stream entry   */
#define A_VIDID    "\"videoId\":\""             /* the 11-char video id      */
#define A_TITLE    "\"lockupMetadataViewModel\":{\"title\":{\"content\":\""
#define A_CONTENT  "\"content\":\""             /* a metadata text run       */
#define A_LIVE     "THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE"  /* present => live  */

/* "公開予定" (upcoming marker) as raw UTF-8 bytes, charset-independent. */
static const char TOK_SCHEDULED[] =
    { (char)0xE5,(char)0x85,(char)0xAC, (char)0xE9,(char)0x96,(char)0x8B,
      (char)0xE4,(char)0xBA,(char)0x88, (char)0xE5,(char)0xAE,(char)0x9A, 0x00 };

/* ---------- text helpers: UTF-8 encode, JSON-string decode, search ---------- */

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

/* ---------- the parser itself ---------- */

static int have_id(const StreamItem *arr, int n, const char *id)
{
    for (int i = 0; i < n; ++i)
        if (strcmp(arr[i].id, id) == 0) return 1;
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

void parse_streams(const char *html, ParseResult *res)
{
    res->itemCount = 0;
    res->vodCount  = 0;
    const char *end = html + strlen(html);
    const char *r = strstr(html, A_REGION);
    while (r) {
        const char *next = strstr(r + 1, A_REGION);
        const char *e    = next ? next : end;

        int isLive = (find_in(r, e, A_LIVE) != NULL);
        const char *sched = find_in(r, e, TOK_SCHEDULED);
        int isVod = (!isLive && !sched);

        if ((isVod && res->vodCount < MAX_VODS) ||
            (!isVod && res->itemCount < MAX_ITEMS)) {
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

            if (isLive)      { it.sortKey = -1; it.when[0] = L'\0'; }
            else if (sched) {
                const char *cp = rfind_before(r, sched, A_CONTENT);
                char su8[256] = "";
                if (cp) json_str(cp + strlen(A_CONTENT), su8, sizeof su8);
                format_when(su8, &it);
            }

            if (it.id[0])
                _snwprintf(it.url, 96, L"https://www.youtube.com/watch?v=%hs", it.id);
            else
                wcscpy(it.url, L"https://www.youtube.com/@usadapekora");

            if (isVod) {
                /* keep page order (newest first); require an id (real video) */
                if (it.id[0] && !have_id(res->vods, res->vodCount, it.id) &&
                                !have_id(res->items, res->itemCount, it.id))
                    res->vods[res->vodCount++] = it;
            } else {
                if (!it.id[0] || !have_id(res->items, res->itemCount, it.id))
                    res->items[res->itemCount++] = it;
            }
        }
        r = next;
    }
    /* live first, then upcoming by time ascending (VODs keep page order) */
    for (int i = 0; i < res->itemCount - 1; ++i)
        for (int j = i + 1; j < res->itemCount; ++j)
            if (res->items[j].sortKey < res->items[i].sortKey) {
                StreamItem t = res->items[i];
                res->items[i] = res->items[j];
                res->items[j] = t;
            }
}

/* ===== SELFTEST: console main() that runs the parser on a saved HTML file ===== */
#ifdef SELFTEST
int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "_streams_sample.html";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1); fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);
    static ParseResult res;
    parse_streams(buf, &res);
    printf("parsed %d live/upcoming, %d VOD(s)\n", res.itemCount, res.vodCount);
    for (int i = 0; i < res.itemCount; ++i)
        printf("[%d] %s id=%s key=%lld\n", i,
               res.items[i].isLive ? "LIVE" : "UP", res.items[i].id, res.items[i].sortKey);
    for (int i = 0; i < res.vodCount; ++i)
        printf("[V%d] VOD id=%s\n", i, res.vods[i].id);
    free(buf);
    return 0;
}
#endif

#include "config.h"
#include <io.h>

/* ---------------------------------------------------------------- defaults */

void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->version       = RG_CONFIG_VERSION;
    c->x             = -1;
    c->y             = -1;
    c->opacity       = 0.78f;
    c->material      = 0.55f;
    c->blur_radius   = 24.0f;
    wcscpy_s(c->size, RG_COUNT(c->size), L"normal");
    wcscpy_s(c->blur_mode, RG_COUNT(c->blur_mode), L"screen");
    wcscpy_s(c->theme, RG_COUNT(c->theme), L"system");
    c->auto_dpi      = 0;
    c->topmost       = 0;
    wcscpy_s(c->language, RG_COUNT(c->language), L"auto");
    c->autostart     = 0;
    c->confirm_empty = 1;
    c->width         = 208;
    c->height        = 100;
    c->last_count    = 0;
    c->last_size     = 0;
}

/* --------------------------------------------------------------- clamping */

static int in_set(const wchar_t *v, const wchar_t *const *set, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (wcscmp(v, set[i]) == 0)
            return 1;
    return 0;
}

static float clamp_f(float v, float dflt, float lo, float hi)
{
    if (v != v) /* NaN */
        return dflt;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int clamp_i(long long v, int dflt, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    (void)dflt;
    return (int)v;
}

void config_sanitise(Config *c)
{
    static const wchar_t *themes[]  = { L"system", L"light", L"dark" };
    static const wchar_t *blurs[]   = { L"screen", L"wallpaper" };
    static const wchar_t *sizes[]   = { L"small", L"normal", L"large" };
    static const wchar_t *langs[]   = { L"auto", L"zh", L"en" };

    if (!in_set(c->theme, themes, RG_COUNT(themes)))
        wcscpy_s(c->theme, RG_COUNT(c->theme), L"system");

    /* "screen" blurs what is really behind the widget; "wallpaper" blurs the
     * wallpaper file.  Older configs said "native", which now means screen. */
    if (wcscmp(c->blur_mode, L"native") == 0)
        wcscpy_s(c->blur_mode, RG_COUNT(c->blur_mode), L"screen");
    if (!in_set(c->blur_mode, blurs, RG_COUNT(blurs)))
        wcscpy_s(c->blur_mode, RG_COUNT(c->blur_mode), L"screen");

    if (!in_set(c->size, sizes, RG_COUNT(sizes)))
        wcscpy_s(c->size, RG_COUNT(c->size), L"normal");

    if (!in_set(c->language, langs, RG_COUNT(langs)))
        wcscpy_s(c->language, RG_COUNT(c->language), L"auto");

    c->opacity     = clamp_f(c->opacity, 0.78f, 0.2f, 1.0f);
    c->material    = clamp_f(c->material, 0.55f, 0.0f, 1.0f);
    c->blur_radius = clamp_f(c->blur_radius, 24.0f, 0.0f, 60.0f);

    c->x = clamp_i(c->x, -1, -32768, 32768);
    c->y = clamp_i(c->y, -1, -32768, 32768);

    c->width  = clamp_i(c->width, 208, 64, 4096);
    c->height = clamp_i(c->height, 100, 48, 4096);

    if (c->last_count < 0) c->last_count = 0;
    if (c->last_count > 10000000LL) c->last_count = 10000000LL;
    if (c->last_size < 0) c->last_size = 0;

    c->auto_dpi      = c->auto_dpi ? 1 : 0;
    c->topmost       = c->topmost ? 1 : 0;
    c->autostart     = c->autostart ? 1 : 0;
    c->confirm_empty = c->confirm_empty ? 1 : 0;
    c->version       = RG_CONFIG_VERSION;
}

/* ------------------------------------------------------------------- paths */

void config_dir(wchar_t *out, size_t cap)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        wchar_t *home = _wgetenv(L"USERPROFILE");
        if (home)
            swprintf_s(base, MAX_PATH, L"%ls", home);
        else
            wcscpy_s(base, MAX_PATH, L".");
    }
    swprintf_s(out, cap, L"%ls\\%ls", base, RG_APP_NAME);
    CreateDirectoryW(out, NULL);
}

void config_path(wchar_t *out, size_t cap)
{
    wchar_t dir[MAX_PATH];
    config_dir(dir, MAX_PATH);
    swprintf_s(out, cap, L"%ls\\config.json", dir);
}

/* --------------------------------------------------------- tiny JSON scan */

static const wchar_t *skip_ws(const wchar_t *p)
{
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')
        p++;
    return p;
}

/* Returns a pointer just past the value of "key", or NULL when absent. */
static const wchar_t *json_value(const wchar_t *js, const wchar_t *key)
{
    wchar_t pat[64];
    const wchar_t *p;

    swprintf_s(pat, RG_COUNT(pat), L"\"%ls\"", key);
    p = js;
    for (;;)
    {
        const wchar_t *hit = wcsstr(p, pat);
        if (!hit)
            return NULL;
        p = skip_ws(hit + wcslen(pat));
        if (*p != L':')
        {
            p = hit + wcslen(pat);
            continue;
        }
        p = skip_ws(p + 1);
        return p;
    }
}

static BOOL json_int(const wchar_t *js, const wchar_t *key, long long *out)
{
    const wchar_t *p = json_value(js, key);
    wchar_t *end = NULL;
    long long v;
    if (!p)
        return FALSE;
    v = wcstoll(p, &end, 10);
    if (end == p)
        return FALSE;
    *out = v;
    return TRUE;
}

static BOOL json_double(const wchar_t *js, const wchar_t *key, double *out)
{
    const wchar_t *p = json_value(js, key);
    wchar_t *end = NULL;
    double v;
    if (!p)
        return FALSE;
    v = wcstod(p, &end);
    if (end == p)
        return FALSE;
    *out = v;
    return TRUE;
}

static BOOL json_bool(const wchar_t *js, const wchar_t *key, int *out)
{
    const wchar_t *p = json_value(js, key);
    if (!p)
        return FALSE;
    if (wcsncmp(p, L"true", 4) == 0)  { *out = 1; return TRUE; }
    if (wcsncmp(p, L"false", 5) == 0) { *out = 0; return TRUE; }
    if (wcsncmp(p, L"1", 1) == 0)     { *out = 1; return TRUE; }
    if (wcsncmp(p, L"0", 1) == 0)     { *out = 0; return TRUE; }
    return FALSE;
}

static BOOL json_str(const wchar_t *js, const wchar_t *key, wchar_t *out, size_t cap)
{
    const wchar_t *p = json_value(js, key);
    size_t n = 0;
    if (!p || *p != L'"')
        return FALSE;
    p++;
    while (*p && *p != L'"' && n + 1 < cap)
    {
        if (*p == L'\\' && p[1])
            p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return TRUE;
}

/* ------------------------------------------------------------------- I/O */

void config_load(Config *c)
{
    wchar_t path[MAX_PATH];
    wchar_t *text = NULL;
    FILE *f;
    long sz;
    size_t got;

    config_defaults(c);
    config_path(path, MAX_PATH);

    f = _wfopen(path, L"rb");
    if (!f)
        return;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    sz = ftell(f);
    if (sz <= 0 || sz > (1 << 20)) { fclose(f); return; }
    rewind(f);

    text = (wchar_t *)calloc((size_t)sz + 1, sizeof(wchar_t));
    if (!text) { fclose(f); return; }

    {
        char *raw = (char *)calloc((size_t)sz + 1, 1);
        if (!raw) { free(text); fclose(f); return; }
        got = fread(raw, 1, (size_t)sz, f);
        raw[got] = 0;
        fclose(f);

        /* Accept what any writer before us may have produced: plain UTF-8,
         * UTF-8 with BOM, or UTF-16 with a BOM. */
        if (got >= 2 && (BYTE)raw[0] == 0xFF && (BYTE)raw[1] == 0xFE)
        {
            size_t n = (got - 2) / 2, i;
            for (i = 0; i < n; i++)
                text[i] = (wchar_t)((BYTE)raw[2 + i * 2] |
                                    ((wchar_t)(BYTE)raw[3 + i * 2] << 8));
            text[n] = 0;
        }
        else
        {
            char *p = raw;
            if (got >= 3 && (BYTE)raw[0] == 0xEF && (BYTE)raw[1] == 0xBB &&
                (BYTE)raw[2] == 0xBF)
                p += 3;
            if (MultiByteToWideChar(CP_UTF8, 0, p, -1, text, (int)(sz + 1)) <= 0)
                text[0] = 0;
        }
        free(raw);
    }

    if (text[0] == 0)
    {
        free(text);
        return;
    }

    {
        long long iv;
        double dv;
        int bv;

        if (json_int(text, L"version", &iv)) c->version = (int)iv;
        if (json_int(text, L"x", &iv)) c->x = (int)iv;
        if (json_int(text, L"y", &iv)) c->y = (int)iv;
        if (json_double(text, L"opacity", &dv))     c->opacity = (float)dv;
        if (json_double(text, L"material", &dv))    c->material = (float)dv;
        if (json_double(text, L"blur_radius", &dv)) c->blur_radius = (float)dv;
        if (json_int(text, L"width", &iv))  c->width = (int)iv;
        if (json_int(text, L"height", &iv)) c->height = (int)iv;
        if (json_int(text, L"last_count", &iv)) c->last_count = iv;
        if (json_int(text, L"last_size", &iv))  c->last_size = iv;

        json_str(text, L"size", c->size, RG_COUNT(c->size));
        json_str(text, L"blur_mode", c->blur_mode, RG_COUNT(c->blur_mode));
        json_str(text, L"theme", c->theme, RG_COUNT(c->theme));
        json_str(text, L"language", c->language, RG_COUNT(c->language));

        if (json_bool(text, L"auto_dpi", &bv))      c->auto_dpi = bv;
        if (json_bool(text, L"topmost", &bv))       c->topmost = bv;
        if (json_bool(text, L"autostart", &bv))     c->autostart = bv;
        if (json_bool(text, L"confirm_empty", &bv)) c->confirm_empty = bv;

        /* v3 replaced the "wallpaper" fake with a real capture of whatever is
         * behind the widget.  Older files are moved over so nobody keeps the
         * wallpaper backdrop by accident. */
        if (c->version < 3)
            wcscpy_s(c->blur_mode, RG_COUNT(c->blur_mode), L"screen");
        c->version = RG_CONFIG_VERSION;
    }

    free(text);
    config_sanitise(c);
}

BOOL config_save(const Config *c)
{
    wchar_t path[MAX_PATH];
    wchar_t tmp[MAX_PATH];
    wchar_t *wide = NULL;
    char *utf8 = NULL;
    int need, have;
    FILE *f;
    BOOL ok = FALSE;

    config_path(path, MAX_PATH);
    swprintf_s(tmp, MAX_PATH, L"%ls.cfg-%lu.tmp", path, GetCurrentProcessId());

    wide = (wchar_t *)calloc(4096, sizeof(wchar_t));
    if (!wide)
        return FALSE;

    swprintf_s(wide, 4096,
        L"{\n"
        L"  \"version\": %d,\n"
        L"  \"x\": %d,\n"
        L"  \"y\": %d,\n"
        L"  \"opacity\": %.3f,\n"
        L"  \"material\": %.3f,\n"
        L"  \"blur_radius\": %.1f,\n"
        L"  \"size\": \"%ls\",\n"
        L"  \"blur_mode\": \"%ls\",\n"
        L"  \"theme\": \"%ls\",\n"
        L"  \"auto_dpi\": %ls,\n"
        L"  \"topmost\": %ls,\n"
        L"  \"language\": \"%ls\",\n"
        L"  \"autostart\": %ls,\n"
        L"  \"confirm_empty\": %ls,\n"
        L"  \"width\": %d,\n"
        L"  \"height\": %d,\n"
        L"  \"last_count\": %lld,\n"
        L"  \"last_size\": %lld\n"
        L"}\n",
        RG_CONFIG_VERSION,
        c->x, c->y,
        c->opacity, c->material, c->blur_radius,
        c->size, c->blur_mode, c->theme,
        c->auto_dpi ? L"true" : L"false",
        c->topmost ? L"true" : L"false",
        c->language,
        c->autostart ? L"true" : L"false",
        c->confirm_empty ? L"true" : L"false",
        c->width, c->height,
        c->last_count, c->last_size);

    /* The original widget stores UTF-8; keep the file interchangeable. */
    need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (need <= 0)
    {
        free(wide);
        return FALSE;
    }

    utf8 = (char *)malloc((size_t)need);
    if (!utf8)
    {
        free(wide);
        return FALSE;
    }

    have = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, need, NULL, NULL);
    free(wide);
    if (have <= 1)
    {
        free(utf8);
        return FALSE;
    }

    f = _wfopen(tmp, L"wb");
    if (!f)
    {
        free(utf8);
        return FALSE;
    }

    ok = (fwrite(utf8, 1, (size_t)(have - 1), f) == (size_t)(have - 1));
    free(utf8);

    fflush(f);
    _commit(_fileno(f));
    fclose(f);

    if (ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return TRUE;

    DeleteFileW(tmp);
    return FALSE;
}

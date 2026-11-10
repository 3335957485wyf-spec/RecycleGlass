#include "shell.h"
#include "i18n.h"

/* CLSID of the Recycle Bin shell folder. */
#define RG_RECYCLE_CLSID L"shell:::{645FF040-5081-101B-9F08-00AA002F954E}"

void shell_query(long long *count, long long *size)
{
    SHQUERYRBINFO info;
    HRESULT hr;

    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);

    hr = SHQueryRecycleBinW(NULL, &info);
    if (FAILED(hr))
    {
        *count = 0;
        *size = 0;
        return;
    }
    *count = (long long)info.i64NumItems;
    *size  = (long long)info.i64Size;
}

BOOL shell_empty(BOOL confirm)
{
    DWORD flags = SHERB_NOPROGRESSUI | SHERB_NOSOUND;
    HRESULT hr;
    long long after = 0, size = 0;

    if (!confirm)
        flags |= SHERB_NOCONFIRMATION;

    hr = SHEmptyRecycleBinW(NULL, NULL, flags);
    (void)hr;

    shell_query(&after, &size);
    return after == 0;
}

void shell_open_bin(void)
{
    ShellExecuteW(NULL, L"open", RG_RECYCLE_CLSID, NULL, NULL, SW_SHOWNORMAL);
}

BOOL shell_recycle_paths(wchar_t **paths, int n, wchar_t *message, size_t msgcap)
{
    wchar_t **existing = NULL;
    int n_existing = 0, i;
    size_t total = 0;
    wchar_t *joined = NULL, *p;
    SHFILEOPSTRUCTW op;
    int rc;
    int gone = 0;

    if (message && msgcap)
        message[0] = 0;

    existing = (wchar_t **)calloc((size_t)(n > 0 ? n : 1), sizeof(wchar_t *));
    if (!existing)
        return FALSE;

    for (i = 0; i < n; i++)
    {
        DWORD attr = GetFileAttributesW(paths[i]);
        if (attr != INVALID_FILE_ATTRIBUTES)
        {
            existing[n_existing++] = paths[i];
            total += wcslen(paths[i]) + 1;
        }
    }

    if (n_existing == 0)
    {
        free(existing);
        if (message)
            swprintf_s(message, msgcap, L"%ls", T(S_TOAST_NOTHING));
        return FALSE;
    }

    joined = (wchar_t *)calloc(total + 2, sizeof(wchar_t));
    if (!joined)
    {
        free(existing);
        return FALSE;
    }

    p = joined;
    for (i = 0; i < n_existing; i++)
    {
        size_t len = wcslen(existing[i]);
        memcpy(p, existing[i], len * sizeof(wchar_t));
        p += len;
        *p++ = L'\0'; /* double NUL terminates the list */
    }
    *p = L'\0';

    memset(&op, 0, sizeof(op));
    op.hwnd   = NULL;
    op.wFunc  = FO_DELETE;
    op.pFrom  = joined;
    op.pTo    = NULL;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    op.fAnyOperationsAborted = FALSE;

    rc = SHFileOperationW(&op);

    if (rc == 0 && !op.fAnyOperationsAborted)
    {
        if (message)
            swprintf_s(message, msgcap, T(S_TOAST_RECYCLED), (long long)n_existing);
        free(joined);
        free(existing);
        return TRUE;
    }

    /* Fall back to "did the files disappear?" — SHFileOperation reports
     * failures for paths it silently skipped. */
    for (i = 0; i < n_existing; i++)
        if (GetFileAttributesW(existing[i]) == INVALID_FILE_ATTRIBUTES)
            gone++;

    if (gone == n_existing)
    {
        if (message)
            swprintf_s(message, msgcap, T(S_TOAST_RECYCLED), (long long)n_existing);
        free(joined);
        free(existing);
        return TRUE;
    }

    if (message)
    {
        long long left = (long long)(n_existing - gone);
        if (gone > 0)
            swprintf_s(message, msgcap, T(S_TOAST_PARTIAL), left);
        else
            swprintf_s(message, msgcap, L"%ls (code %d)", T(S_TOAST_FAILED), rc);
    }

    free(joined);
    free(existing);
    return FALSE;
}

void shell_format_size(long long n, wchar_t *out, size_t cap)
{
    static const wchar_t *units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v;
    int i;

    if (n <= 0)
    {
        swprintf_s(out, cap, L"0 B");
        return;
    }

    v = (double)n;
    for (i = 0; i < RG_COUNT(units); i++)
    {
        if (v < 1024.0 || i == RG_COUNT(units) - 1)
        {
            if (wcscmp(units[i], L"B") == 0)
                swprintf_s(out, cap, L"%lld %ls", (long long)v, units[i]);
            else
                swprintf_s(out, cap, L"%.2f %ls", v, units[i]);
            return;
        }
        v /= 1024.0;
    }
    swprintf_s(out, cap, L"%.2f TB", v);
}

void shell_format_count(long long n, wchar_t *out, size_t cap)
{
    if (n == 0)
    {
        swprintf_s(out, cap, L"%ls", T(S_SUB_EMPTY));
        return;
    }
    if (i18n_is_cjk())
        swprintf_s(out, cap, L"%lld 项", n);
    else
        swprintf_s(out, cap, L"%lld item%ls", n, n == 1 ? L"" : L"s");
}

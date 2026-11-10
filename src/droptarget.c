#include "droptarget.h"
#include <ole2.h>

typedef struct {
    IDropTargetVtbl *lpVtbl;
    LONG             refs;
    HWND             hwnd;
    BOOL             registered;
} RgDropTarget;

static RgDropTarget  g_dt;
static RgDropEnterFn g_on_enter;
static RgDropLeaveFn g_on_leave;
static RgDropFilesFn g_on_drop;

#define SELF(p) ((RgDropTarget *)(p))

/* ------------------------------------------------------------- COM plumbing */

static HRESULT STDMETHODCALLTYPE dt_query_interface(IDropTarget *self, REFIID riid,
                                                    void **ppv)
{
    if (!ppv)
        return E_POINTER;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget))
    {
        *ppv = self;
        InterlockedIncrement(&SELF(self)->refs);
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dt_add_ref(IDropTarget *self)
{
    return (ULONG)InterlockedIncrement(&SELF(self)->refs);
}

static ULONG STDMETHODCALLTYPE dt_release(IDropTarget *self)
{
    LONG n = InterlockedDecrement(&SELF(self)->refs);
    return (ULONG)n;
}

/* --------------------------------------------------------------- data pull */

static void emit_paths(IDataObject *obj)
{
    FORMATETC fmt;
    STGMEDIUM stg;

    if (!obj || !g_on_drop)
        return;

    memset(&fmt, 0, sizeof(fmt));
    fmt.cfFormat = CF_HDROP;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;

    memset(&stg, 0, sizeof(stg));
    if (FAILED(obj->lpVtbl->GetData(obj, &fmt, &stg)))
        return;

    if (stg.tymed == TYMED_HGLOBAL && stg.hGlobal)
    {
        HDROP drop = (HDROP)GlobalLock(stg.hGlobal);
        if (drop)
        {
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
            if (n > 0)
            {
                wchar_t **paths = (wchar_t **)calloc(n, sizeof(wchar_t *));
                if (paths)
                {
                    UINT i;
                    for (i = 0; i < n; i++)
                    {
                        UINT len = DragQueryFileW(drop, i, NULL, 0) + 1;
                        paths[i] = (wchar_t *)calloc(len, sizeof(wchar_t));
                        if (paths[i])
                            DragQueryFileW(drop, i, paths[i], len);
                    }
                    g_on_drop(paths, (int)n);
                    for (i = 0; i < n; i++)
                        free(paths[i]);
                    free(paths);
                }
            }
            GlobalUnlock(stg.hGlobal);
        }
    }

    ReleaseStgMedium(&stg);
}

/* --------------------------------------------------------------- IDropTarget */

static HRESULT STDMETHODCALLTYPE dt_drag_enter(IDropTarget *self, IDataObject *obj,
                                               DWORD keys, POINTL pt, DWORD *effect)
{
    (void)self;
    (void)obj;
    (void)keys;
    (void)pt;

    if (g_on_enter)
        g_on_enter();
    if (effect)
        *effect = DROPEFFECT_COPY;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_drag_over(IDropTarget *self, DWORD keys,
                                              POINTL pt, DWORD *effect)
{
    (void)self;
    (void)keys;
    (void)pt;

    if (effect)
        *effect = DROPEFFECT_COPY;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_drag_leave(IDropTarget *self)
{
    (void)self;
    if (g_on_leave)
        g_on_leave();
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_drop(IDropTarget *self, IDataObject *obj,
                                         DWORD keys, POINTL pt, DWORD *effect)
{
    (void)self;
    (void)keys;
    (void)pt;

    if (g_on_leave)
        g_on_leave();

    emit_paths(obj);

    if (effect)
        *effect = DROPEFFECT_COPY;
    return S_OK;
}

static IDropTargetVtbl g_vtbl = {
    dt_query_interface,
    dt_add_ref,
    dt_release,
    dt_drag_enter,
    dt_drag_over,
    dt_drag_leave,
    dt_drop
};

/* ------------------------------------------------------------------ public */

BOOL droptarget_register(HWND hwnd, RgDropEnterFn on_enter,
                         RgDropLeaveFn on_leave, RgDropFilesFn on_drop)
{
    HRESULT hr;

    if (g_dt.registered)
        return TRUE;

    g_on_enter = on_enter;
    g_on_leave = on_leave;
    g_on_drop = on_drop;

    g_dt.lpVtbl = &g_vtbl;
    g_dt.refs = 1;
    g_dt.hwnd = hwnd;
    g_dt.registered = FALSE;

    hr = RegisterDragDrop(hwnd, (IDropTarget *)&g_dt);
    if (FAILED(hr))
        return FALSE;

    g_dt.registered = TRUE;
    return TRUE;
}

void droptarget_revoke(void)
{
    if (g_dt.registered)
    {
        RevokeDragDrop(g_dt.hwnd);
        g_dt.registered = FALSE;
    }
}

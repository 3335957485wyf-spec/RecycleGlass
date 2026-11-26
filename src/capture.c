/* Background screen grab — the widget's own capture path.
 *
 * One worker thread keeps a screen-anchored BGRA snapshot of the desktop in
 * a pair of swap-locked DIB buffers, refreshed with BitBlt as fast as it can
 * while the requested region is moving and at a low duty cycle when it is
 * still.  cap_grab() copies the wanted rect out of the newest snapshot; a
 * miss (first calls, or the card outran the covered region) returns FALSE
 * and the caller falls back to its own synchronous grab.
 *
 * Why not DXGI Desktop Duplication: on an HDR / auto-color-managed desktop
 * the duplicated frame is display-referred (linear gain ~3.85x over the GDI
 * paint space; 8-bit frames clip above source ~140/255, and calibration of
 * the gain proved fragile).  BitBlt returns exactly the source space the
 * ULW paint path consumes — pixel-perfect by construction (verified:
 * backdrop-vs-true = 1.000) — and moving it off the UI thread removes its
 * ~3.5 ms stall just the same.
 *
 * Why a slightly stale screen-anchored snapshot is safe while dragging: the
 * desktop content at a screen coordinate does not move when the card does.
 * A snapshot a few milliseconds old, cropped at the card's *current* rect,
 * therefore shows the right pixels; the margin only has to cover the card's
 * travel between refreshes.
 *
 * Use a grab only when the widget cannot appear in it (WDA_EXCLUDEFROMCAPTURE).
 * When the card is capture-visible the caller keeps its synchronous BitBlt
 * so the grab always matches the self layer it un-composites. */
#include "capture.h"

#define CAP_MARGIN 96
#define CAP_MARGIN_BURST 24
#define CAP_MARGIN_DRAG 160

/* Live background frame interval, in microseconds.  60 fps: 10000. */
#define LIVE_FRAME_US 10000

typedef struct {
    CRITICAL_SECTION lk;    /* guards bits: worker rewrites the non-front
                             * slot while a slow reader may still be on it */
    HDC        mem;
    HBITMAP    dib;
    void      *bits;        /* the DIB's bits — this IS the pixel buffer */
    int        w, h;        /* DIB buffer size (>= the grabbed rect) */
    int        x, y;        /* screen rect covered by the current contents */
    int        cw, ch;      /* grabbed rect size inside the buffer */
    int        valid;
} CapSlot;

static CRITICAL_SECTION g_lock;
static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
static HANDLE           g_thread;
static HANDLE           g_wake;
static volatile LONG    g_running;
static volatile LONG    g_burst;
static volatile LONG    g_frames;

static ULONGLONG cap_now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;

    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (ULONGLONG)(c.QuadPart * 1000000ULL / (ULONGLONG)f.QuadPart);
}
static volatile LONG    g_demand;   /* the live path is asking for frames now */    /* live background: grab back to back */
/* Nobody is reading snapshots (the card is capture-visible and paints from a
 * frozen frame), so the duplication threads park instead of refreshing a slot
 * no one will look at. */
static volatile LONG    g_paused;
static int              g_want_x, g_want_y, g_want_w, g_want_h;
static int              g_want_moved;
static CapSlot          g_slot[2];
static int              g_front;    /* newest complete snapshot */

static BOOL CALLBACK cap_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_slot[0].lk);
    InitializeCriticalSection(&g_slot[1].lk);
    return TRUE;
}

/* Recreates the slot's DIB when the wanted grab size grows.  Only the
 * worker thread touches the GDI objects. */
static BOOL slot_prepare(CapSlot *s, int w, int h)
{
    BITMAPINFO bi;

    if (s->dib && s->w >= w && s->h >= h)
        return TRUE;   /* buffer keeps its size; coverage tracked per grab */
    if (s->dib)
    {
        DeleteObject(s->dib);
        s->dib = NULL;
        s->bits = NULL;
    }
    if (!s->mem)
    {
        HDC scr = GetDC(NULL);

        if (!scr)
            return FALSE;
        s->mem = CreateCompatibleDC(scr);
        ReleaseDC(NULL, scr);
        if (!s->mem)
            return FALSE;
    }
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;      /* top-down: rows in screen order */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s->dib = CreateDIBSection(s->mem, &bi, DIB_RGB_COLORS, &s->bits, NULL, 0);
    if (!s->dib)
    {
        s->bits = NULL;
        return FALSE;
    }
    s->w = w;
    s->h = h;
    return TRUE;
}

static void slot_free(CapSlot *s)
{
    if (s->dib)
    {
        DeleteObject(s->dib);
        s->dib = NULL;
    }
    if (s->mem)
    {
        DeleteDC(s->mem);
        s->mem = NULL;
    }
    s->bits = NULL;
    s->w = s->h = 0;
    s->valid = 0;
}

static BOOL slot_grab(CapSlot *s, int x, int y, int w, int h)
{
    HDC scr;
    HGDIOBJ old;
    BOOL ok = FALSE;

    if (!slot_prepare(s, w, h))
        return FALSE;
    scr = GetDC(NULL);
    if (!scr)
        return FALSE;
    old = SelectObject(s->mem, s->dib);
    if (BitBlt(s->mem, 0, 0, w, h, scr, x, y, SRCCOPY))
    {
        s->x = x;
        s->y = y;
        s->cw = w;
        s->ch = h;
        s->valid = 1;
        ok = TRUE;
    }
    else
    {
        s->valid = 0;
    }
    SelectObject(s->mem, old);
    ReleaseDC(NULL, scr);
    return ok;
}

static DWORD WINAPI cap_thread(void *arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_running, 0, 0))
    {
        int wx, wy, ww, wh, moved;
        int x, y, w, h, back;

        EnterCriticalSection(&g_lock);
        wx = g_want_x; wy = g_want_y;
        ww = g_want_w; wh = g_want_h;
        moved = g_want_moved;
        g_want_moved = 0;
        back = 1 - g_front;
        LeaveCriticalSection(&g_lock);

        if (InterlockedCompareExchange(&g_paused, 0, 0))
        {
            /* Parked: no box is wanted, so do not pay for a compositor
             * readback every 32 ms.  The event wakes us on resume. */
            WaitForSingleObject(g_wake, 250);
            continue;
        }

        if (ww <= 0 || wh <= 0)
        {
            if (WaitForSingleObject(g_wake, 50) != WAIT_OBJECT_0)
                Sleep(50);
            continue;
        }

        /* Cover the requested rect plus the card's travel margin so the
         * next request still lands inside the snapshot.  A parked card does
         * not travel, and while the live background runs the snapshot rate is
         * the frame rate: grab a tight box then (BitBlt cost scales with the
         * area, and a full margin box measured ~5 ms = 120 fps ceiling). */
        /* A drag needs a deep margin: the card outruns the snapshot between
         * two grabs, and a miss costs a 12 ms synchronous BitBlt on the UI
         * thread - far worse than the extra pixels here. */
        int cm = moved ? CAP_MARGIN_DRAG
                       : ((InterlockedCompareExchange(&g_burst, 0, 0))
                              ? CAP_MARGIN_BURST : CAP_MARGIN);

        x = wx - cm;
        y = wy - cm;
        w = ww + cm * 2;
        h = wh + cm * 2;

        EnterCriticalSection(&g_slot[back].lk);
        if (slot_grab(&g_slot[back], x, y, w, h))
        {
            InterlockedIncrement(&g_frames);
            EnterCriticalSection(&g_lock);
            g_front = back;
            LeaveCriticalSection(&g_lock);
        }
        LeaveCriticalSection(&g_slot[back].lk);

        /* While the requested region is moving (a drag) refresh back to
         * back; when it sits still, idle down so a desktop widget does not
         * burn a core — a video behind the card still shows through at
         * ~20 Hz.  The live-background switch asks for 120 fps instead, so it
         * flips g_burst and the idle shrinks to a single short wait. */
        /* Pace the snapshots by wall clock, NOT by waiting on the wake event:
         * every cap_grab() signals that event, so the old "wait" returned
         * immediately and the thread spun at BitBlt speed - about 0.6 of a
         * core during a drag, which is what made dragging cost so much with
         * the live backdrop on.  Nothing needs a fresh snapshot sooner than
         * the target interval: 4 ms while the card is being dragged (a few
         * pixels of travel, far inside the 96 px margin), 3 ms while the live
         * background is bursting, 32 ms otherwise. */
        if (InterlockedCompareExchange(&g_running, 0, 0))
        {
            ULONGLONG target, t0;

            if (moved)
                /* A drag paints its own frame-exact grab on the UI thread; the
                 * snapshot only has to stay ahead of the card, so it does not
                 * need to run at the pointer rate. */
                target = 3000;
            /* The live backdrop now reads this snapshot on every tile, so the
             * snapshot cadence - not the worker's poll - is the frame rate.
             * 12 ms keeps up with 60 fps tiles while leaving the compositor
             * readback room to breathe. */
            else if (InterlockedCompareExchange(&g_demand, 0, 0))
                target = 12000;
            else if (InterlockedCompareExchange(&g_burst, 0, 0))
                /* 3 ms asked for ~330 snapshots/s while the live path can only
                 * publish ~130 - a third of a core completely wasted.  6 ms
                 * still covers 160 fps. */
                /* One BitBlt of the live box costs ~6.5 ms of desktop-
                 * compositor readback no matter how small the box is, so the
                 * snapshot interval IS the frame rate: 10 + 6.5 ms ~= 60 fps
                 * and about 0.4 of a core (2 ms gave 120 fps for 0.8 core). */
                target = InterlockedCompareExchange(&g_demand, 0, 0) ? LIVE_FRAME_US : 32000;
            else
                target = 32000;

            t0 = cap_now_us();
            for (;;)
            {
                ULONGLONG el = cap_now_us() - t0;
                DWORD left;

                if (el >= target)
                    break;
                left = (DWORD)((target - el + 999) / 1000);
                if (left > 4)
                    left = 4;
                if (!InterlockedCompareExchange(&g_running, 0, 0))
                    return 0;
                Sleep(left);
            }
        }
    }
    return 0;
}

/* Live background on/off: refresh the snapshot back to back (card is kept out
 * of capture then, so this is the only thing pricing the backdrop frame rate). */
/* Snapshots taken since start (diagnostics: the live path can never be faster
 * than the snapshot it reads). */
LONG cap_frames(void)
{
    return InterlockedCompareExchange(&g_frames, 0, 0);
}

void cap_set_live(BOOL on)
{
    InterlockedExchange(&g_burst, on ? 1 : 0);
    if (g_wake)
        SetEvent(g_wake);
}

/* The live path asked for a frame on its last iteration (-1: it backed off to
 * the slow cadence, so the snapshot may idle down and the desktop widget costs
 * nothing while nothing moves). */
void cap_set_demand(BOOL on)
{
    InterlockedExchange(&g_demand, on ? 1 : 0);
}

/* Parks (on = TRUE) or resumes the snapshot threads.  The capture-visible body
 * takes its pixels from a frozen frame, so there is nothing to keep fresh. */
void cap_set_paused(BOOL on)
{
    InterlockedExchange(&g_paused, on ? 1 : 0);
    if (g_wake)
        SetEvent(g_wake);
}

BOOL cap_start(void)
{
    InitOnceExecuteOnce(&g_once, cap_once, NULL, NULL);
    if (InterlockedCompareExchange(&g_running, 0, 0))
        return TRUE;

    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_wake)
        return FALSE;          /* nothing started: g_running stays clear */

    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, cap_thread, NULL, 0, NULL);
    if (!g_thread)
    {
        /* A failed CreateThread used to leave g_running set, so every later
         * cap_grab() believed a worker existed and missed forever - the
         * caller's synchronous grab was the only thing left painting, and
         * nothing ever reported why. */
        InterlockedExchange(&g_running, 0);
        CloseHandle(g_wake);
        g_wake = NULL;
        return FALSE;
    }
    return TRUE;
}

void cap_stop(void)
{
    BOOL stopped = TRUE;

    if (!InterlockedCompareExchange(&g_running, 0, 0) && !g_thread)
        return;
    InterlockedExchange(&g_running, 0);
    if (g_wake)
        SetEvent(g_wake);
    if (g_thread)
    {
        stopped = WaitForSingleObject(g_thread, 2000) == WAIT_OBJECT_0;
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_wake)
    {
        CloseHandle(g_wake);
        g_wake = NULL;
    }
    /* The worker polls g_running every few ms, so the wait above always wins
     * in practice.  If it somehow did not, the thread may still be inside
     * slot_grab(): freeing the DIB/DC under it would be a use-after-free, so
     * keep them.  A leak on a path that never happens beats a crash. */
    if (!stopped)
        return;
    slot_free(&g_slot[0]);
    slot_free(&g_slot[1]);
    g_front = 0;
    g_want_w = g_want_h = 0;
}

BOOL cap_grab(int x, int y, int w, int h, BYTE *out)
{
    CapSlot *s;
    int row;

    if (w <= 0 || h <= 0 || !out)
        return FALSE;
    InitOnceExecuteOnce(&g_once, cap_once, NULL, NULL);
    if (!InterlockedCompareExchange(&g_running, 0, 0))
        return FALSE;

    InterlockedExchange(&g_demand, 1);
    EnterCriticalSection(&g_lock);
    if (g_want_x != x || g_want_y != y || g_want_w != w || g_want_h != h)
        g_want_moved = 1;
    g_want_x = x;
    g_want_y = y;
    g_want_w = w;
    g_want_h = h;
    if (g_wake)
        SetEvent(g_wake);
    s = &g_slot[g_front];
    LeaveCriticalSection(&g_lock);

    EnterCriticalSection(&s->lk);
    if (s->valid && s->bits &&
        x >= s->x && y >= s->y &&
        x + w <= s->x + s->cw && y + h <= s->y + s->ch)
    {
        const BYTE *src = (const BYTE *)s->bits +
            ((size_t)(y - s->y) * s->w + (size_t)(x - s->x)) * 4;

        for (row = 0; row < h; row++)
        {
            memcpy(out + (size_t)row * w * 4,
                   src + (size_t)row * s->w * 4, (size_t)w * 4);
        }
        LeaveCriticalSection(&s->lk);
        return TRUE;
    }
    LeaveCriticalSection(&s->lk);
    return FALSE;
}

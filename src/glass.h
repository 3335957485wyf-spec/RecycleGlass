/* Backdrop provider: the wallpaper area behind the card, blurred.
 *
 * The Python original screenshots the desktop with the widget excluded from
 * capture.  This rewrite reads the wallpaper image instead, which is what the
 * default config ("blur_mode": "wallpaper") does anyway and needs no capture
 * trickery: same visual result, no flicker, no display-affinity side effects. */
#ifndef RG_GLASS_H
#define RG_GLASS_H

#include "common.h"

typedef struct {
    int   w, h;
    BYTE *px;   /* BGRA, 4 bytes per pixel, opaque (a = 255) */
} RgBitmap;

/* Fills `out` with a blurred crop of the desktop behind the given screen
 * rectangle.  Returns FALSE when no wallpaper could be produced (the caller
 * then falls back to a flat tint). */
BOOL glass_backdrop(int x, int y, int w, int h, float blur_radius, RgBitmap *out);

/* Captures the desktop behind (x, y, w, h) and blurs it: this is the real
 * "frosted glass over whatever is there" effect.  The caller must have put its
 * own windows out of capture range (WDA_EXCLUDEFROMCAPTURE) first. */
BOOL glass_live(int x, int y, int w, int h, float blur_radius, RgBitmap *out);

/* Monotonic id of the cached live frame; changes whenever a new capture or a
 * re-blur has landed.  Lets a caller skip repaints that would show the same
 * pixels, and lets a cached layer know its backdrop went stale. */
LONG glass_live_gen(void);

/* Forces the next glass_live() to take a fresh capture. */
void glass_live_invalidate(void);

/* Re-blurs the cached capture with a new radius (no screen grab).  Used while
 * the blur slider is being dragged. */
BOOL glass_live_reblur(float blur_radius);

/* Freezes the background capture while the menu owns the cache. */
void glass_live_menu(BOOL open);

/* Starts the background capture thread; `hwnd` gets RG_WM_REFRESH when a new
 * capture lands. */
void glass_live_set_target(HWND hwnd);

/* Tells the capture thread where the card is right now, so it keeps refreshing
 * the blur around it (call once per frame). */
void glass_live_follow(int x, int y, int w, int h, float blur_radius, BOOL hold);

/* Synchronous variant: captures on the calling thread (used for one-off paints
 * such as opening the menu, where a hitch does not matter). */
BOOL glass_live_blocking(int x, int y, int w, int h, float blur_radius, RgBitmap *out);

/* Raw screen pixels of one rectangle, unblurred.  The menu keeps its own copy
 * so that dragging the radius slider is a copy plus one Gaussian instead of a
 * second screen grab - and so the two windows never contend for one cache. */
BOOL glass_capture(int x, int y, int w, int h, RgBitmap *out);

/* Blurs `src` into a freshly allocated `out`. */
BOOL glass_blurred_copy(const RgBitmap *src, float radius, RgBitmap *out);

/* Copies the top-left w x h of `src` into `out`. */
BOOL glass_crop(const RgBitmap *src, int w, int h, RgBitmap *out);

/* TRUE when the desktop could be captured (Win10 2004+ with WDA support). */
BOOL glass_live_available(void);

/* Display scale (1.0 = 96 dpi).  Blur radii are authored in logical pixels,
 * so the engine multiplies them by this to look the same at any scaling. */
void glass_set_scale(float scale);

void glass_release(RgBitmap *b);

/* Slides a backdrop by (dx, dy) in place (edge pixels are replicated). */
void glass_shift(RgBitmap *b, int dx, int dy);
void glass_clear_cache(void);

#endif /* RG_GLASS_H */

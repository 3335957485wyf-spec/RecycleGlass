/* Backdrop provider: a blurred crop of the desktop behind the card.
 *
 * The live path captures the screen with the widget excluded from capture and
 * reconstructs what it covers.  A static path renders the desktop window (or
 * the wallpaper file when the shell cannot render it) for the cases where no
 * live frame exists yet - no flicker, no display-affinity side effects. */
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

/* Same crop as glass_backdrop() / glass_live(), but of the unblurred capture
 * those blurred pixels were made from.  The refraction band samples this so the
 * rim keeps bending real detail even at blur radii that erase all local
 * contrast from the blurred plane.  Call right after the blurred crop succeeded;
 * the geometry must match. */
BOOL glass_backdrop_rim(int x, int y, int w, int h, RgBitmap *out);

/* Captures the desktop behind (x, y, w, h) and blurs it: this is the real
 * "frosted glass over whatever is there" effect.  With the widget excluded
 * from capture (WDA_EXCLUDEFROMCAPTURE) the grab is clean as-is; when the user
 * keeps the card capture-visible, the registered self layers are
 * un-composited out of it instead. */
BOOL glass_live(int x, int y, int w, int h, float blur_radius, RgBitmap *out);

/* Unblurred companion of a successful glass_live() crop (same frame region). */
BOOL glass_live_rim(int x, int y, int w, int h, RgBitmap *out);

/* Frame-exact synchronous backdrop for drag frames.  Grabs exactly (x,y,w,h)
 * on the calling thread - no helper IPC, no DwmFlush, no worker round-trip -
 * so the painted backdrop is the screen as it is in this very frame and the
 * glass never "swims" against the desktop.  out receives the blurred crop;
 * out_rim may be NULL or receive the unblurred companion. */
BOOL glass_drag_backdrop(int x, int y, int w, int h, float blur_radius,
                         RgBitmap *out, RgBitmap *out_rim);

/* Monotonic id of the cached live frame; changes whenever a new capture or a
 * re-blur has landed.  Lets a caller skip repaints that would show the same
 * pixels, and lets a cached layer know its backdrop went stale. */
LONG glass_live_gen(void);

/* Age in ms of the live tile the card would paint right now, -1 if none. */
LONG glass_live_age_ms(void);

/* Forces the next glass_live() to take a fresh capture. */
void glass_live_invalidate(void);
void glass_set_live_idle(BOOL on);   /* still-card backdrop keeps refreshing */

/* Kicks a fresh capture without invalidating the current tile. */
void glass_live_warm(void);

/* Tiles published since start (used to measure the live frame rate). */
LONG glass_live_frames(void);

/* Average cost of one tile rebuild (and of the capture inside it), in us.
 * Resets the counters on read. */
void glass_live_timings(LONG *cap_us, LONG *build_us, LONG *builds);

/* Re-blurs the cached capture with a new radius (no screen grab).  Used while
 * the blur slider is being dragged. */
BOOL glass_live_reblur(float blur_radius);

/* Freezes the background capture while the menu owns the cache. */
void glass_live_menu(BOOL open);

/* Starts the background capture thread; `hwnd` gets RG_WM_REFRESH when a new
 * capture lands. */
void glass_live_set_target(HWND hwnd);

/* TRUE parks the live-capture worker (the card is reading the desktop). */
void glass_live_pause(BOOL pause);

/* TRUE stops all screen grabs until FALSE (workstation locked). */
void glass_live_suspend(BOOL suspend);

/* Tells the capture thread where the card is right now, so it keeps refreshing
 * the blur around it (call once per frame). */
void glass_live_follow(int x, int y, int w, int h, float blur_radius, BOOL hold);

/* Synchronous variant: captures on the calling thread (used for one-off paints
 * such as opening the menu, where a hitch does not matter). */

/* Raw screen pixels of one rectangle, unblurred.  The menu keeps its own copy
 * so that dragging the radius slider is a copy plus one Gaussian instead of a
 * second screen grab - and so the two windows never contend for one cache. */
BOOL glass_capture(int x, int y, int w, int h, RgBitmap *out);

/* Returns the plane to the wallpaper. */
void glass_unfreeze_plane(void);

/* Blurs `src` into a freshly allocated `out`. */
BOOL glass_blurred_copy(const RgBitmap *src, float radius, RgBitmap *out);

/* Same result at half the cost: downsample, blur, bilinear upscale. */
BOOL glass_blur_half(const RgBitmap *src, float radius, RgBitmap *out);

/* The last uploaded premultiplied BGRA layered surface, so a screen grab can
 * reconstruct the desktop under our own windows without ever toggling WDA
 * (which made recorders show the widget blinking).  which: 0 = card,
 * 1 = menu.  Call with px = NULL when the window is hidden/destroyed. */
void glass_set_self_layer(int which, const BYTE *px, int w, int h, int x, int y);

/* Age in ms of the card's uploaded surface (large before the first upload). */
int glass_self_age_ms(void);

/* Bracket one UpdateLayeredWindow plus its glass_set_self_layer() call.  A
 * grab waits on the compositor inside this guard, so it can never see a
 * composed frame that is newer than the surface it un-composites. */
void glass_frame_begin(void);
void glass_frame_end(void);

/* Blocks until the pending UpdateLayeredWindow upload has reached the
 * compositor (DwmFlush).  Called inside the frame guard, right after the
 * upload was registered, so a later grab cannot un-composite a surface the
 * screen is not showing yet. */
void glass_present(void);

/* Remember the widget's / menu's window rect for the fallback reconstruction.
 * Call after every move/resize; pass w=0 when the menu closes. */
void glass_set_widget_rect(int x, int y, int w, int h);
void glass_set_menu_rect(int x, int y, int w, int h);

/* TRUE when the layered windows are capture-visible (WDA_NONE).  With WDA
 * exclusion on, no reconstruction is needed and the real desktop is kept. */
void glass_set_capture_visible(BOOL on);

/* Copies the top-left w x h of `src` into `out`. */
BOOL glass_crop(const RgBitmap *src, int w, int h, RgBitmap *out);

/* TRUE when the desktop could be captured (Win10 2004+ with WDA support). */
BOOL glass_live_available(void);

/* Display scale (1.0 = 96 dpi).  Blur radii are authored in logical pixels,
 * so the engine multiplies them by this to look the same at any scaling. */
void glass_set_scale(float scale);

void glass_release(RgBitmap *b);

#endif /* RG_GLASS_H */

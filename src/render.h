/* Fluent glass card renderer.
 *
 * The card is a single premultiplied-ARGB surface composited in this order:
 *
 *     drop shadow -> blurred wallpaper -> tint -> sheen -> rim -> icon
 *                 -> title / subtitle -> action pills -> drop ripple
 *
 * Every layout constant is derived from a 1.0x reference design and rebuilt
 * whenever the scale changes, so the widget is pixel-crisp at any DPI. */
#ifndef RG_RENDER_H
#define RG_RENDER_H

#include "common.h"
#include "glass.h"

typedef struct {
    HDC       dc;
    HBITMAP   bmp;
    HGDIOBJ   old;
    void     *bits;
    int       w, h;
    GpBitmap *gp;
} RgSurface;

typedef struct {
    const RgBitmap *backdrop;   /* card-sized crop, may be NULL */
    int             backdrop_x, backdrop_y;  /* screen origin of that crop */
    /* Identity of the backdrop contents.  The static layer is cached, so this
     * has to take part in the cache key or a live backdrop would never reach
     * the screen - the body would keep whatever frame was painted first. */
    unsigned        backdrop_gen;
    float           blur;       /* blur radius used for the crop */
    GpImage        *icon;       /* unused, kept for API symmetry */
    const wchar_t  *title;
    const wchar_t  *subtitle;
    int             hover;      /* pointer inside the card */
    int             hover_btn;  /* 0 none, 1 open, 2 empty */
    float           btn_t[2];   /* eased highlight per pill */
    float           hover_t;    /* 0..1 hover animation */
    float           opacity;    /* 0.2..1.0 */
    float           material;   /* 0..1 */
    int             drag_hover;
    int             has_fx;
    float           fx_t;       /* 0..1 ripple */
    int             dark;
} RgRenderState;

void  render_init(HINSTANCE inst);
void  render_shutdown(void);

void  render_set_scale(float s);
float render_scale(void);
int   render_win_w(void);
int   render_win_h(void);
int   render_card_w(void);
int   render_card_h(void);
int   render_pad(void);

BOOL  render_surface_resize(RgSurface *s, int w, int h);
void  render_surface_free(RgSurface *s);

void  render_card(RgSurface *s, const RgRenderState *st);

/* Colours resolved from the card palette, so secondary surfaces (the context
 * menu) can match the widget instead of hard-coding a second theme. */
typedef struct {
    ARGB text, dim, accent, accent_soft, border;
    ARGB hover_bg, track, pill_bg, pill_text, switch_off;
    /* Saturated fill for the "on" switch track.  accent is deliberately muted
     * in the dark palette because it doubles as text colour; a switch needs the
     * full-strength blue to read as "on" at 26 px. */
    ARGB switch_on;
} RgTheme;

void  render_theme(int dark, RgTheme *out);

/* Paints the card material (shadow, blurred backdrop, tint, sheen, rim,
 * border) into an existing surface at an arbitrary box.  Callers must leave
 * shadow_pad() room around (x, y, w, h) and draw their content afterwards. */
void  render_glass_panel(RgSurface *s, const RgBitmap *backdrop,
                         int x, int y, int w, int h, int radius,
                         float opacity, float material, int dark);
int   render_shadow_pad(void);

/* Drops the cached static layer (shadow + glass body + rim + icon + title).
 * Call it when anything that feeds those layers changes. */
void  render_invalidate(void);

/* Pill hit rectangles in client coordinates, refreshed by render_card(). */
extern RECT g_hit_open;
extern RECT g_hit_empty;

void  render_drop_caches(void);

#endif /* RG_RENDER_H */

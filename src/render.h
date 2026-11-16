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
    int             glass_style; /* 0 Crystal, 1 Aurora, 2 Mist */
    int             accent;      /* 0 ice, 1 iris, 2 mint, 3 amber */
    float           corner_radius; /* 12..44 reference-DPI px; 0 = default */
    float           refraction;  /* 0..1 Crystal refraction / dispersion */
    int             icon_style;  /* 0 accent-tinted chip, 1 original chip */
    float           light_phase; /* 0..1 Aurora light orbit */
    float           press;       /* 0..1 restrained press feedback */
} RgRenderState;

void  render_init(HINSTANCE inst);
void  render_shutdown(void);

void  render_set_scale(float s);
float render_scale(void);
/* The single UI typeface family (bundled MiSans, or the platform fallback).
 * The returned pointer stays valid for the whole process. */
GpFontFamily *render_app_family(void);
/* How far a font's glyphs sit above the centre of GDI+'s line box.  Positive
 * means "draw this many pixels lower" so the INK, not the line box, is centred.
 * Measured once per font by render_text_ink_offset(); callers should cache it. */
float render_text_ink_offset(GpFont *font);
int   render_win_w(void);
int   render_win_h(void);
int   render_card_w(void);
int   render_card_h(void);
int   render_pad(void);
int   render_refract_pad(void);

/* A themed copy of the app mark for the tray / window icon.  The caller owns
 * the HICON and must DestroyIcon() it. */
HICON render_make_icon(int accent, int dark, int size);

BOOL  render_surface_resize(RgSurface *s, int w, int h);
void  render_surface_free(RgSurface *s);

void  render_card(RgSurface *s, const RgRenderState *st);

/* Colours resolved from the card palette, so secondary surfaces (the context
 * menu) can match the widget instead of hard-coding a second theme. */
typedef struct {
    ARGB text, dim, accent, accent_soft, border;
    ARGB accent_alt, accent_alt_soft;
    ARGB hover_bg, track, pill_bg, pill_text, switch_off;
    /* Saturated fill for the "on" switch track, resolved from the selected
     * accent rather than sharing the text colour's dark-theme muting. */
    ARGB switch_on;
} RgTheme;

void  render_theme(int dark, int accent, RgTheme *out);

/* The gradient pair behind an accent preset.  Menu swatches and the card use
 * one shared table, so a colour can never mean two slightly different things. */
void  render_accent_palette(int dark, int accent,
                            ARGB *from, ARGB *to, ARGB *text);

/* Paints the card material (shadow, blurred backdrop, tint, sheen, rim,
 * border) into an existing surface at an arbitrary box.  Callers must leave
 * shadow_pad() room around (x, y, w, h) and draw their content afterwards. */
/* `coverage` > 0 pins the surface alpha (0..1) instead of deriving it from
 * the opacity slider.  Menus pass ~0.96 so the blurred backdrop actually
 * replaces the sharp desktop; the decorative card passes 0. */
void  render_glass_panel(RgSurface *s, const RgBitmap *backdrop,
                         int x, int y, int w, int h, int radius,
                         float opacity, float material, int dark,
                         int glass_style, int accent, float refraction,
                         float coverage);
int   render_shadow_pad(void);

/* Drops the cached static layer (shadow + glass body + rim + icon + title).
 * Call it when anything that feeds those layers changes. */
void  render_invalidate(void);

/* Pill and title-menu hit rectangles in client coordinates, refreshed by
 * render_card(). */
extern RECT g_hit_open;
extern RECT g_hit_empty;
extern RECT g_hit_menu;

void  render_drop_caches(void);

#endif /* RG_RENDER_H */

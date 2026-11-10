/* Lucide icon set, drawn as vectors.
 *
 * The icons are the official Lucide 24x24 stroke paths, parsed at runtime by a
 * miniature SVG path interpreter (M/L/H/V/C/S/Q/T/A/Z, absolute + relative) and
 * stroked with round caps and joins.  Nothing is rasterised to a bitmap, so the
 * set stays crisp at any scale and follows the theme colour. */
#ifndef RG_ICON_H
#define RG_ICON_H

#include "common.h"

typedef enum {
    IC_FOLDER_OPEN = 0,
    IC_TRASH,
    IC_SLIDERS,
    IC_PIN,
    IC_POWER,
    IC_MONITOR,
    IC_ROTATE_CCW,
    IC_X,
    IC_CHEVRON_LEFT,
    IC_CHEVRON_RIGHT,
    IC_CHECK,
    IC_SUN,
    IC_MOON,
    IC_DROPLET,
    IC_LAYERS,
    IC_WIND,
    IC_MAXIMIZE,
    IC_COUNT
} RgIcon;

/* Strokes icon `id` centred on (cx, cy) inside a size x size box. */
void icon_draw(GpGraphics *g, int id, float cx, float cy, float size,
               ARGB color, float stroke);

/* Convenience: draws inside the rect (x, y, size, size). */
void icon_draw_at(GpGraphics *g, int id, float x, float y, float size,
                  ARGB color, float stroke);

#endif /* RG_ICON_H */

/* Persistent settings, stored as JSON at %APPDATA%\RecycleGlass\config.json.
 *
 * Two rules carried over from the original design:
 *   - writes are atomic (temp file in the same folder, then MoveFileEx), so a
 *     half-written file can never brick the widget;
 *   - every value is clamped on load, because a hand-edited "opacity": "abc"
 *     used to raise inside the render path. */
#ifndef RG_CONFIG_H
#define RG_CONFIG_H

#include "common.h"

#define RG_CONFIG_VERSION 4

typedef struct {
    int   version;
    int   x, y;
    float opacity;
    float material;
    float blur_radius;
    wchar_t size[16];      /* small | normal | large */
    wchar_t blur_mode[16]; /* screen | wallpaper */
    wchar_t theme[16];     /* system | light | dark */
    wchar_t glass[16];     /* crystal | aurora | mist */
    wchar_t accent[16];    /* blue | violet | mint | peach | amber | lime */
    wchar_t icon[16];      /* accent | classic */
    float corner_radius;   /* reference-DPI pixels, 12..44 */
    float refraction;      /* 0..1 */
    float motion;          /* 0..1; scales hover/entry animation */
    int   capture_visible; /* show the widget in screenshots */
    int   auto_dpi;
    int   topmost;         /* keep above normal windows (off by default) */
    wchar_t language[8];   /* auto | zh | en */
    int   autostart;
    int   confirm_empty;
    int   width, height;
    long long last_count;
    long long last_size;
} Config;

void config_defaults(Config *c);
void config_sanitise(Config *c);
void config_load(Config *c);
BOOL config_save(const Config *c);
void config_path(wchar_t *out, size_t cap);
void config_dir(wchar_t *out, size_t cap);

#endif /* RG_CONFIG_H */

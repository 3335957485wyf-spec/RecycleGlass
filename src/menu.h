/* Self-drawn context menu.
 *
 * The widget window is WS_EX_NOACTIVATE and never becomes foreground, which
 * makes TrackPopupMenu unreliable (the menu can be dismissed instantly or not
 * appear at all).  This is our own layered popup instead: the same glass
 * material as the card (shadow, blurred wallpaper, tint, sheen, rim), Lucide
 * icons, switches and live sliders. */
#ifndef RG_MENU_H
#define RG_MENU_H

#include "common.h"
#include "config.h"

typedef struct {
    Config *cfg;
    void  (*on_command)(int cmd);
    void  (*on_set)(int id, float value);   /* RG_SET_* slider callbacks */
    /* Called while a slider is being dragged with a continuous (unquantised)
     * value; the host repaints the card at the display refresh rate so both
     * windows follow the knob.  on_set() runs once when the drag ends. */
    void  (*on_preview)(int id, float value);
    int     count;    /* recycle bin item count, drives "Empty" enabled state */
    long long size;   /* total bytes, shown in the empty confirmation */
    int     dark;     /* resolved theme (initial value) */
    int     capture_effective; /* the card is part of captures right now */
    float   scale;    /* same UI scale as the card (initial value) */
    /* Live queries so the panel follows theme/size changes while it is open. */
    int   (*query_dark)(void);
    float (*query_scale)(void);
} MenuHost;

void menu_init(HINSTANCE inst, MenuHost *host);
void menu_show(int screen_x, int screen_y);
/* Opens the menu with the "empty the bin?" prompt already unfolded. */
void menu_confirm_show(int screen_x, int screen_y);
/* Call after the item count changes: the prompt quotes it. */
void menu_data_changed(void);
void menu_hide(void);
void menu_shutdown(void);
/* Re-apply WDA on the menu window after the "visible in screenshots"
 * toggle changes (the card and the menu must agree). */
void menu_sync_capture(void);
/* Raise the open panel back to the top of the topmost band.  The host calls
 * this after re-raising the card: SetWindowPos(HWND_TOPMOST) on the card
 * would otherwise put it over the panel, and the card's lens then samples
 * the panel's stale self-image. */
void menu_keep_above(void);

#endif /* RG_MENU_H */

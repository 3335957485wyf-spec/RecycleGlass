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
    /* Called while a slider is being dragged: update the value but skip the
     * expensive card repaint - the menu repaints itself anyway, and the card is
     * behind it.  on_set() runs once when the drag ends. */
    void  (*on_preview)(int id, float value);
    int     count;    /* recycle bin item count, drives "Empty" enabled state */
    long long size;   /* total bytes, shown in the empty confirmation */
    int     dark;     /* resolved theme (initial value) */
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
BOOL menu_visible(void);
void menu_shutdown(void);

#endif /* RG_MENU_H */

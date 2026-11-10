/* Minimal IDropTarget so Explorer drops are accepted (and so the card can
 * show its "release to recycle" hint while a drag hovers over it). */
#ifndef RG_DROPTARGET_H
#define RG_DROPTARGET_H

#include "common.h"

typedef void (*RgDropEnterFn)(void);
typedef void (*RgDropLeaveFn)(void);
typedef void (*RgDropFilesFn)(wchar_t **paths, int n);

BOOL droptarget_register(HWND hwnd, RgDropEnterFn on_enter,
                         RgDropLeaveFn on_leave, RgDropFilesFn on_drop);
void droptarget_revoke(void);

#endif /* RG_DROPTARGET_H */

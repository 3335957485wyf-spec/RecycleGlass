/* Minimal IDropTarget so Explorer drops are accepted (and so the card can
 * show its "release to recycle" hint while a drag hovers over it). */
#ifndef RG_DROPTARGET_H
#define RG_DROPTARGET_H

#include "common.h"

/* count/bytes describe what is being dragged, so the card can preview
 * the drop instead of only washing its background. */
typedef void (*RgDropEnterFn)(int count, unsigned long long bytes, int has_folder);
typedef void (*RgDropLeaveFn)(void);
typedef void (*RgDropFilesFn)(wchar_t **paths, int n);

BOOL droptarget_register(HWND hwnd, RgDropEnterFn on_enter,
                         RgDropLeaveFn on_leave, RgDropFilesFn on_drop);
void droptarget_revoke(void);

#endif /* RG_DROPTARGET_H */

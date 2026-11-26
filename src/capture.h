/* DXGI Desktop Duplication frame cache — the widget's own screen grab.
 *
 * Replaces the BitBlt-per-paint grab (a ~3.5 ms GPU→CPU stall per frame on
 * hybrid graphics) and the Magnification helper process (the ghost-window
 * source).  One thread per monitor keeps the newest composed desktop frame
 * in a GPU texture; cap_grab() copies just the wanted rect out of it through
 * a staging texture, which measured at 0.2–0.8 ms on the target machine.
 *
 * When the card is excluded from capture (WDA_EXCLUDEFROMCAPTURE) the frames
 * never contain the widget, so no self-reconstruction is needed at all. */
#ifndef RG_CAPTURE_H
#define RG_CAPTURE_H

#include "common.h"

/* Lazily enumerates outputs and starts the duplication threads.  Returns
 * FALSE when no output could be opened (the caller then BitBlts). */
BOOL cap_start(void);

/* Stops the capture threads and releases the D3D objects. */
void cap_stop(void);

/* Refreshes the snapshot back to back (live background) or at the idle
 * cadence (~30 Hz).  Only meaningful while cap_grab() is being used. */
void cap_set_live(BOOL on);

LONG cap_frames(void);

/* FALSE lets the snapshot idle down to the normal 30 Hz cadence. */
void cap_set_demand(BOOL on);

/* Parks the duplication threads entirely (no reads, no copies).  Used while
 * the card paints from a frozen frame and nothing consumes snapshots. */
void cap_set_paused(BOOL on);

/* Copies the screen rect (x, y, w, h) into `out` as tightly packed BGRA
 * (w * 4 bytes per row).  Returns FALSE when this rect is not covered by a
 * live duplication stream — the caller falls back to a GDI grab. */
BOOL cap_grab(int x, int y, int w, int h, BYTE *out);

#endif /* RG_CAPTURE_H */

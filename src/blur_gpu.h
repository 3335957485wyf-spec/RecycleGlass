/* GPU blur — D3D11 separable box-blur pipeline for the drag fast path.
 *
 * The CPU blur (prefix sums) is radius-free but still costs ~1 ms on the UI
 * thread, and the 2x bilinear crop of the blurred plane costs another ~1 ms:
 * together they eat half the 240 Hz frame budget and the drag stutters as
 * soon as blur_radius > 0.  This module moves the whole
 * downsample -> blur -> upsample chain to the GPU:
 *
 *   upload card rect -> 2x2 box downsample -> 3x separable box blur
 *                    -> bilinear upsample -> one small readback
 *
 * Pixel semantics match glass.c's downsample2 + blur_bgra (sigma halved at
 * half resolution, three box passes, edge-clamped) to within UNORM rounding.
 * Any failure (no D3D11, device lost) returns FALSE and the caller keeps its
 * CPU path - the glass never depends on the GPU succeeding. */
#ifndef BLUR_GPU_H
#define BLUR_GPU_H

#include <windows.h>

/* Lazy one-time device + shader setup.  Safe to call repeatedly. */
BOOL gpu_blur_init(void);
void gpu_blur_shutdown(void);

/* Full-resolution in/out: src/dst are w*h BGRA (4 bytes/pixel, w,h >= 4).
 * sigma is the FULL-resolution blur sigma (glass_drag_backdrop's r); the
 * chain blurs at half resolution with sigma/2 exactly like the CPU path.
 * dst receives the blurred, upsampled frame; alpha = 255. */
BOOL gpu_blur2x(const BYTE *src, BYTE *dst, int w, int h, float sigma);

#endif

/* One-shot asset tool: downscale res/app-icon-1920.png to the 512 px copy
 * embedded in the exe.  The runtime bakes the icon to 256 px anyway, so 512
 * keeps a 2x margin while cutting ~350 KB from the binary.
 *
 *   g++ -O2 -DUNICODE -D_UNICODE tools/shrink_icon.c -o build/shrink_icon.exe \
 *       -lgdiplus -lole32 && build/shrink_icon.exe
 */
#include <windows.h>
#include <gdiplus.h>
#include <stdio.h>

using namespace Gdiplus;

int main(void)
{
    GdiplusStartupInput in;
    ULONG_PTR token;
    Bitmap *src, *dst;
    Graphics *g;
    CLSID png;
    UINT num = 0, size = 0;
    ImageCodecInfo *info;

    GdiplusStartup(&token, &in, NULL);
    src = new Bitmap(L"res/app-icon-1920.png");
    if (src->GetLastStatus() != Ok) { fprintf(stderr, "load failed\n"); return 1; }

    dst = new Bitmap(512, 512, PixelFormat32bppARGB);
    g = Graphics::FromImage(dst);
    g->SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g->SetPixelOffsetMode(PixelOffsetModeHalf);
    g->SetSmoothingMode(SmoothingModeAntiAlias);
    g->Clear(Color(0, 0, 0, 0));
    g->DrawImage(src, 0, 0, 512, 512);
    delete g;

    DllExports::GdipGetImageEncodersSize(&num, &size);
    info = (ImageCodecInfo *)malloc(size);
    DllExports::GdipGetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; i++)
        if (wcscmp(info[i].MimeType, L"image/png") == 0) { png = info[i].Clsid; break; }
    free(info);

    Status st = dst->Save(L"res/app-icon-512.png", &png, NULL);
    printf("save status=%d src=%ux%u\n", (int)st, src->GetWidth(), src->GetHeight());
    delete src;
    delete dst;
    GdiplusShutdown(token);
    return st == Ok ? 0 : 1;
}
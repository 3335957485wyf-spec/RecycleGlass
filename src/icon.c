#include "icon.h"

#define RG_PI 3.14159265358979f

/* --------------------------------------------------------------- path data */

static const char *const ICON_PATH[IC_COUNT] = {
    /* IC_FOLDER_OPEN */
    "M6 14l1.45-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.55 6a2 2 0 0 1-1.94 "
    "1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 "
    "1.67.9H18a2 2 0 0 1 2 2v2",

    /* IC_TRASH (trash-2) */
    "M3 6h18M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 "
    "1 2 2v2M10 11v6M14 11v6",

    /* IC_SLIDERS (sliders-horizontal) */
    "M21 4h-7M10 4H3M21 12h-9M8 12H3M21 20h-5M12 20H3M14 2v4M8 10v4M16 18v4",

    /* IC_PIN */
    "M12 17v5M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 "
    "1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 "
    "0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z",

    /* IC_POWER */
    "M12 2v10M18.36 6.64a9 9 0 1 1-12.73 0",

    /* IC_MONITOR */
    "M8 21h8M12 17v4M4 3h16a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 "
    "0 1 2-2z",

    /* IC_ROTATE_CCW */
    "M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8M3 3v5h5",

    /* IC_X */
    "M18 6 6 18M6 6l12 12",

    /* IC_CHEVRON_LEFT */
    "m15 18-6-6 6-6",

    /* IC_CHEVRON_RIGHT */
    "m9 18 6-6-6-6",

    /* IC_CHECK */
    "M20 6 9 17l-5-5",

    /* IC_SUN */
    "M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 "
    "17.66l1.41 1.41M2 12h2M20 12h2M6.34 17.66l-1.41 1.41M19.07 4.93l-1.41 1.41",

    /* IC_MOON */
    "M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z",

    /* IC_DROPLET */
    "M12 22a7 7 0 0 0 7-7c0-2-1-3.9-3-5.5s-3.5-4-4-6.5c-.5 2.5-2 4.9-4 6.5C6 11.1 "
    "5 13 5 15a7 7 0 0 0 7 7z",

    /* IC_LAYERS */
    "m12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 "
    "1.66 0l8.58-3.9a1 1 0 0 0 0-1.83zM22 17.65l-9.17 4.16a2 2 0 0 1-1.66 0L2 "
    "17.65M22 12.65l-9.17 4.16a2 2 0 0 1-1.66 0L2 12.65",

    /* IC_WIND */
    "M17.7 7.7a2.5 2.5 0 1 1 1.8 4.3H2M9.6 4.6A2 2 0 1 1 11 8H2M12.6 19.4A2 2 0 1 "
    "0 14 16H2",

    /* IC_MAXIMIZE */
    "M8 3H5a2 2 0 0 0-2 2v3M21 8V5a2 2 0 0 0-2-2h-3M3 16v3a2 2 0 0 0 2 2h3M16 "
    "21h3a2 2 0 0 0 2-2v-3"
};

/* ---------------------------------------------------------------- builder */

typedef struct {
    GpPath *p;
    float   s, ox, oy;      /* 24-grid -> device: scale + origin */
    float   cx, cy;         /* current point, 24-grid */
    float   sx, sy;         /* current subpath start */
    float   kx, ky;         /* last cubic control point */
    char    last;           /* last command, for S/T reflection */
} Ctx;

static float dx(Ctx *c, float v) { return c->ox + v * c->s; }
static float dy(Ctx *c, float v) { return c->oy + v * c->s; }

static void move_to(Ctx *c, float x, float y)
{
    c->cx = x;
    c->cy = y;
    c->sx = x;
    c->sy = y;
    GdipStartPathFigure(c->p);
}

static void line_to(Ctx *c, float x, float y)
{
    GdipAddPathLine(c->p, dx(c, c->cx), dy(c, c->cy), dx(c, x), dy(c, y));
    c->cx = x;
    c->cy = y;
}

static void cubic_to(Ctx *c, float x1, float y1, float x2, float y2,
                     float x, float y)
{
    GdipAddPathBezier(c->p,
                      dx(c, c->cx), dy(c, c->cy),
                      dx(c, x1),    dy(c, y1),
                      dx(c, x2),    dy(c, y2),
                      dx(c, x),     dy(c, y));
    c->kx = x2;
    c->ky = y2;
    c->cx = x;
    c->cy = y;
}

static float vlen(float x, float y) { return sqrtf(x * x + y * y); }

/* Angle between (ux,uy) and (vx,vy), signed, in radians. */
static float angle_between(float ux, float uy, float vx, float vy)
{
    float dot = ux * vx + uy * vy;
    float len = vlen(ux, uy) * vlen(vx, vy);
    float a;

    if (len < 1e-9f)
        return 0.0f;
    a = acosf(rg_clampf(dot / len, -1.0f, 1.0f));
    if (ux * vy - uy * vx < 0.0f)
        a = -a;
    return a;
}

/* SVG elliptical arc (endpoint parameterisation) -> cubic segments. */
static void arc_to(Ctx *c, float rx, float ry, float phi_deg, int large,
                   int sweep, float x2, float y2)
{
    float x1 = c->cx, y1 = c->cy;
    float phi, cs, sn, hx, hy, x1p, y1p, lambda, sign, num, den, co;
    float cxp, cyp, ecx, ecy, ux, uy, vx, vy, th1, dth, d, t;
    int segs, i;

    if (fabsf(x1 - x2) < 1e-6f && fabsf(y1 - y2) < 1e-6f)
        return;

    rx = fabsf(rx);
    ry = fabsf(ry);
    if (rx < 1e-6f || ry < 1e-6f)
    {
        line_to(c, x2, y2);
        return;
    }

    phi = phi_deg * RG_PI / 180.0f;
    cs = cosf(phi);
    sn = sinf(phi);

    hx = (x1 - x2) * 0.5f;
    hy = (y1 - y2) * 0.5f;
    x1p =  cs * hx + sn * hy;
    y1p = -sn * hx + cs * hy;

    lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0f)
    {
        float k = sqrtf(lambda);
        rx *= k;
        ry *= k;
    }

    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    sign = (large != sweep) ? 1.0f : -1.0f;
    co = (den > 0.0f) ? sign * sqrtf(fmaxf(0.0f, num / den)) : 0.0f;

    cxp =  co * rx * y1p / ry;
    cyp = -co * ry * x1p / rx;

    ecx = cs * cxp - sn * cyp + (x1 + x2) * 0.5f;
    ecy = sn * cxp + cs * cyp + (y1 + y2) * 0.5f;

    ux = (x1p - cxp) / rx;
    uy = (y1p - cyp) / ry;
    vx = (-x1p - cxp) / rx;
    vy = (-y1p - cyp) / ry;

    th1 = angle_between(1.0f, 0.0f, ux, uy);
    dth = angle_between(ux, uy, vx, vy);

    if (!sweep && dth > 0.0f)
        dth -= 2.0f * RG_PI;
    else if (sweep && dth < 0.0f)
        dth += 2.0f * RG_PI;

    segs = (int)ceilf(fabsf(dth) / (RG_PI * 0.5f));
    if (segs < 1)
        segs = 1;
    d = dth / (float)segs;
    t = 4.0f / 3.0f * tanf(d * 0.25f);

    for (i = 0; i < segs; i++)
    {
        float a1 = th1 + d * (float)i;
        float a2 = a1 + d;
        float c1 = cosf(a1), s1 = sinf(a1);
        float c2 = cosf(a2), s2 = sinf(a2);
        float p1x = ecx + rx * c1 * cs - ry * s1 * sn;
        float p1y = ecy + rx * c1 * sn + ry * s1 * cs;
        float p2x = ecx + rx * c2 * cs - ry * s2 * sn;
        float p2y = ecy + rx * c2 * sn + ry * s2 * cs;
        float d1x = -rx * s1 * cs - ry * c1 * sn;
        float d1y = -rx * s1 * sn + ry * c1 * cs;
        float d2x = -rx * s2 * cs - ry * c2 * sn;
        float d2y = -rx * s2 * sn + ry * c2 * cs;

        cubic_to(c,
                 p1x + t * d1x, p1y + t * d1y,
                 p2x - t * d2x, p2y - t * d2y,
                 p2x, p2y);
    }

    /* Absorb rounding drift so the next segment starts exactly on target. */
    c->cx = x2;
    c->cy = y2;
}

static const char *scan_num(const char *p, float *out)
{
    char *end;

    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n')
        p++;
    if (!*p)
    {
        *out = 0.0f;
        return p;
    }
    *out = (float)strtod(p, &end);
    if (end == p)          /* not a number: skip one byte so we never loop */
        return p + 1;
    return end;
}

static GpPath *build_path(const char *d, float cx, float cy, float size)
{
    Ctx c;
    const char *p = d;
    char cmd = 0;

    memset(&c, 0, sizeof(c));
    if (GdipCreatePath(FillModeWinding, &c.p) != Ok)
        return NULL;

    c.s = size / 24.0f;
    c.ox = cx - size * 0.5f;
    c.oy = cy - size * 0.5f;

    while (*p)
    {
        float a[7];
        int   rel, i;
        char  u;

        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n')
            p++;
        if (!*p)
            break;

        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
            cmd = *p++;
        else if (!cmd)
            break;

        rel = (cmd >= 'a' && cmd <= 'z');
        u = (char)((cmd >= 'a' && cmd <= 'z') ? cmd - 32 : cmd);

        for (i = 0; i < 7; i++)
            a[i] = 0.0f;

        switch (u)
        {
        case 'M':
            p = scan_num(p, &a[0]);
            p = scan_num(p, &a[1]);
            if (rel) { a[0] += c.cx; a[1] += c.cy; }
            move_to(&c, a[0], a[1]);
            c.last = 'M';
            cmd = rel ? 'l' : 'L';   /* extra pairs are implicit linetos */
            break;

        case 'L':
            p = scan_num(p, &a[0]);
            p = scan_num(p, &a[1]);
            if (rel) { a[0] += c.cx; a[1] += c.cy; }
            line_to(&c, a[0], a[1]);
            c.last = 'L';
            break;

        case 'H':
            p = scan_num(p, &a[0]);
            if (rel) a[0] += c.cx;
            line_to(&c, a[0], c.cy);
            c.last = 'H';
            break;

        case 'V':
            p = scan_num(p, &a[0]);
            if (rel) a[0] += c.cy;
            line_to(&c, c.cx, a[0]);
            c.last = 'V';
            break;

        case 'C':
            for (i = 0; i < 6; i++)
                p = scan_num(p, &a[i]);
            if (rel)
                for (i = 0; i < 6; i += 2)
                {
                    a[i] += c.cx;
                    a[i + 1] += c.cy;
                }
            cubic_to(&c, a[0], a[1], a[2], a[3], a[4], a[5]);
            c.last = 'C';
            break;

        case 'S':
            for (i = 0; i < 4; i++)
                p = scan_num(p, &a[i]);
            if (rel)
                for (i = 0; i < 4; i += 2)
                {
                    a[i] += c.cx;
                    a[i + 1] += c.cy;
                }
            {
                float x1 = c.cx, y1 = c.cy;
                if (c.last == 'C' || c.last == 'S')
                {
                    x1 = 2.0f * c.cx - c.kx;
                    y1 = 2.0f * c.cy - c.ky;
                }
                cubic_to(&c, x1, y1, a[0], a[1], a[2], a[3]);
            }
            c.last = 'S';
            break;

        case 'Q':
        {
            float qx, qy, x, y;
            p = scan_num(p, &qx);
            p = scan_num(p, &qy);
            p = scan_num(p, &x);
            p = scan_num(p, &y);
            if (rel)
            {
                qx += c.cx; qy += c.cy;
                x += c.cx;  y += c.cy;
            }
            {
                float x0 = c.cx, y0 = c.cy;
                cubic_to(&c,
                         x0 + (qx - x0) * 2.0f / 3.0f, y0 + (qy - y0) * 2.0f / 3.0f,
                         x + (qx - x) * 2.0f / 3.0f,   y + (qy - y) * 2.0f / 3.0f,
                         x, y);
            }
            c.last = 'Q';
            break;
        }

        case 'T':
        {
            float x, y;
            p = scan_num(p, &x);
            p = scan_num(p, &y);
            if (rel) { x += c.cx; y += c.cy; }
            {
                float qx = 2.0f * c.cx - c.kx;
                float qy = 2.0f * c.cy - c.ky;
                float x0 = c.cx, y0 = c.cy;
                if (c.last != 'Q' && c.last != 'T')
                {
                    qx = c.cx;
                    qy = c.cy;
                }
                cubic_to(&c,
                         x0 + (qx - x0) * 2.0f / 3.0f, y0 + (qy - y0) * 2.0f / 3.0f,
                         x + (qx - x) * 2.0f / 3.0f,   y + (qy - y) * 2.0f / 3.0f,
                         x, y);
            }
            c.last = 'T';
            break;
        }

        case 'A':
            for (i = 0; i < 7; i++)
                p = scan_num(p, &a[i]);
            if (rel) { a[5] += c.cx; a[6] += c.cy; }
            arc_to(&c, a[0], a[1], a[2], (int)(a[3] != 0.0f),
                   (int)(a[4] != 0.0f), a[5], a[6]);
            c.last = 'A';
            break;

        case 'Z':
            GdipClosePathFigure(c.p);
            c.cx = c.sx;
            c.cy = c.sy;
            c.last = 'Z';
            break;

        default:
            /* Unknown command: bail out rather than spin. */
            p = d + strlen(d);
            break;
        }
    }

    return c.p;
}

/* ----------------------------------------------------------------- public */

void icon_draw(GpGraphics *g, int id, float cx, float cy, float size,
               ARGB color, float stroke)
{
    GpPath *path;
    GpPen *pen = NULL;

    if (!g || id < 0 || id >= IC_COUNT || size <= 1.0f)
        return;

    path = build_path(ICON_PATH[id], cx, cy, size);
    if (!path)
        return;

    if (GdipCreatePen1(color, stroke, UnitPixel, &pen) == Ok)
    {
        GdipSetPenStartCap(pen, LineCapRound);
        GdipSetPenEndCap(pen, LineCapRound);
        GdipSetPenLineJoin(pen, LineJoinRound);
        GdipDrawPath(g, pen, path);
        GdipDeletePen(pen);
    }

    GdipDeletePath(path);
}

void icon_draw_at(GpGraphics *g, int id, float x, float y, float size,
                  ARGB color, float stroke)
{
    icon_draw(g, id, x + size * 0.5f, y + size * 0.5f, size, color, stroke);
}

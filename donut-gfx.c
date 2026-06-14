// Donut at pixel resolution — the iconic spinning shaded torus.
// A torus is parametrised by theta (around the tube) and phi (around the
// centre), revolved and tumbled by two spin angles, projected with
// perspective, and shaded by the dot of the surface normal with a light.
// In pixel mode the surface is splatted as small shaded discs into an RGBA
// framebuffer with a per-pixel z-buffer, so the near surface occludes the
// back and the donut floats opaque over the transparent terminal
// background. Falls back to a colour ASCII-ramp donut on the cell grid in
// terminals without graphics support.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space tumbles (random spin impulse), mouse click spins
// toward the pointer, 'a' toggles autopilot (steady spin), 'c' cycles
// palette + tube shape, +/- change the spin speed.

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "termpaint.h"
#include "termpaintx.h"

#include "kitty_gfx.h"

#define FRAME_MS_CELLS 33
#define FRAME_MS_GFX 50     // 20fps — kitty RGBA frames are large; tune with DONUT_FPS

// Inigo Quilez cosine palette: colour(t) = a + b * cos(2pi (c*t + d)).
typedef struct {
    const char *name;
    float a[3], b[3], c[3], d[3];
} scheme;

static const scheme SCHEMES[] = {
    { "gold",   {0.55f,0.42f,0.18f}, {0.45f,0.40f,0.20f}, {1.0f,1.0f,1.0f}, {0.00f,0.10f,0.20f} },
    { "neon",   {0.50f,0.40f,0.55f}, {0.50f,0.45f,0.45f}, {1.0f,1.0f,1.0f}, {0.50f,0.20f,0.85f} },
    { "ember",  {0.50f,0.30f,0.15f}, {0.50f,0.32f,0.20f}, {1.0f,1.0f,1.0f}, {0.00f,0.12f,0.20f} },
    { "ice",    {0.40f,0.50f,0.60f}, {0.35f,0.40f,0.40f}, {1.0f,1.0f,1.0f}, {0.60f,0.45f,0.30f} },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

// Tube/centre radius pairs — 'c' cycles through fatter and thinner donuts.
typedef struct { const char *name; float r1, r2; } shape;
static const shape SHAPES[] = {
    { "classic", 1.0f, 2.0f },
    { "fat",     1.2f, 1.8f },
    { "thin",    0.7f, 2.2f },
};
#define NSHAPES ((int)(sizeof(SHAPES) / sizeof(SHAPES[0])))

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

static bool quit_requested;
static int scheme_idx;
static int shape_idx;
static bool autopilot = true;
static float spin_mul = 1.0f;
static float A, B;             // the two spin angles
static float vA = 0.55f, vB = 0.32f;   // spin velocities (rad/s base)

// Per-pixel z-buffer (1/z, larger is nearer). A static 1024*1024 float
// buffer (4MB) covers the largest sensible framebuffer; the renderer
// guards W*H <= ZMAX.
#define ZDIM 1024
#define ZMAX (ZDIM * ZDIM)
static float zbuf[ZMAX];

static float frand(void) {
    return (float)rand() / (float)RAND_MAX;
}

static int clamp255(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void palette(float t, const scheme *s, int *r, int *g, int *b) {
    *r = clamp255((int)(255 * (s->a[0] + s->b[0] * cosf(6.2831853f * (s->c[0] * t + s->d[0])))));
    *g = clamp255((int)(255 * (s->a[1] + s->b[1] * cosf(6.2831853f * (s->c[1] * t + s->d[1])))));
    *b = clamp255((int)(255 * (s->a[2] + s->b[2] * cosf(6.2831853f * (s->c[2] * t + s->d[2])))));
}

static void step(float dt) {
    if (autopilot) {
        A += vA * spin_mul * dt;
        B += vB * spin_mul * dt;
    }
}

// Shade a surface luminance through the palette, with a specular flash
// toward white at the bright end.
static void shade(float lum, const scheme *s, int *r, int *g, int *b) {
    palette(0.30f + lum * 0.55f, s, r, g, b);
    if (lum > 0.85f) {
        float hot = (lum - 0.85f) / 0.15f;
        *r += (int)((255 - *r) * hot * 0.7f);
        *g += (int)((255 - *g) * hot * 0.7f);
        *b += (int)((255 - *b) * hot * 0.7f);
        *r = clamp255(*r); *g = clamp255(*g); *b = clamp255(*b);
    }
}

// Pixel-mode shading: unlike cells (where the glyph carries the shape even
// in dark colours), pixels need real brightness. Take one bright base
// colour from the palette and scale it by luminance with an ambient floor,
// so the shadow side stays visibly coloured instead of vanishing to black.
static void shade_px(float lum, const scheme *s, int *r, int *g, int *b) {
    palette(0.80f, s, r, g, b);
    float k = 0.24f + 0.82f * lum;
    *r = (int)(*r * k);
    *g = (int)(*g * k);
    *b = (int)(*b * k);
    if (lum > 0.80f) {
        float hot = (lum - 0.80f) / 0.20f;
        *r += (int)((255 - *r) * hot * 0.85f);
        *g += (int)((255 - *g) * hot * 0.85f);
        *b += (int)((255 - *b) * hot * 0.85f);
    }
    *r = clamp255(*r); *g = clamp255(*g); *b = clamp255(*b);
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// The torus surface is splatted opaquely into the framebuffer with a
// per-pixel z-buffer; everywhere else stays transparent (alpha 0) so the
// donut floats over the terminal's own background colour.

// Splat a small shaded disc, z-tested against the per-pixel buffer so the
// front of the donut occludes the back. ooz is 1/z (larger is nearer).
static void px_disc(uint8_t *fb, float *zb, int W, int H, float fx, float fy,
                    float radius, float ooz, int r, int g, int b) {
    int ir = (int)radius + 1;
    int cx = (int)fx, cy = (int)fy;
    for (int dy = -ir; dy <= ir; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= H) {
            continue;
        }
        for (int dx = -ir; dx <= ir; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= W) {
                continue;
            }
            float d2 = (float)(dx * dx + dy * dy) / (radius * radius);
            if (d2 >= 1.0f) {
                continue;
            }
            int idx = y * W + x;
            if (ooz <= zb[idx]) {
                continue;   // something nearer already owns this pixel
            }
            zb[idx] = ooz;
            // mostly-solid disc with a thin anti-aliased rim: dense splats
            // overlap under the z-test, so a soft falloff would show up as
            // speckle across the surface — keep the core flat instead.
            float f = d2 < 0.82f ? 1.0f : (1.0f - d2) / 0.18f;
            uint8_t *px = &fb[(size_t)idx * 4];
            px[0] = (uint8_t)(r * f);
            px[1] = (uint8_t)(g * f);
            px[2] = (uint8_t)(b * f);
            px[3] = 255;
        }
    }
}

static void render_px(void) {
    if (!kitty_sync_size(&kctx)) {
        return;
    }
    int W = kctx.fb_w, H = kctx.fb_h;
    if ((size_t)W * H > ZMAX) {
        return;   // beyond the static z-buffer cap; nothing to draw safely
    }
    uint8_t *fb = kctx.fb;
    const scheme *s = &SCHEMES[scheme_idx];
    const shape *sh = &SHAPES[shape_idx];

    // clear: framebuffer transparent, z-buffer empty
    size_t total = (size_t)W * H * 4;
    for (size_t i = 0; i < total; i++) {
        fb[i] = 0;
    }
    for (int i = 0; i < W * H; i++) {
        zbuf[i] = 0.0f;
    }

    float cA = cosf(A), sA = sinf(A);
    float cB = cosf(B), sB = sinf(B);

    float R1 = sh->r1, R2 = sh->r2;
    float K2 = 5.0f;
    int dim = W < H ? W : H;
    float K1 = dim * K2 * 0.40f / (R1 + R2);

    // light from the upper-left-front
    float lx = -0.5f, ly = 0.5f, lz = -1.0f;
    float ln = 1.0f / sqrtf(lx * lx + ly * ly + lz * lz);
    lx *= ln; ly *= ln; lz *= ln;

    // sample density scales with resolution so the disc splats overlap and
    // leave no gaps; disc radius scales likewise
    // Fixed sample counts (not scaled by dim — that blew up to tens of
    // millions of iterations per frame); the disc radius scales with the
    // framebuffer so the splats overlap and leave the surface gap-free.
    float dphi = 6.2831853f / 600.0f;
    float dtheta = 6.2831853f / 300.0f;
    float radius = dim * 0.014f + 2.0f;

    for (float phi = 0; phi < 6.2831853f; phi += dphi) {
        float cphi = cosf(phi), sphi = sinf(phi);
        for (float theta = 0; theta < 6.2831853f; theta += dtheta) {
            float ct = cosf(theta), st = sinf(theta);

            float cx = R2 + R1 * ct;
            float ox = cx * cphi;
            float oy = R1 * st;
            float oz = -cx * sphi;

            float nx0 = ct * cphi;
            float ny0 = st;
            float nz0 = -ct * sphi;

            float x1 = ox;
            float y1 = oy * cA - oz * sA;
            float z1 = oy * sA + oz * cA;
            float x = x1 * cB - y1 * sB;
            float y = x1 * sB + y1 * cB;
            float z = z1 + K2 + R2 + R1;

            float nx1 = nx0;
            float ny1 = ny0 * cA - nz0 * sA;
            float nz1 = ny0 * sA + nz0 * cA;
            float nx = nx1 * cB - ny1 * sB;
            float ny = nx1 * sB + ny1 * cB;
            float nz = nz1;

            float ooz = 1.0f / z;
            float sx = W / 2.0f + K1 * ooz * x;
            float sy = H / 2.0f - K1 * ooz * y;
            if (sx < 0 || sx >= W || sy < 0 || sy >= H) {
                continue;
            }

            float L = nx * lx + ny * ly + nz * lz;
            if (L < 0) L = 0;
            float lum = 0.15f + 0.85f * L;
            if (lum > 1.0f) lum = 1.0f;

            int r, g, b;
            shade_px(lum, s, &r, &g, &b);
            px_disc(fb, zbuf, W, H, sx, sy, radius, ooz, r, g, b);
        }
    }
}

// ---- cell renderer / overlay ----

// Brightness ramp, darkest to brightest (Andy Sloane's ".,-~:;=!*#$@").
static const char *RAMP = ".,-~:;=!*#$@";
#define NRAMP 12

// Cell-grid z-buffer cap (small; cell grids never approach this).
#define ZCELL 512
static float zcell[ZCELL * ZCELL];

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *s = &SCHEMES[scheme_idx];
    const shape *sh = &SHAPES[shape_idx];
    int bg = TERMPAINT_RGB_COLOR(0, 0, 0);
    char buf[160];

    if (pixel_mode) {
        // the image supplies the donut; cells only carry the status bar
        termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR,
                                TERMPAINT_DEFAULT_COLOR);
        snprintf(buf, sizeof(buf),
                 " space: tumble  click: spin  a: %s  c: %s/%s  +/-: speed x%.2f  q: quit │ kitty gfx %dx%d ",
                 autopilot ? "auto " : "manual", s->name, sh->name, spin_mul,
                 kctx.fb_w, kctx.fb_h);
        termpaint_surface_write_with_colors(surface, 0, 0, buf,
                TERMPAINT_RGB_COLOR(180, 180, 200), TERMPAINT_DEFAULT_COLOR);
        return;
    }

    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    int zw = w > ZCELL ? ZCELL : w;
    int zh = h > ZCELL ? ZCELL : h;
    for (int i = 0; i < zw * zh; i++) {
        zcell[i] = 0.0f;
    }

    float cA = cosf(A), sA = sinf(A);
    float cB = cosf(B), sB = sinf(B);

    float R1 = sh->r1, R2 = sh->r2;
    float K2 = 5.0f;
    float K1 = zw * K2 * 0.30f / (R1 + R2);

    float lx = -0.5f, ly = 0.5f, lz = -1.0f;
    float ln = 1.0f / sqrtf(lx * lx + ly * ly + lz * lz);
    lx *= ln; ly *= ln; lz *= ln;

    for (float phi = 0; phi < 6.2831853f; phi += 0.05f) {
        float cphi = cosf(phi), sphi = sinf(phi);
        for (float theta = 0; theta < 6.2831853f; theta += 0.015f) {
            float ct = cosf(theta), st = sinf(theta);

            float cx = R2 + R1 * ct;
            float ox = cx * cphi;
            float oy = R1 * st;
            float oz = -cx * sphi;

            float nx0 = ct * cphi;
            float ny0 = st;
            float nz0 = -ct * sphi;

            float x1 = ox;
            float y1 = oy * cA - oz * sA;
            float z1 = oy * sA + oz * cA;
            float x = x1 * cB - y1 * sB;
            float y = x1 * sB + y1 * cB;
            float z = z1 + K2 + R2 + R1;

            float nx1 = nx0;
            float ny1 = ny0 * cA - nz0 * sA;
            float nz1 = ny0 * sA + nz0 * cA;
            float nx = nx1 * cB - ny1 * sB;
            float ny = nx1 * sB + ny1 * cB;
            float nz = nz1;

            float ooz = 1.0f / z;
            // cells are ~2:1 (taller than wide), so halve the y projection
            int sx = (int)(zw / 2.0f + K1 * ooz * x);
            int sy = (int)(zh / 2.0f - K1 * 0.5f * ooz * y);
            if (sx < 0 || sx >= zw || sy < 1 || sy >= zh) {
                continue;
            }

            int idx = sy * zw + sx;
            if (ooz <= zcell[idx]) {
                continue;
            }
            zcell[idx] = ooz;

            float L = nx * lx + ny * ly + nz * lz;
            if (L < 0) L = 0;
            float lum = 0.15f + 0.85f * L;
            if (lum > 1.0f) lum = 1.0f;

            int gi = (int)(lum * NRAMP);
            if (gi >= NRAMP) gi = NRAMP - 1;
            if (gi < 0) gi = 0;
            char glyph[2] = { RAMP[gi], 0 };

            int r, g, b;
            shade(lum, s, &r, &g, &b);
            termpaint_surface_write_with_colors(surface, sx, sy, glyph,
                    TERMPAINT_RGB_COLOR(r, g, b), bg);
        }
    }

    snprintf(buf, sizeof(buf),
             " space: tumble  click: spin  a: %s  c: %s/%s  +/-: speed x%.2f  q: quit ",
             autopilot ? "auto " : "manual", s->name, sh->name, spin_mul);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': autopilot = !autopilot; break;
            case 'c':
                scheme_idx = (scheme_idx + 1) % NSCHEMES;
                shape_idx = (shape_idx + 1) % NSHAPES;
                break;
            case '+':
            case '=': if (spin_mul < 4.0f) spin_mul += 0.25f; break;
            case '-': if (spin_mul > 0.25f) spin_mul -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            // random spin impulse on both axes
            autopilot = true;
            vA = (frand() * 2.0f - 1.0f) * 1.6f;
            vB = (frand() * 2.0f - 1.0f) * 1.6f;
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        int tc = termpaint_surface_width(surface);
        int tr = termpaint_surface_height(surface);
        if (tc > 0 && tr > 0) {
            // spin toward the pointer: velocity from offset off screen centre
            autopilot = true;
            float ox = (event->mouse.x + 0.5f) / tc - 0.5f;
            float oy = (event->mouse.y + 0.5f) / tr - 0.5f;
            vB = ox * 4.0f;
            vA = -oy * 4.0f;
        }
    }
}

int main(void) {
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("DONUT_CELLS")) {
        tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
        if (tty_fd >= 0) {
            kitty_ok = kitty_detect(tty_fd);
        }
    }

    integration = termpaintx_full_integration_setup_terminal_fullscreen(
            "+kbdsig +kbdsigint",
            event_callback, NULL,
            &terminal);
    surface = termpaint_terminal_get_surface(terminal);
    termpaint_terminal_set_mouse_mode(terminal, TERMPAINT_MOUSE_MODE_CLICKS);
    termpaint_terminal_set_cursor_visible(terminal, false);

    if (kitty_ok) {
        int max_dim = 512;  // ~1MB RGBA per frame; use DONUT_MAXDIM=1024 for sharper
        const char *s = getenv("DONUT_MAXDIM");
        if (s && atoi(s) >= 128) {
            max_dim = atoi(s);
            if (max_dim > ZDIM) {
                max_dim = ZDIM;   // keep within the static z-buffer cap
            }
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    int frame_ms = pixel_mode ? FRAME_MS_GFX : FRAME_MS_CELLS;
    const char *fps_env = getenv("DONUT_FPS");
    if (fps_env && atoi(fps_env) > 0) {
        frame_ms = 1000 / atoi(fps_env);
    }

    int timeout = frame_ms;
    while (!quit_requested) {
        if (pixel_mode) {
            kitty_begin_sync(tty_fd);
            redraw();
            termpaint_terminal_flush(terminal, false);
            render_px();
            kitty_present(&kctx);
            kitty_end_sync(tty_fd);
        } else {
            redraw();
            termpaint_terminal_flush(terminal, false);
        }
        if (!termpaintx_full_integration_do_iteration_with_timeout(integration, &timeout)) {
            break;
        }
        if (timeout == 0) {
            step(frame_ms / 1000.0f);
            timeout = frame_ms;
        }
    }

    if (pixel_mode) {
        kitty_close(&kctx);
    }
    termpaint_terminal_free_with_restore(terminal);
    if (tty_fd >= 0) {
        close(tty_fd);
    }
    return 0;
}

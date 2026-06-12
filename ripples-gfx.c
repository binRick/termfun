// Water ripples at pixel resolution — termpaint + kitty graphics.
// A damped wave equation runs on a height field the size of the
// framebuffer; the surface is shaded by its slope (light from the upper
// left) with specular glints toward the sun colour. Falls back to
// glyph-ramp cell rendering in terminals without graphics support.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, mouse click drops a stone at the pointer, space drops one
// somewhere random, 'a' toggles the rain, 'c' cycles the colour scheme,
// +/- change the rain rate.

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "termpaint.h"
#include "termpaintx.h"

#include "kitty_gfx.h"

#define MAX_SW 1024
#define MAX_SH 1024
#define FRAME_MS_CELLS 33
#define FRAME_MS_GFX 33      // tune with RIPPLES_FPS if frames are too heavy
#define KEEP_PER_SEC 0.80f   // wave energy kept per second, any frame rate
#define RAIN_RATE 3.0f       // drops per second at x1.00

typedef struct {
    const char *name;
    int dr, dg, db;     // deep water (trough / shadow side)
    int lr, lg, lb;     // lit water (crest / light side)
    int sr, sg, sb;     // sun colour for specular glints
    int br, bg, bb;     // background tint
} scheme;

static const scheme SCHEMES[] = {
    { "ocean",    4, 16, 58,   36, 110, 205,  255, 250, 215,   2,  6, 18 },
    { "lagoon",   2, 44, 42,   22, 165, 145,  235, 255, 235,   1, 11, 11 },
    { "sunset",  26, 10, 48,  150,  62, 128,  255, 178,  92,   9,  4, 15 },
    { "mercury", 14, 16, 20,  115, 125, 138,  255, 255, 255,   4,  5,  7 },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

// Two-buffer height field (Hugo Elias water): framebuffer-sized in pixel
// mode, terminal cells x half-cell rows in cell mode. cur is the newest.
static float buf_a[MAX_SW * MAX_SH];
static float buf_b[MAX_SW * MAX_SH];
static float *cur = buf_a, *prv = buf_b;
static int sw, sh;

static bool quit_requested;
static int scheme_idx;
static bool raining = true;
static float rate_mul = 1.0f;
static float rain_acc;
static int dropped;

static float frand(void) {
    return (float)rand() / (float)RAND_MAX;
}

static void sync_sim(void) {
    int w, h;
    if (pixel_mode) {
        w = kctx.fb_w;
        h = kctx.fb_h;
    } else {
        w = termpaint_surface_width(surface);
        h = termpaint_surface_height(surface) * 2;
    }
    if (w > MAX_SW) w = MAX_SW;
    if (h > MAX_SH) h = MAX_SH;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (w != sw || h != sh) {
        sw = w;
        sh = h;
        memset(buf_a, 0, sizeof(buf_a));
        memset(buf_b, 0, sizeof(buf_b));
    }
}

static void splash(float cx, float cy, float radius, float amp) {
    int ir = (int)radius + 1;
    for (int dy = -ir; dy <= ir; dy++) {
        int y = (int)cy + dy;
        if (y < 1 || y >= sh - 1) {
            continue;
        }
        for (int dx = -ir; dx <= ir; dx++) {
            int x = (int)cx + dx;
            if (x < 1 || x >= sw - 1) {
                continue;
            }
            float d2 = (float)(dx * dx + dy * dy) / (radius * radius);
            if (d2 >= 1.0f) {
                continue;
            }
            float f = 1.0f - d2;
            // poke both buffers so the bump starts without a shock flash
            cur[y * sw + x] -= amp * f * f;
            prv[y * sw + x] -= amp * f * f * 0.85f;
        }
    }
    dropped++;
}

static void stone(float cx, float cy) {
    splash(cx, cy, sw / 40.0f + 2.0f, 4.0f);
}

static void raindrop(void) {
    splash(2 + frand() * (sw - 4), 2 + frand() * (sh - 4),
           sw / 90.0f + 1.2f, 1.3f + frand() * 0.9f);
}

static void step(float dt) {
    sync_sim();

    if (raining) {
        rain_acc += RAIN_RATE * rate_mul * dt;
        while (rain_acc >= 1.0f) {
            rain_acc -= 1.0f;
            raindrop();
        }
    }

    // more iterations on denser grids so the apparent wave speed matches
    int iters = 1 + sw / 256;
    float damp = powf(KEEP_PER_SEC, dt / iters);
    for (int it = 0; it < iters; it++) {
        for (int y = 1; y < sh - 1; y++) {
            float *c = &cur[y * sw];
            float *p = &prv[y * sw];
            for (int x = 1; x < sw - 1; x++) {
                float v = (c[x - 1] + c[x + 1] + c[x - sw] + c[x + sw]) * 0.5f
                          - p[x];
                p[x] = v * damp;
            }
        }
        // soak up energy near the borders so reflections don't ring forever
        for (int d = 1; d <= 3; d++) {
            float soak = d == 1 ? 0.80f : d == 2 ? 0.90f : 0.96f;
            for (int x = 1; x < sw - 1; x++) {
                prv[d * sw + x] *= soak;
                prv[(sh - 1 - d) * sw + x] *= soak;
            }
            for (int y = 1; y < sh - 1; y++) {
                prv[y * sw + d] *= soak;
                prv[y * sw + sw - 1 - d] *= soak;
            }
        }
        float *t = cur;
        cur = prv;
        prv = t;
    }
}

// Slope-lit surface value: 0.5 is calm, >1 spills into the specular range.
// The pixel grid needs a finer shading constant than the coarse cell grid,
// where one step covers a whole cell of slope.
static float shade(const float *rm, const float *r0, const float *rp, int x) {
    float gx = r0[x + 1] - r0[x - 1];
    float gy = rp[x] - rm[x];
    float k = pixel_mode ? 2.60f * 512.0f / (float)sw : 1.00f;
    float v = 0.5f - k * (0.6f * gx + 0.8f * gy) + 0.05f * r0[x];
    if (v < 0.0f) v = 0.0f;
    if (v > 1.45f) v = 1.45f;
    return v;
}

static void shade_rgb(float v, const scheme *sc, int *r, int *g, int *b) {
    if (v <= 1.0f) {
        float t = v * v;    // calm water sits closer to the deep colour
        *r = (int)(sc->dr + (sc->lr - sc->dr) * t);
        *g = (int)(sc->dg + (sc->lg - sc->dg) * t);
        *b = (int)(sc->db + (sc->lb - sc->db) * t);
    } else {
        float s = (v - 1.0f) / 0.45f;
        if (s > 1.0f) s = 1.0f;
        *r = (int)(sc->lr + (sc->sr - sc->lr) * s);
        *g = (int)(sc->lg + (sc->sg - sc->lg) * s);
        *b = (int)(sc->lb + (sc->sb - sc->lb) * s);
    }
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// The image is the water surface at z=1; alpha fades out over the top
// cell row so the status bar stays readable above the pool.

static void render_px(void) {
    if (!kitty_sync_size(&kctx)) {
        return;
    }
    int W = kctx.fb_w, H = kctx.fb_h;
    uint8_t *fb = kctx.fb;
    const scheme *sc = &SCHEMES[scheme_idx];

    float band = kctx.rows > 0 ? (float)H / kctx.rows * 1.6f : 24.0f;
    for (int y = 0; y < H; y++) {
        int sy = sh == H ? y : (int)((int64_t)y * sh / H);
        if (sy < 1) sy = 1;
        if (sy > sh - 2) sy = sh - 2;
        const float *rm = &cur[(sy - 1) * sw];
        const float *r0 = &cur[sy * sw];
        const float *rp = &cur[(sy + 1) * sw];
        float af = y < band ? (float)y / band : 1.0f;
        uint8_t alpha = (uint8_t)(235.0f * af * af);
        uint8_t *px = &fb[(size_t)y * W * 4];
        // faint sky reflection toward the top of the pool
        float ambient = 0.09f * (1.0f - (float)sy / sh);
        for (int x = 0; x < W; x++, px += 4) {
            int sx = sw == W ? x : (int)((int64_t)x * sw / W);
            if (sx < 1) sx = 1;
            if (sx > sw - 2) sx = sw - 2;
            int r, g, b;
            shade_rgb(shade(rm, r0, rp, sx) + ambient, sc, &r, &g, &b);
            px[0] = (uint8_t)r;
            px[1] = (uint8_t)g;
            px[2] = (uint8_t)b;
            px[3] = alpha;
        }
    }
}

// ---- cell renderer / overlay ----

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *sc = &SCHEMES[scheme_idx];
    int bg = TERMPAINT_RGB_COLOR(sc->br, sc->bg, sc->bb);
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    char buf[160];
    if (pixel_mode) {
        // the image supplies the water; cells only carry the status bar
        snprintf(buf, sizeof(buf),
                 " click: stone  space: stone  a: rain %s  c: %s  +/-: rain x%.2f  q: quit │ kitty gfx %dx%d │ drops %d ",
                 raining ? "on" : "off", sc->name, rate_mul,
                 kctx.fb_w, kctx.fb_h, dropped);
        termpaint_surface_write_with_colors(surface, 0, 0, buf,
                TERMPAINT_RGB_COLOR(170, 185, 200), bg);
        return;
    }

    for (int y = 1; y < h && 2 * y + 1 < sh; y++) {
        const float *rt[3] = { &cur[(2 * y - 1) * sw], &cur[2 * y * sw],
                               &cur[(2 * y + 1) * sw] };
        for (int x = 1; x < w - 1 && x < sw - 1; x++) {
            float vt = shade(rt[0], rt[1], rt[2], x);
            float vb = shade(rt[1], rt[2],
                             2 * y + 2 < sh ? &cur[(2 * y + 2) * sw] : rt[2], x);
            float v = (vt + vb) * 0.5f;
            if (fabsf(v - 0.5f) < 0.06f) {
                continue;   // calm water is just the background tint
            }
            const char *glyph = v > 0.5f
                    ? (v > 1.05f ? "✦" : v > 0.72f ? "≈" : "~")
                    : (v < 0.24f ? "•" : "·");
            int r, g, b;
            shade_rgb(v, sc, &r, &g, &b);
            // cells are sparse, so lift the glyph colour for legibility
            r = r * 3 / 2 + 25; if (r > 255) r = 255;
            g = g * 3 / 2 + 25; if (g > 255) g = 255;
            b = b * 3 / 2 + 25; if (b > 255) b = 255;
            termpaint_surface_write_with_colors(surface, x, y, glyph,
                    TERMPAINT_RGB_COLOR(r, g, b), bg);
        }
    }

    snprintf(buf, sizeof(buf),
             " click: stone  space: stone  a: rain %s  c: %s  +/-: rain x%.2f  q: quit │ drops %d ",
             raining ? "on" : "off", sc->name, rate_mul, dropped);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(170, 185, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': raining = !raining; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': if (rate_mul < 4.0f) rate_mul += 0.25f; break;
            case '-': if (rate_mul > 0.3f) rate_mul -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            stone(sw * (0.15f + frand() * 0.7f), sh * (0.15f + frand() * 0.7f));
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        // mouse positions are in cell coordinates; map onto the sim grid
        int tc = termpaint_surface_width(surface);
        int tr = termpaint_surface_height(surface);
        if (tc > 0 && tr > 0) {
            stone((event->mouse.x + 0.5f) * sw / tc,
                  (event->mouse.y + 0.5f) * sh / tr);
        }
    }
}

int main(void) {
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("RIPPLES_CELLS")) {
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
        int max_dim = 512;  // ~480KB RGBA per frame; use RIPPLES_MAXDIM=1024 for sharper
        const char *s = getenv("RIPPLES_MAXDIM");
        if (s && atoi(s) >= 128) {
            max_dim = atoi(s);
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    sync_sim();

    int frame_ms = pixel_mode ? FRAME_MS_GFX : FRAME_MS_CELLS;
    const char *fps_env = getenv("RIPPLES_FPS");
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

// Matrix digital rain at pixel resolution — termpaint + kitty graphics.
// The rain falls on its own pixel glyph grid, denser than the cell grid,
// using randomly generated 5x7 bitmap glyphs. Falls back to cell rendering
// in terminals without graphics support.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space spawns a wave of drops, mouse click drops one at the
// pointer, 'c' cycles the colour scheme, +/- change the fall speed.

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

#define MAX_GCOLS 1024
#define MAX_GROWS 400
#define FRAME_MS_CELLS 33
#define FRAME_MS_GFX 50      // kitty RGBA frames are large; tune with MATRIX_FPS
#define HEAT_DECAY 1.5f      // heat per second; tail length ≈ speed / decay

// cell-mode glyphs
static const char *GLYPHS[] = {
    "ｦ", "ｱ", "ｲ", "ｳ", "ｴ", "ｵ", "ｶ", "ｷ", "ｸ", "ｹ", "ｺ", "ｻ", "ｼ", "ｽ",
    "ｾ", "ｿ", "ﾀ", "ﾁ", "ﾂ", "ﾃ", "ﾄ", "ﾅ", "ﾆ", "ﾇ", "ﾈ", "ﾉ", "ﾊ", "ﾋ",
    "ﾌ", "ﾍ", "ﾎ", "ﾏ", "ﾐ", "ﾑ", "ﾒ", "ﾓ", "ﾔ", "ﾕ", "ﾖ", "ﾗ", "ﾘ", "ﾙ",
    "ﾚ", "ﾛ", "ﾜ", "ﾝ",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Z", "*", "+", ":", "=", "<", ">", "¦",
};
#define NGLYPHS ((int)(sizeof(GLYPHS) / sizeof(GLYPHS[0])))

// pixel-mode glyphs: random 5x7 bitmaps, bit (y*5+x), generated at startup
#define NPXGLYPHS 64
static uint64_t pxglyphs[NPXGLYPHS];

typedef struct {
    const char *name;
    int hr, hg, hb;     // head colour
    int tr, tg, tb;     // tail colour at full heat
    int br, bg, bb;     // background tint
} scheme;

static const scheme SCHEMES[] = {
    { "green", 200, 255, 200,    0, 220,  70,    0, 10,  4 },
    { "amber", 255, 240, 200,  255, 176,   0,   12,  7,  0 },
    { "cyan",  220, 255, 255,    0, 200, 255,    0,  8, 12 },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

typedef struct {
    bool active;
    float head;     // grid row position of the head
    float speed;    // grid rows per second
    float delay;    // seconds until the next spawn while inactive
} drop;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

// The rain lives on a glyph grid: terminal cells in cell mode, a denser
// pixel-glyph grid in graphics mode. Heat is 1.0 where a head just passed,
// cooling to 0 along the tail. Glyph indices are mod-ed per renderer.
static int gcols, grows;
static int gpw, gph;     // glyph cell size in framebuffer pixels
static float heat[MAX_GROWS * MAX_GCOLS];
static uint8_t glyph_idx[MAX_GROWS * MAX_GCOLS];
static drop drops[MAX_GCOLS];

static bool quit_requested;
static int scheme_idx;
static float speed_mul = 1.0f;
static int spawned;

static float frand(void) {
    return (float)rand() / (float)RAND_MAX;
}

static void init_pxglyphs(void) {
    for (int i = 0; i < NPXGLYPHS; i++) {
        uint64_t bits = 0;
        int set = 0;
        do {
            bits = 0;
            set = 0;
            for (int b = 0; b < 35; b++) {
                if (rand() % 100 < 45) {
                    bits |= (uint64_t)1 << b;
                    set++;
                }
            }
        } while (set < 8);
        pxglyphs[i] = bits;
    }
}

static void sync_grid(void) {
    if (pixel_mode) {
        gpw = kctx.fb_w / 96;
        if (gpw < 5) {
            gpw = 5;
        }
        gph = gpw * 8 / 5;
        gcols = kctx.fb_w / gpw;
        grows = kctx.fb_h / gph;
    } else {
        gcols = termpaint_surface_width(surface);
        grows = termpaint_surface_height(surface);
    }
    if (gcols > MAX_GCOLS) gcols = MAX_GCOLS;
    if (grows > MAX_GROWS) grows = MAX_GROWS;
}

static void spawn_drop(int x, bool at_top) {
    drop *d = &drops[x];
    d->active = true;
    d->head = at_top ? 0 : -frand() * 6;
    d->speed = 6 + frand() * 14;
    spawned++;
}

static void step(float dt) {
    sync_grid();

    // cool every cell; tails fade behind the heads
    for (size_t i = 0; i < (size_t)MAX_GROWS * MAX_GCOLS; i++) {
        if (heat[i] > 0) {
            heat[i] -= HEAT_DECAY * dt;
            if (heat[i] < 0) {
                heat[i] = 0;
            }
        }
    }

    for (int x = 0; x < gcols; x++) {
        drop *d = &drops[x];
        if (!d->active) {
            d->delay -= dt;
            if (d->delay <= 0) {
                spawn_drop(x, false);
            }
            continue;
        }
        int prev = (int)floorf(d->head);
        d->head += d->speed * speed_mul * dt;
        int cur = (int)floorf(d->head);
        for (int y = prev + 1; y <= cur; y++) {
            if (y >= 0 && y < grows) {
                heat[y * MAX_GCOLS + x] = 1.0f;
                glyph_idx[y * MAX_GCOLS + x] = (uint8_t)(rand() & 0xff);
            }
        }
        // recycle once the whole tail has scrolled off the bottom
        if (d->head - d->speed * speed_mul / HEAT_DECAY > grows) {
            d->active = false;
            d->delay = frand() * 3.0f;
        }
    }

    // the classic flicker: lit glyphs occasionally mutate in place
    int mutations = gcols * grows / 96 + 1;
    for (int i = 0; i < mutations; i++) {
        int x = rand() % gcols;
        int y = rand() % grows;
        if (heat[y * MAX_GCOLS + x] > 0.05f) {
            glyph_idx[y * MAX_GCOLS + x] = (uint8_t)(rand() & 0xff);
        }
    }
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// The image supplies the rain at z=1; cells underneath carry only the
// status bar and the background tint. Unlit pixels stay transparent.

static void px_glow(uint8_t *fb, int W, int H, float fx, float fy,
                    float radius, int r, int g, int b) {
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
            float f = (1.0f - d2) * (1.0f - d2);
            uint8_t *px = &fb[((size_t)y * W + x) * 4];
            int v;
            v = px[0] + (int)(r * f); px[0] = v > 255 ? 255 : (uint8_t)v;
            v = px[1] + (int)(g * f); px[1] = v > 255 ? 255 : (uint8_t)v;
            v = px[2] + (int)(b * f); px[2] = v > 255 ? 255 : (uint8_t)v;
            uint8_t a = (uint8_t)((r + g + b) * f / 3);
            if (px[3] < a) {
                px[3] = a;
            }
        }
    }
}

// Blit one 5x7 bitmap glyph scaled to the (gpw-1)x(gph-2) box at x0,y0,
// leaving a 1px gap between grid neighbours.
static void blit_glyph(uint8_t *fb, int W, int H, int x0, int y0,
                       uint64_t bits, int r, int g, int b) {
    int bw = gpw - 1, bh = gph - 2;
    int a = r > g ? (r > b ? r : b) : (g > b ? g : b);
    for (int py = 0; py < bh; py++) {
        int y = y0 + py;
        if (y < 0 || y >= H) {
            continue;
        }
        int sy = py * 7 / bh;
        for (int px_ = 0; px_ < bw; px_++) {
            int x = x0 + px_;
            if (x < 0 || x >= W) {
                continue;
            }
            int sx = px_ * 5 / bw;
            if (!(bits >> (sy * 5 + sx) & 1)) {
                continue;
            }
            uint8_t *px = &fb[((size_t)y * W + x) * 4];
            px[0] = (uint8_t)r;
            px[1] = (uint8_t)g;
            px[2] = (uint8_t)b;
            px[3] = (uint8_t)a;
        }
    }
}

static void render_px(void) {
    if (!kitty_sync_size(&kctx)) {
        return;
    }
    int W = kctx.fb_w, H = kctx.fb_h;
    uint8_t *fb = kctx.fb;
    const scheme *sc = &SCHEMES[scheme_idx];

    memset(fb, 0, (size_t)W * H * 4);

    // tail glyphs, dimming with heat
    for (int gy = 0; gy < grows; gy++) {
        for (int gx = 0; gx < gcols; gx++) {
            float v = heat[gy * MAX_GCOLS + gx];
            if (v <= 0.03f) {
                continue;
            }
            float f = v * v;   // quadratic falloff keeps the tail tip dim
            blit_glyph(fb, W, H, gx * gpw, gy * gph,
                       pxglyphs[glyph_idx[gy * MAX_GCOLS + gx] % NPXGLYPHS],
                       (int)(sc->tr * f), (int)(sc->tg * f), (int)(sc->tb * f));
        }
    }

    // heads: bright glyph snapped to the grid, bloom sliding smoothly
    for (int gx = 0; gx < gcols; gx++) {
        drop *d = &drops[gx];
        if (!d->active || d->head < -1 || d->head > grows) {
            continue;
        }
        int gy = (int)floorf(d->head);
        if (gy >= 0 && gy < grows) {
            blit_glyph(fb, W, H, gx * gpw, gy * gph,
                       pxglyphs[glyph_idx[gy * MAX_GCOLS + gx] % NPXGLYPHS],
                       sc->hr, sc->hg, sc->hb);
        }
        float fx = (gx + 0.5f) * gpw;
        float fy = (d->head + 0.5f) * gph;
        px_glow(fb, W, H, fx, fy, gpw * 0.9f + 2.0f,
                sc->tr / 2, sc->tg / 2, sc->tb / 2);
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
        // the image supplies the rain; cells only carry the status bar
        snprintf(buf, sizeof(buf),
                 " space: wave  click: drop  c: %s  +/-: speed x%.2f  q: quit │ kitty gfx %dx%d, %dx%d glyphs │ drops %d ",
                 sc->name, speed_mul, kctx.fb_w, kctx.fb_h, gcols, grows, spawned);
        termpaint_surface_write_with_colors(surface, 0, 0, buf,
                TERMPAINT_RGB_COLOR(140, 160, 140), bg);
        return;
    }

    int gw = w < gcols ? w : gcols;
    int gh = h < grows ? h : grows;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            float v = heat[y * MAX_GCOLS + x];
            if (v <= 0.04f) {
                continue;
            }
            float f = v * v;
            termpaint_surface_write_with_colors(surface, x, y,
                    GLYPHS[glyph_idx[y * MAX_GCOLS + x] % NGLYPHS],
                    TERMPAINT_RGB_COLOR((int)(sc->tr * f), (int)(sc->tg * f),
                                        (int)(sc->tb * f)),
                    bg);
        }
    }

    // heads stay bright for as long as they occupy a cell
    for (int x = 0; x < gw; x++) {
        drop *d = &drops[x];
        if (!d->active) {
            continue;
        }
        int y = (int)floorf(d->head);
        if (y < 0 || y >= gh) {
            continue;
        }
        termpaint_surface_write_with_colors(surface, x, y,
                GLYPHS[glyph_idx[y * MAX_GCOLS + x] % NGLYPHS],
                TERMPAINT_RGB_COLOR(sc->hr, sc->hg, sc->hb), bg);
    }

    snprintf(buf, sizeof(buf),
             " space: wave  click: drop  c: %s  +/-: speed x%.2f  q: quit │ drops %d ",
             sc->name, speed_mul, spawned);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(140, 160, 140), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': if (speed_mul < 3.0f) speed_mul += 0.25f; break;
            case '-': if (speed_mul > 0.3f) speed_mul -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            for (int x = 0; x < gcols; x++) {
                if (!drops[x].active && drops[x].delay > 0.4f) {
                    drops[x].delay = frand() * 0.4f;
                }
            }
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        // mouse positions are in cell coordinates; map onto the glyph grid
        int tc = termpaint_surface_width(surface);
        int gx = pixel_mode && tc > 0 ? event->mouse.x * gcols / tc
                                      : event->mouse.x;
        if (gx >= 0 && gx < gcols) {
            spawn_drop(gx, true);
        }
    }
}

int main(void) {
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("MATRIX_CELLS")) {
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
        int max_dim = 512;  // ~480KB RGBA per frame; use MATRIX_MAXDIM=1024 for sharper
        const char *s = getenv("MATRIX_MAXDIM");
        if (s && atoi(s) >= 128) {
            max_dim = atoi(s);
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    init_pxglyphs();
    sync_grid();

    // stagger the columns so the rain builds up over the first seconds
    for (int x = 0; x < MAX_GCOLS; x++) {
        drops[x].active = false;
        drops[x].delay = frand() * 4.0f;
    }

    int frame_ms = pixel_mode ? FRAME_MS_GFX : FRAME_MS_CELLS;
    const char *fps_env = getenv("MATRIX_FPS");
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

// Demoscene fire at pixel resolution — termpaint + kitty graphics.
// The classic Doom fire: a heat field where every cell pulls heat from
// the row below with a little sideways drift and random cooling, so
// flames rise, flicker, and die out. Renders 2x2 pixel blocks with a
// four-stop palette; falls back to glyph-ramp cell rendering in
// terminals without graphics support.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, mouse click lobs a fireball at the pointer, space flares
// the burner, 'a' snuffs/relights it, 'c' cycles the colour scheme,
// +/- change the flame height.

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "termpaint.h"
#include "termpaintx.h"

#include "kitty_gfx.h"

#define MAX_GW 1024
#define MAX_GH 600
#define PX_BLOCK 2           // sim cell size in framebuffer pixels
#define FRAME_MS_CELLS 33
#define FRAME_MS_GFX 33      // tune with FIRE_FPS if frames are too heavy

typedef struct {
    const char *name;
    int c0[3], c1[3], c2[3], c3[3];  // palette stops at heat 0, 1/3, 2/3, 1
    int br, bg, bb;                  // background tint
} scheme;

static const scheme SCHEMES[] = {
    { "fire",   { 10,  2,  0}, {170, 24,   5}, {250, 120,  16}, {255, 235, 140}, 12, 4,  0 },
    { "gas",    {  0,  2, 14}, { 12, 60, 185}, { 70, 160, 255}, {215, 245, 255},  1, 4, 14 },
    { "toxic",  {  0,  8,  2}, { 12, 110, 28}, {110, 220,  60}, {235, 255, 180},  2, 8,  2 },
    { "plasma", { 12,  0, 18}, {115, 18, 165}, {225,  90, 225}, {255, 225, 255},  8, 2, 12 },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

// Heat field, 0..1, row gh-1 is the burner. Updated in place top-down
// (each row only reads the row below it). 2x2 framebuffer blocks in
// pixel mode, terminal cells x half-cell rows in cell mode.
static float heat[MAX_GW * MAX_GH];
static int gw, gh;

static bool quit_requested;
static int scheme_idx;
static bool burner = true;
static float height_mul = 1.0f;
static float surge;
static int flares;

static uint32_t rng = 0x2545f491;

static inline uint32_t xr(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static float frand(void) {
    return (float)(xr() & 0xffffff) / (float)0xffffff;
}

static void sync_sim(void) {
    int w, h;
    if (pixel_mode) {
        w = kctx.fb_w / PX_BLOCK;
        h = kctx.fb_h / PX_BLOCK;
    } else {
        w = termpaint_surface_width(surface);
        h = termpaint_surface_height(surface) * 2;
    }
    if (w > MAX_GW) w = MAX_GW;
    if (h > MAX_GH) h = MAX_GH;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (w != gw || h != gh) {
        gw = w;
        gh = h;
        memset(heat, 0, sizeof(heat));
    }
}

static void fireball(float cx, float cy) {
    float radius = gw / 30.0f + 2.0f;
    int ir = (int)radius + 1;
    for (int dy = -ir; dy <= ir; dy++) {
        int y = (int)cy + dy;
        if (y < 0 || y >= gh) {
            continue;
        }
        for (int dx = -ir; dx <= ir; dx++) {
            int x = (int)cx + dx;
            if (x < 0 || x >= gw) {
                continue;
            }
            if (dx * dx + dy * dy >= radius * radius) {
                continue;
            }
            heat[y * gw + x] = 1.0f;
        }
    }
    flares++;
}

static void step(float dt) {
    sync_sim();

    surge -= dt * 1.5f;
    if (surge < 0) {
        surge = 0;
    }

    // cooling is all-or-nothing per cell: half the cells lose one big
    // quantum per row (the classic Doom trick that carves the flames
    // into ragged tongues); the quantum sets the flame height at about
    // half the grid
    float k = 3.6f / (gh * height_mul * (1.0f + surge));

    // scatter, not gather: each cell pushes its heat to a jittered spot
    // one row up. Collisions and holes are the point — unwritten cells
    // keep last frame's heat, which is what makes the tongues cohere.
    for (int y = 1; y < gh; y++) {
        const float *src = &heat[y * gw];
        float *dst = &heat[(y - 1) * gw];
        for (int x = 0; x < gw; x++) {
            uint32_t r = xr();
            int dx = x + (int)(r % 3) - 1;
            if (dx < 0) dx = 0;
            if (dx >= gw) dx = gw - 1;
            float v = src[x] - (r >> 8 & 1 ? k : 0.0f);
            dst[dx] = v > 0 ? v : 0;
        }
    }

    float *src_row = &heat[(gh - 1) * gw];
    for (int x = 0; x < gw; x++) {
        if (burner) {
            float v = (0.88f + 0.12f * frand()) * (1.0f + surge * 0.1f);
            src_row[x] = v > 1.0f ? 1.0f : v;
        } else {
            src_row[x] = 0;
        }
    }
}

static void palette(float t, const scheme *sc, int *r, int *g, int *b) {
    const int *a, *c;
    float f;
    t = t * t * (2.0f - t);   // keep the tail dark so tongues stay ragged
    if (t < 1.0f / 3.0f) {
        a = sc->c0; c = sc->c1; f = t * 3.0f;
    } else if (t < 2.0f / 3.0f) {
        a = sc->c1; c = sc->c2; f = t * 3.0f - 1.0f;
    } else {
        a = sc->c2; c = sc->c3; f = t * 3.0f - 2.0f;
    }
    *r = (int)(a[0] + (c[0] - a[0]) * f);
    *g = (int)(a[1] + (c[1] - a[1]) * f);
    *b = (int)(a[2] + (c[2] - a[2]) * f);
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// The image is the flames at z=1; alpha follows the heat, so the status
// bar and terminal background show through wherever nothing burns.

static void render_px(void) {
    if (!kitty_sync_size(&kctx)) {
        return;
    }
    int W = kctx.fb_w, H = kctx.fb_h;
    uint8_t *fb = kctx.fb;
    const scheme *sc = &SCHEMES[scheme_idx];

    memset(fb, 0, (size_t)W * H * 4);

    for (int gy = 0; gy < gh; gy++) {
        int y0 = H - (gh - gy) * PX_BLOCK;
        if (y0 + PX_BLOCK <= 0) {
            continue;
        }
        for (int gx = 0; gx < gw; gx++) {
            float t = heat[gy * gw + gx];
            if (t <= 0.04f) {
                continue;
            }
            int r, g, b;
            palette(t, sc, &r, &g, &b);
            int a = t > 0.55f ? 255 : (int)(t * t * 840.0f);
            for (int py = 0; py < PX_BLOCK; py++) {
                int y = y0 + py;
                if (y < 0 || y >= H) {
                    continue;
                }
                for (int px_ = 0; px_ < PX_BLOCK; px_++) {
                    int x = gx * PX_BLOCK + px_;
                    if (x >= W) {
                        break;
                    }
                    uint8_t *px = &fb[((size_t)y * W + x) * 4];
                    px[0] = (uint8_t)r;
                    px[1] = (uint8_t)g;
                    px[2] = (uint8_t)b;
                    px[3] = (uint8_t)a;
                }
            }
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
        // the image supplies the flames; cells only carry the status bar
        snprintf(buf, sizeof(buf),
                 " click: fireball  space: flare  a: burner %s  c: %s  +/-: height x%.2f  q: quit │ kitty gfx %dx%d │ flares %d ",
                 burner ? "on" : "off", sc->name, height_mul,
                 kctx.fb_w, kctx.fb_h, flares);
        termpaint_surface_write_with_colors(surface, 0, 0, buf,
                TERMPAINT_RGB_COLOR(200, 180, 160), bg);
        return;
    }

    for (int y = 1; y < h && 2 * y + 1 < gh; y++) {
        for (int x = 0; x < w && x < gw; x++) {
            float t = (heat[2 * y * gw + x] + heat[(2 * y + 1) * gw + x]) * 0.5f;
            if (t <= 0.05f) {
                continue;
            }
            const char *glyph = t > 0.85f ? "@" : t > 0.70f ? "#"
                              : t > 0.55f ? "*" : t > 0.40f ? "~"
                              : t > 0.22f ? ":" : "·";
            int r, g, b;
            palette(t, sc, &r, &g, &b);
            // cells are sparse, so lift the glyph colour for legibility
            r = r * 3 / 2 + 20; if (r > 255) r = 255;
            g = g * 3 / 2 + 20; if (g > 255) g = 255;
            b = b * 3 / 2 + 20; if (b > 255) b = 255;
            termpaint_surface_write_with_colors(surface, x, y, glyph,
                    TERMPAINT_RGB_COLOR(r, g, b), bg);
        }
    }

    snprintf(buf, sizeof(buf),
             " click: fireball  space: flare  a: burner %s  c: %s  +/-: height x%.2f  q: quit │ flares %d ",
             burner ? "on" : "off", sc->name, height_mul, flares);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(200, 180, 160), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': burner = !burner; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': if (height_mul < 2.5f) height_mul += 0.25f; break;
            case '-': if (height_mul > 0.5f) height_mul -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            surge = 1.0f;
            flares++;
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
            fireball((event->mouse.x + 0.5f) * gw / tc,
                     (event->mouse.y + 0.5f) * gh / tr);
        }
    }
}

int main(void) {
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("FIRE_CELLS")) {
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
        int max_dim = 512;  // ~480KB RGBA per frame; use FIRE_MAXDIM=1024 for sharper
        const char *s = getenv("FIRE_MAXDIM");
        if (s && atoi(s) >= 128) {
            max_dim = atoi(s);
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    sync_sim();

    int frame_ms = pixel_mode ? FRAME_MS_GFX : FRAME_MS_CELLS;
    const char *fps_env = getenv("FIRE_FPS");
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

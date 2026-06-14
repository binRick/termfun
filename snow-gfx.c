// Snow at pixel resolution — termpaint + kitty graphics protocol.
// Drifting snowfall with wind and accumulation: hundreds of flakes in three
// parallax layers fall under per-layer gravity, carried sideways by a slowly
// varying wind plus a gentle per-flake sway. Flakes that reach the drift on
// the floor stick and pile up; the drift slowly self-smooths into mounds.
// The flakes float over the terminal background (transparent sky). Falls
// back to a glyph-field cell renderer on terminals without graphics support.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space gusts the wind, mouse click gusts at the pointer, 'a'
// toggles accumulation, 'c' cycles the type (snow / ash / petals / rain),
// +/- change the wind strength (negative blows leftward).

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

#define FRAME_MS 50     // 20fps — kitty RGBA frames are large; tune with SNOW_FPS
#define MAX_FLAKES 600
#define MAX_COLS 1024

typedef enum { T_SNOW, T_ASH, T_PETALS, T_RAIN } ptype;

typedef struct {
    const char *name;
    ptype id;
    int nr, ng, nb;     // near (bright) tint
    int fr, fg, fb_;    // far (dim) tint
    float gravity;      // base fall speed (view units / s)
    float sway;         // horizontal sway amplitude
    bool accumulates;   // does the floor drift grow
    bool streak;        // draw short vertical streaks (rain)
} typeinfo;

static const typeinfo TYPES[] = {
    { "snow",   T_SNOW,   235, 245, 255,  120, 140, 175,  0.22f, 0.45f, true,  false },
    { "ash",    T_ASH,    150, 145, 140,   70,  68,  66,  0.10f, 0.30f, true,  false },
    { "petals", T_PETALS, 255, 170, 200,  200, 110, 150,  0.18f, 0.95f, true,  false },
    { "rain",   T_RAIN,   170, 200, 235,  110, 150, 200,  0.85f, 0.06f, false, true  },
};
#define NTYPES ((int)(sizeof(TYPES) / sizeof(TYPES[0])))

typedef struct {
    float x, y;        // position in normalised view (0..1)
    float fall;        // per-flake fall multiplier
    float drift;       // per-flake drift multiplier
    float phase;       // sway phase
    float freq;        // sway frequency
    int layer;         // 0 = far/dim/slow .. 2 = near/bright/fast
} flake;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

static bool quit_requested;
static int type_idx;
static bool accumulate = true;
static float wind = 0.10f;        // global horizontal wind, view units/s
static float wind_target = 0.10f; // wind eases toward this
static float gust;                // transient gust that decays
static float gust_x = 0.5f;       // gust centre (normalised x), -1 = global
static float windphase;

static int nflakes;
static flake flakes[MAX_FLAKES];
static float drift[MAX_COLS];     // accumulation height per column, 0..1

static float frand(void) {
    return (float)rand() / (float)RAND_MAX;
}

static void spawn(flake *f, bool anywhere) {
    f->x = frand();
    f->y = anywhere ? frand() : -0.02f - frand() * 0.05f;
    int l = rand() % 3;
    f->layer = l;
    f->fall = 0.6f + 0.5f * l + frand() * 0.4f;
    f->drift = 0.7f + 0.6f * frand();
    f->phase = frand() * 6.2831853f;
    f->freq = 0.6f + frand() * 1.2f;
}

static void init_flakes(void) {
    nflakes = 400;
    for (int i = 0; i < nflakes; i++) {
        spawn(&flakes[i], true);
    }
    for (int i = 0; i < MAX_COLS; i++) {
        drift[i] = 0.0f;
    }
}

static void step(float dt) {
    const typeinfo *t = &TYPES[type_idx];

    // wind slowly wanders around its eased target; gust decays
    windphase += dt * 0.4f;
    float wander = 0.05f * sinf(windphase * 0.7f) + 0.03f * sinf(windphase * 1.9f + 1.3f);
    wind += (wind_target - wind) * dt * 0.8f;
    float w = wind + wander;
    gust -= dt * 1.6f;
    if (gust < 0) {
        gust = 0;
    }

    for (int i = 0; i < nflakes; i++) {
        flake *f = &flakes[i];
        // petals flutter (faster sway), ash drifts lazily
        float fmul = t->id == T_PETALS ? 2.2f : t->id == T_ASH ? 1.6f : 1.0f;
        f->phase += dt * f->freq * fmul;

        float fall = t->gravity * f->fall * (0.7f + 0.45f * f->layer);
        // ash is light and turbulent: it can briefly rise on an updraft
        if (t->id == T_ASH) {
            fall += t->gravity * 0.6f * sinf(f->phase * 1.3f + (float)i);
        }
        f->y += fall * dt;

        float sway = sinf(f->phase) * t->sway * 0.012f;
        float localgust = 0.0f;
        if (gust > 0) {
            if (gust_x < 0) {
                localgust = gust * 0.6f;        // global gust to the right
            } else {
                float d = f->x - gust_x;        // localized: strongest near pointer
                localgust = gust * 0.9f * expf(-(d * d) / 0.04f);
            }
        }
        f->x += (w * f->drift + localgust + sway) * dt;

        // wrap horizontally
        if (f->x < 0.0f) f->x += 1.0f;
        if (f->x >= 1.0f) f->x -= 1.0f;

        // accumulation: stick when reaching the drift height in this column
        if (t->accumulates && accumulate && f->y > 0.0f) {
            int col = (int)(f->x * MAX_COLS);
            if (col < 0) col = 0;
            if (col >= MAX_COLS) col = MAX_COLS - 1;
            float floor_y = 1.0f - drift[col];
            if (f->y >= floor_y) {
                drift[col] += 0.0012f + 0.0006f * f->layer;
                if (drift[col] > 0.45f) drift[col] = 0.45f;
                spawn(f, false);
                continue;
            }
        }
        if (f->y >= 1.05f) {
            spawn(f, false);
        }
    }

    // drift very slowly self-smooths so it forms natural mounds
    if (t->accumulates) {
        static float tmp[MAX_COLS];
        for (int i = 0; i < MAX_COLS; i++) {
            float l = drift[i > 0 ? i - 1 : i];
            float r = drift[i < MAX_COLS - 1 ? i + 1 : i];
            tmp[i] = drift[i] * 0.94f + (l + r) * 0.03f;
        }
        memcpy(drift, tmp, sizeof(drift));
    }
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// The framebuffer is cleared to transparent each frame and the flakes are
// redrawn crisply (no decay buffer). Transparent pixels reveal the terminal
// background, which forms the sky. The accumulated drift is drawn opaque.

// Additive soft glow clamped to 255; alpha = max(current, intensity) so
// overlapping glows don't double-dim against the terminal background.
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
            int lum = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int af = (int)(lum * f * 2.0f);
            if (af > 255) af = 255;
            if (px[3] < af) {
                px[3] = (uint8_t)af;
            }
        }
    }
}

static void render_px(void) {
    if (!kitty_sync_size(&kctx)) {
        return;
    }
    int W = kctx.fb_w, H = kctx.fb_h;
    uint8_t *fb = kctx.fb;
    const typeinfo *t = &TYPES[type_idx];

    // clear to fully transparent: the sky is the terminal background
    memset(fb, 0, (size_t)W * H * 4);

    // flakes — soft additive glow sized by layer, brighter the nearer it is
    for (int i = 0; i < nflakes; i++) {
        flake *f = &flakes[i];
        float fx = f->x * W;
        float fy = f->y * H;
        if (fy < 0) {
            continue;
        }
        float td = f->layer / 2.0f;   // 0 far .. 1 near
        float bri = 0.45f + 0.55f * td;
        int r = (int)((t->fr + (t->nr - t->fr) * td) * bri);
        int g = (int)((t->fg + (t->ng - t->fg) * td) * bri);
        int b = (int)((t->fb_ + (t->nb - t->fb_) * td) * bri);
        float radius = 1.2f + td * 2.2f;
        if (t->streak) {
            // rain: a short vertical streak instead of a dot
            float len = (3.0f + 5.0f * td) * (H / 200.0f + 1.0f);
            int samples = (int)(len / 1.5f) + 1;
            if (samples > 10) samples = 10;
            for (int k = 0; k < samples; k++) {
                float u = (float)k / samples;
                px_glow(fb, W, H, fx, fy - len * u, 1.0f + td * 0.8f,
                        (int)(r * (1.0f - 0.5f * u)),
                        (int)(g * (1.0f - 0.5f * u)),
                        (int)(b * (1.0f - 0.5f * u)));
            }
        } else {
            px_glow(fb, W, H, fx, fy, radius, r, g, b);
        }
    }

    // accumulation drift — opaque white-blue mound filling each column
    if (t->accumulates) {
        for (int x = 0; x < W; x++) {
            int col = (int)(((float)x + 0.5f) / W * MAX_COLS);
            if (col < 0) col = 0;
            if (col >= MAX_COLS) col = MAX_COLS - 1;
            int top = (int)((1.0f - drift[col]) * H);
            if (top < 0) top = 0;
            for (int y = top; y < H; y++) {
                // a hint of shading down into the drift
                float dd = H > top ? (float)(y - top) / (H - top) : 0.0f;
                uint8_t *px = &fb[((size_t)y * W + x) * 4];
                px[0] = (uint8_t)(235 - 30 * dd);
                px[1] = (uint8_t)(240 - 22 * dd);
                px[2] = (uint8_t)(252 - 10 * dd);
                px[3] = 255;
            }
        }
    }
}

// ---- cell renderer / overlay ----

// drift block glyphs, partial-cell fill from thin to full.
static const char *BLOCKS[] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
#define NBLOCKS ((int)(sizeof(BLOCKS) / sizeof(BLOCKS[0])))

static const char *flake_glyph(const typeinfo *t, int layer) {
    if (t->streak) {
        return layer >= 2 ? "│" : layer == 1 ? "╎" : "'";
    }
    switch (layer) {
        case 2: return "❄";
        case 1: return "❅";
        default: return "·";
    }
}

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const typeinfo *t = &TYPES[type_idx];
    char buf[200];

    if (pixel_mode) {
        // the image supplies the snow; cells only carry the status bar
        termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR,
                                TERMPAINT_DEFAULT_COLOR);
        snprintf(buf, sizeof(buf),
                 " space: gust  click: gust here  a: accum %s  c: %s  +/-: wind %.2f  q: quit │ kitty gfx %dx%d ",
                 accumulate ? "on " : "off", t->name, wind,
                 kctx.fb_w, kctx.fb_h);
        termpaint_surface_write_with_colors(surface, 0, 0, buf,
                TERMPAINT_RGB_COLOR(180, 180, 200), TERMPAINT_DEFAULT_COLOR);
        return;
    }

    int bg = TERMPAINT_RGB_COLOR(8, 10, 18);
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    // flakes
    for (int i = 0; i < nflakes; i++) {
        flake *f = &flakes[i];
        int x = (int)(f->x * w);
        int y = (int)(f->y * h);
        if (x < 0 || x >= w || y < 1 || y >= h) {
            continue;
        }
        float td = f->layer / 2.0f;   // 0 far .. 1 near
        int r = (int)(t->fr + (t->nr - t->fr) * td);
        int g = (int)(t->fg + (t->ng - t->fg) * td);
        int b = (int)(t->fb_ + (t->nb - t->fb_) * td);
        termpaint_surface_write_with_colors(surface, x, y,
                flake_glyph(t, f->layer),
                TERMPAINT_RGB_COLOR(r, g, b), bg);
        // rain draws a short streak above its head
        if (t->streak && y - 1 >= 1) {
            termpaint_surface_write_with_colors(surface, x, y - 1, "╎",
                    TERMPAINT_RGB_COLOR(r * 3 / 5, g * 3 / 5, b * 3 / 5), bg);
        }
    }

    // accumulation drift along the bottom, partial-cell block glyphs
    if (t->accumulates) {
        int drift_white = TERMPAINT_RGB_COLOR(225, 232, 245);
        for (int x = 0; x < w; x++) {
            int col = (int)(((float)x + 0.5f) / w * MAX_COLS);
            if (col < 0) col = 0;
            if (col >= MAX_COLS) col = MAX_COLS - 1;
            float rows_f = drift[col] * h;
            int full = (int)rows_f;
            float frac = rows_f - full;
            for (int k = 0; k < full && k < h; k++) {
                int y = h - 1 - k;
                if (y < 1) break;
                termpaint_surface_write_with_colors(surface, x, y, "█",
                        drift_white, bg);
            }
            int top = h - 1 - full;
            if (top >= 1 && full < h) {
                int bi = (int)(frac * (NBLOCKS - 1) + 0.5f);
                if (bi > 0) {
                    termpaint_surface_write_with_colors(surface, x, top,
                            BLOCKS[bi], drift_white, bg);
                }
            }
        }
    }

    snprintf(buf, sizeof(buf),
             " space: gust  click: gust here  a: accum %s  c: %s  +/-: wind %.2f  q: quit ",
             accumulate ? "on " : "off", t->name, wind);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': accumulate = !accumulate; break;
            case 'c':
                type_idx = (type_idx + 1) % NTYPES;
                for (int i = 0; i < MAX_COLS; i++) drift[i] = 0.0f;
                break;
            case '+':
            case '=': if (wind_target < 0.6f) wind_target += 0.05f; break;
            case '-': if (wind_target > -0.6f) wind_target -= 0.05f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            gust = 0.5f + frand() * 0.4f;
            gust_x = -1.0f;   // global gust
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        // mouse positions are in cell coordinates; map onto the view
        int tc = termpaint_surface_width(surface);
        if (tc > 0) {
            gust = 0.7f + frand() * 0.5f;
            gust_x = (event->mouse.x + 0.5f) / tc;
        }
    }
}

int main(void) {
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("SNOW_CELLS")) {
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
        int max_dim = 512;  // ~1MB RGBA per frame; use SNOW_MAXDIM=1024 for sharper
        const char *s = getenv("SNOW_MAXDIM");
        if (s && atoi(s) >= 128) {
            max_dim = atoi(s);
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    init_flakes();

    int frame_ms = FRAME_MS;
    const char *fps_env = getenv("SNOW_FPS");
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

// Snow — drifting snowfall with wind and accumulation for the terminal.
// Hundreds of flakes in three parallax layers fall under per-layer gravity,
// carried sideways by a slowly varying wind plus a gentle per-flake sway.
// Flakes that reach the drift on the floor stick and pile up; the drift
// slowly self-smooths into natural mounds. Rendered as a glyph field on the
// cell grid.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space gusts the wind, mouse click gusts at the pointer, 'a'
// toggles accumulation, 'c' cycles the type (snow / ash / petals / rain),
// +/- change the wind strength (negative blows leftward).

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "termpaint.h"
#include "termpaintx.h"

#define FRAME_MS 33
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

// ---- cell renderer ----

// drift block glyphs, partial-cell fill from thin to full.
static const char *BLOCKS[] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
#define NBLOCKS ((int)(sizeof(BLOCKS) / sizeof(BLOCKS[0])))

static const char *flake_glyph(const typeinfo *t, int layer) {
    if (t->streak) {
        return layer >= 2 ? "│" : layer == 1 ? "╎" : "'";
    }
    // snow / ash / petals share a layered glyph ramp
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

    // accumulation drift along the bottom
    if (t->accumulates) {
        int drift_white = TERMPAINT_RGB_COLOR(225, 232, 245);
        for (int x = 0; x < w; x++) {
            int col = (int)(((float)x + 0.5f) / w * MAX_COLS);
            if (col < 0) col = 0;
            if (col >= MAX_COLS) col = MAX_COLS - 1;
            float dh = drift[col];           // 0..0.45 of the screen height
            float rows_f = dh * h;
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

    char buf[200];
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
        int tc = termpaint_surface_width(surface);
        if (tc > 0) {
            gust = 0.7f + frand() * 0.5f;
            gust_x = (event->mouse.x + 0.5f) / tc;
        }
    }
}

int main(void) {
    integration = termpaintx_full_integration_setup_terminal_fullscreen(
            "+kbdsig +kbdsigint",
            event_callback, NULL,
            &terminal);
    surface = termpaint_terminal_get_surface(terminal);
    termpaint_terminal_set_mouse_mode(terminal, TERMPAINT_MOUSE_MODE_CLICKS);
    termpaint_terminal_set_cursor_visible(terminal, false);

    init_flakes();

    int timeout = FRAME_MS;
    while (!quit_requested) {
        redraw();
        termpaint_terminal_flush(terminal, false);
        if (!termpaintx_full_integration_do_iteration_with_timeout(integration, &timeout)) {
            break;
        }
        if (timeout == 0) {
            step(FRAME_MS / 1000.0f);
            timeout = FRAME_MS;
        }
    }

    termpaint_terminal_free_with_restore(terminal);
    return 0;
}

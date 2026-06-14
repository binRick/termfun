// Coral — a Gray-Scott reaction-diffusion system grown on the cell grid.
// Two chemical fields U and V react and diffuse across a small grid,
// growing coral, fingerprint, spot and maze morphologies depending on the
// feed/kill rates. The V field is mapped through a cosine palette and a
// glyph density ramp for the organic look.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space reseeds with random V blobs, mouse click seeds a blob
// at the pointer, 'a' pauses the growth, 'c' cycles the preset + palette,
// +/- change the simulation speed (substeps per frame).

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "termpaint.h"
#include "termpaintx.h"

#define FRAME_MS 33
#define MAX_SW 200          // reaction-diffusion is heavy; keep the grid small
#define MAX_SH 200

// Gray-Scott diffusion rates and timestep. Different feed (F) / kill (k)
// pairs grow wildly different morphologies, gathered here as named presets.
#define DU 0.16f
#define DV 0.08f
#define DT 1.0f

typedef struct {
    const char *name;
    float f, k;
} preset;

static const preset PRESETS[] = {
    { "coral",   0.0545f, 0.0620f },
    { "mitosis", 0.0367f, 0.0649f },
    { "spots",   0.0300f, 0.0620f },
    { "maze",    0.0290f, 0.0570f },
    { "waves",   0.0140f, 0.0450f },
};
#define NPRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

// Inigo Quilez cosine palette: colour(t) = a + b * cos(2pi (c*t + d)).
typedef struct {
    const char *name;
    float a[3], b[3], c[3], d[3];
} scheme;

static const scheme SCHEMES[] = {
    { "reef",   {0.50f,0.50f,0.50f}, {0.50f,0.50f,0.50f}, {1.0f,1.0f,1.0f}, {0.00f,0.10f,0.20f} },
    { "ember",  {0.50f,0.35f,0.20f}, {0.50f,0.35f,0.25f}, {1.0f,1.0f,1.0f}, {0.00f,0.12f,0.20f} },
    { "lagoon", {0.30f,0.50f,0.55f}, {0.30f,0.45f,0.45f}, {1.0f,1.0f,1.0f}, {0.55f,0.30f,0.20f} },
    { "moss",   {0.30f,0.45f,0.30f}, {0.35f,0.40f,0.30f}, {1.0f,1.0f,1.0f}, {0.20f,0.10f,0.30f} },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

// Two-buffer concentration fields on a capped grid. cur is the newest
// state; we read cur and write nxt, then swap.
static float u_a[MAX_SW * MAX_SH];
static float v_a[MAX_SW * MAX_SH];
static float u_b[MAX_SW * MAX_SH];
static float v_b[MAX_SW * MAX_SH];
static float *u_cur = u_a, *v_cur = v_a;
static float *u_nxt = u_b, *v_nxt = v_b;
static int sw, sh;

static bool quit_requested;
static int preset_idx;
static int scheme_idx;
static bool paused;
static int substeps = 8;        // Gray-Scott iterations per displayed frame

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

// Drop a square blob of V (and depleted U) at grid (cx, cy); this is the
// disturbance the reaction grows outward from.
static void seed_blob(int cx, int cy, int radius) {
    for (int dy = -radius; dy <= radius; dy++) {
        int y = cy + dy;
        if (y < 1 || y >= sh - 1) {
            continue;
        }
        for (int dx = -radius; dx <= radius; dx++) {
            int x = cx + dx;
            if (x < 1 || x >= sw - 1) {
                continue;
            }
            int i = y * sw + x;
            u_cur[i] = 0.5f;
            v_cur[i] = 0.25f + frand() * 0.25f;
        }
    }
}

// U=1, V=0 everywhere, then scatter a handful of random V blobs to grow from.
static void reseed(void) {
    for (int i = 0; i < sw * sh; i++) {
        u_cur[i] = 1.0f;
        v_cur[i] = 0.0f;
    }
    int blobs = 6 + (int)(frand() * 6);
    int radius = sw / 40 + 2;
    for (int b = 0; b < blobs; b++) {
        seed_blob(2 + (int)(frand() * (sw - 4)),
                  2 + (int)(frand() * (sh - 4)), radius);
    }
}

static void sync_sim(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface) * 2;
    if (w > MAX_SW) w = MAX_SW;
    if (h > MAX_SH) h = MAX_SH;
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (w != sw || h != sh) {
        sw = w;
        sh = h;
        reseed();
    }
}

// One Gray-Scott update. Reads u_cur/v_cur, writes u_nxt/v_nxt, then swaps.
// The 5-point Laplacian uses clamped edges so the borders stay quiescent.
static void react(void) {
    float F = PRESETS[preset_idx].f;
    float k = PRESETS[preset_idx].k;
    for (int y = 1; y < sh - 1; y++) {
        int row = y * sw;
        const float *uc = &u_cur[row];
        const float *vc = &v_cur[row];
        const float *un = &u_cur[row - sw];
        const float *us = &u_cur[row + sw];
        const float *vn = &v_cur[row - sw];
        const float *vs = &v_cur[row + sw];
        float *uo = &u_nxt[row];
        float *vo = &v_nxt[row];
        for (int x = 1; x < sw - 1; x++) {
            float u = uc[x];
            float v = vc[x];
            float lapU = uc[x - 1] + uc[x + 1] + un[x] + us[x] - 4.0f * u;
            float lapV = vc[x - 1] + vc[x + 1] + vn[x] + vs[x] - 4.0f * v;
            float uvv = u * v * v;
            float nu = u + (DU * lapU - uvv + F * (1.0f - u)) * DT;
            float nv = v + (DV * lapV + uvv - (F + k) * v) * DT;
            // clamp to keep the system bounded; reaction-diffusion can drift
            if (nu < 0.0f) nu = 0.0f; else if (nu > 1.0f) nu = 1.0f;
            if (nv < 0.0f) nv = 0.0f; else if (nv > 1.0f) nv = 1.0f;
            uo[x] = nu;
            vo[x] = nv;
        }
    }
    // keep the clamped border held at the resting state U=1, V=0
    for (int x = 0; x < sw; x++) {
        u_nxt[x] = 1.0f; v_nxt[x] = 0.0f;
        u_nxt[(sh - 1) * sw + x] = 1.0f; v_nxt[(sh - 1) * sw + x] = 0.0f;
    }
    for (int y = 0; y < sh; y++) {
        u_nxt[y * sw] = 1.0f; v_nxt[y * sw] = 0.0f;
        u_nxt[y * sw + sw - 1] = 1.0f; v_nxt[y * sw + sw - 1] = 0.0f;
    }
    float *tu = u_cur; u_cur = u_nxt; u_nxt = tu;
    float *tv = v_cur; v_cur = v_nxt; v_nxt = tv;
}

static void step(void) {
    sync_sim();
    if (paused) {
        return;
    }
    for (int i = 0; i < substeps; i++) {
        react();
    }
}

// Density ramp: sparse glyphs for thin V, dense for thick growth.
static const char *RAMP[] = { " ", "·", ":", "+", "*", "x", "#", "%", "@" };
#define NRAMP ((int)(sizeof(RAMP) / sizeof(RAMP[0])))

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *s = &SCHEMES[scheme_idx];
    const preset *p = &PRESETS[preset_idx];
    int bg = TERMPAINT_RGB_COLOR(0, 0, 0);
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    for (int y = 1; y < h && 2 * y + 1 < sh; y++) {
        const float *vt = &v_cur[(2 * y) * sw];
        const float *vbm = &v_cur[(2 * y + 1) * sw];
        for (int x = 0; x < w && x < sw; x++) {
            float val = (vt[x] + vbm[x]) * 0.5f;
            int gi = (int)(val * 3.2f * NRAMP);
            if (gi >= NRAMP) gi = NRAMP - 1;
            if (gi <= 0) continue;   // bare substrate stays background
            int r, g, b;
            palette(val * 2.4f + 0.15f, s, &r, &g, &b);
            // cells are sparse, so lift the glyph colour for legibility
            r = r * 3 / 2 + 25; if (r > 255) r = 255;
            g = g * 3 / 2 + 25; if (g > 255) g = 255;
            b = b * 3 / 2 + 25; if (b > 255) b = 255;
            termpaint_surface_write_with_colors(surface, x, y, RAMP[gi],
                    TERMPAINT_RGB_COLOR(r, g, b), bg);
        }
    }

    char buf[200];
    snprintf(buf, sizeof(buf),
             " space: reseed  click: seed  a: %s  c: %s/%s  +/-: speed x%d  q: quit ",
             paused ? "paused" : "live  ", p->name, s->name, substeps);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': paused = !paused; break;
            case 'c':
                preset_idx = (preset_idx + 1) % NPRESETS;
                scheme_idx = (scheme_idx + 1) % NSCHEMES;
                reseed();
                break;
            case '+':
            case '=': if (substeps < 20) substeps++; break;
            case '-': if (substeps > 1) substeps--; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            reseed();
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
            int gx = (int)((event->mouse.x + 0.5f) * sw / tc);
            int gy = (int)((event->mouse.y + 0.5f) * sh / tr);
            seed_blob(gx, gy, sw / 40 + 3);
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

    sync_sim();

    int timeout = FRAME_MS;
    while (!quit_requested) {
        redraw();
        termpaint_terminal_flush(terminal, false);
        if (!termpaintx_full_integration_do_iteration_with_timeout(integration, &timeout)) {
            break;
        }
        if (timeout == 0) {
            step();
            timeout = FRAME_MS;
        }
    }

    termpaint_terminal_free_with_restore(terminal);
    return 0;
}

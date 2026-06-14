// Julia set — an animated, colour-cycling Julia fractal for the terminal.
// Each cell iterates z = z*z + c and is shaded by a smooth (continuous)
// escape count mapped through a cycling cosine palette, rendered as a
// colour glyph ramp on the cell grid. The constant c drifts on a loop just
// inside the main cardioid so the fractal breathes and morphs.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space jumps c to a new pretty value, mouse click sets c from
// the pointer, 'a' toggles autopilot (c drifting on a loop), 'c' cycles the
// palette, +/- raise/lower the iteration detail.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "termpaint.h"
#include "termpaintx.h"

#define FRAME_MS 33
#define VIEW 1.6f       // half-width of the complex view on the real axis
#define MINITER 40
#define MAXITER_CAP 300

// Inigo Quilez cosine palette: colour(t) = a + b * cos(2pi (c*t + d)).
typedef struct {
    const char *name;
    float a[3], b[3], c[3], d[3];
} scheme;

static const scheme SCHEMES[] = {
    { "neon",   {0.50f,0.50f,0.50f}, {0.50f,0.50f,0.50f}, {1.0f,1.0f,1.0f}, {0.00f,0.33f,0.67f} },
    { "ember",  {0.50f,0.35f,0.20f}, {0.50f,0.35f,0.25f}, {1.0f,1.0f,1.0f}, {0.00f,0.12f,0.20f} },
    { "lagoon", {0.30f,0.50f,0.55f}, {0.30f,0.45f,0.45f}, {1.0f,1.0f,1.0f}, {0.55f,0.30f,0.20f} },
    { "candy",  {0.60f,0.50f,0.55f}, {0.40f,0.45f,0.45f}, {2.0f,1.0f,1.0f}, {0.50f,0.20f,0.25f} },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

// A handful of curated Julia constants that yield lively, connected shapes.
static const float SEEDS[][2] = {
    { -0.8000f,  0.1560f },
    { -0.7269f,  0.1889f },
    {  0.2850f,  0.0100f },
    {  0.2850f,  0.5300f },
    { -0.4000f,  0.6000f },
    { -0.7900f,  0.1500f },
    {  0.3450f,  0.3550f },
    { -0.1230f,  0.7450f },
    { -0.5400f,  0.5400f },
};
#define NSEEDS ((int)(sizeof(SEEDS) / sizeof(SEEDS[0])))

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static bool quit_requested;
static int scheme_idx;
static bool autopilot = true;
static int maxiter = 120;
static float phase;             // accumulated animation time (autopilot loop)
static float cre = -0.8f, cim = 0.156f;   // the Julia constant c

static int clamp255(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void palette(float t, const scheme *s, int *r, int *g, int *b) {
    *r = clamp255((int)(255 * (s->a[0] + s->b[0] * cosf(6.2831853f * (s->c[0] * t + s->d[0])))));
    *g = clamp255((int)(255 * (s->a[1] + s->b[1] * cosf(6.2831853f * (s->c[1] * t + s->d[1])))));
    *b = clamp255((int)(255 * (s->a[2] + s->b[2] * cosf(6.2831853f * (s->c[2] * t + s->d[2])))));
}

// Smooth (continuous) escape count for z0 = (zr, zi) under z = z*z + c.
// Returns -1 for interior points that never escape.
static float julia_mu(float zr, float zi) {
    int i;
    float zr2 = zr * zr, zi2 = zi * zi;
    for (i = 0; i < maxiter && zr2 + zi2 <= 4.0f; i++) {
        zi = 2.0f * zr * zi + cim;
        zr = zr2 - zi2 + cre;
        zr2 = zr * zr;
        zi2 = zi * zi;
    }
    if (i >= maxiter) {
        return -1.0f;   // interior
    }
    // mu = iter + 1 - log2(log2(|z|)); guard the logs near the boundary
    float mag = zr2 + zi2;
    float l = 0.5f * logf(mag > 1e-12f ? mag : 1e-12f);   // ln|z|
    float ll = logf(l > 1e-12f ? l : 1e-12f) / 0.6931472f;  // log2(ln|z|)
    return (float)i + 1.0f - ll;
}

static void step(float dt) {
    phase += dt;
    if (autopilot) {
        // trace just inside the main cardioid boundary; a slow Lissajous
        // wobble keeps the shape morphing rather than merely spinning
        float t = phase * 0.35f;
        float r = 0.7885f;
        cre = r * cosf(t) + 0.04f * cosf(t * 1.7f + 0.6f);
        cim = r * sinf(t) + 0.04f * sinf(t * 1.3f);
    }
}

static const char *RAMP[] = { " ", ".", ":", "-", "=", "+", "*", "#", "%", "@" };
#define NRAMP ((int)(sizeof(RAMP) / sizeof(RAMP[0])))

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *s = &SCHEMES[scheme_idx];
    int bg = TERMPAINT_RGB_COLOR(0, 0, 0);
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    // map the view rectangle to the cell grid, correcting for aspect; cells
    // are about twice as tall as wide, so scale the vertical span to match
    float ar = w > 0 ? (float)w / (h > 0 ? h : 1) * 0.5f : 1.0f;
    float spanx = VIEW;
    float spany = VIEW / (ar > 0.0f ? ar : 1.0f);
    float cyc = phase * 0.05f;   // slow colour-cycle for extra life
    for (int y = 1; y < h; y++) {
        float zi = ((y + 0.5f) / h * 2.0f - 1.0f) * spany;
        for (int x = 0; x < w; x++) {
            float zr = ((x + 0.5f) / w * 2.0f - 1.0f) * spanx;
            float mu = julia_mu(zr, zi);
            if (mu < 0.0f) {
                continue;   // interior cells stay background
            }
            int r, g, b;
            palette(mu * 0.05f + cyc, s, &r, &g, &b);
            float luma = sqrtf(mu / maxiter);
            int gi = (int)(luma * NRAMP);
            if (gi >= NRAMP) gi = NRAMP - 1;
            if (gi <= 0) gi = 1;
            termpaint_surface_write_with_colors(surface, x, y, RAMP[gi],
                    TERMPAINT_RGB_COLOR(r, g, b), bg);
        }
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             " space: jump  click: set c  a: %s  c: %s  +/-: iters %d  q: quit ",
             autopilot ? "auto  " : "manual", s->name, maxiter);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': autopilot = !autopilot; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': if (maxiter < MAXITER_CAP) maxiter += 10; break;
            case '-': if (maxiter > MINITER) maxiter -= 10; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            int i = rand() % NSEEDS;
            cre = SEEDS[i][0];
            cim = SEEDS[i][1];
            autopilot = false;
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        // mouse positions are in cell coordinates; map onto a small region
        // of c-space around the cardioid for fine, interactive control
        int tc = termpaint_surface_width(surface);
        int tr = termpaint_surface_height(surface);
        if (tc > 0 && tr > 0) {
            cre = ((event->mouse.x + 0.5f) / tc * 2.0f - 1.0f) * 0.9f;
            cim = ((event->mouse.y + 0.5f) / tr * 2.0f - 1.0f) * 0.9f;
            autopilot = false;
        }
    }
}

int main(void) {
    cre = SEEDS[0][0];
    cim = SEEDS[0][1];

    integration = termpaintx_full_integration_setup_terminal_fullscreen(
            "+kbdsig +kbdsigint",
            event_callback, NULL,
            &terminal);
    surface = termpaint_terminal_get_surface(terminal);
    termpaint_terminal_set_mouse_mode(terminal, TERMPAINT_MOUSE_MODE_CLICKS);
    termpaint_terminal_set_cursor_visible(terminal, false);

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

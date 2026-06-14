// Plasma — a colour-cycling demoscene plasma for the terminal.
// Several sine fields summed together and mapped through a cycling cosine
// palette, rendered as a colour glyph ramp on the cell grid.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space reseeds the wave centre, mouse click moves it to the
// pointer, 'a' freezes the animation, 'c' cycles the palette, +/- change
// the speed.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "termpaint.h"
#include "termpaintx.h"

#define FRAME_MS 33
#define FREQ 16.0f      // how many radians of sine span the screen

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

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static bool quit_requested;
static int scheme_idx;
static bool paused;
static float speed_mul = 1.0f;
static float phase;
static float cx = 0.5f, cy = 0.5f;

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

static float field(float u, float v, float ar, float t) {
    float x = u * FREQ;
    float y = v * FREQ;
    float dx = (u - cx) * ar * FREQ;
    float dy = (v - cy) * FREQ;
    return sinf(x + t)
         + sinf(0.7f * y - 1.1f * t)
         + sinf(0.6f * (x + y) + 0.9f * t)
         + sinf(0.5f * sqrtf(dx * dx + dy * dy) - 1.3f * t);
}

static void step(float dt) {
    if (!paused) {
        phase += dt * speed_mul;
    }
}

static const char *RAMP[] = { " ", "·", ":", "+", "*", "x", "#", "%", "@" };
#define NRAMP ((int)(sizeof(RAMP) / sizeof(RAMP[0])))

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *s = &SCHEMES[scheme_idx];
    int bg = TERMPAINT_RGB_COLOR(0, 0, 0);
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    float ar = h > 0 ? (float)w / h : 1.0f;
    for (int y = 1; y < h; y++) {
        float v = (y + 0.5f) / h;
        for (int x = 0; x < w; x++) {
            float u = (x + 0.5f) / w;
            float val = field(u, v, ar, phase) * 0.125f + 0.5f;
            int r, g, b;
            palette(val + phase * 0.02f, s, &r, &g, &b);
            float luma = (0.30f * r + 0.59f * g + 0.11f * b) / 255.0f;
            int gi = (int)(luma * NRAMP);
            if (gi >= NRAMP) gi = NRAMP - 1;
            if (gi <= 0) continue;
            termpaint_surface_write_with_colors(surface, x, y, RAMP[gi],
                    TERMPAINT_RGB_COLOR(r, g, b), bg);
        }
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             " space: reseed  click: centre  a: %s  c: %s  +/-: speed x%.2f  q: quit ",
             paused ? "frozen" : "live  ", s->name, speed_mul);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': paused = !paused; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': if (speed_mul < 4.0f) speed_mul += 0.25f; break;
            case '-': if (speed_mul > 0.25f) speed_mul -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            cx = 0.1f + frand() * 0.8f;
            cy = 0.1f + frand() * 0.8f;
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        int tc = termpaint_surface_width(surface);
        int tr = termpaint_surface_height(surface);
        if (tc > 0 && tr > 0) {
            cx = (event->mouse.x + 0.5f) / tc;
            cy = (event->mouse.y + 0.5f) / tr;
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

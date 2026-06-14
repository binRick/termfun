// Tunnel — a classic demoscene texture tunnel for the terminal.
// Each cell's polar coordinates about a drifting centre map to texture
// coords (angle wraps the wall, 1/radius flies you inward); a summed-sine
// band pattern is tinted through a cycling cosine palette, with distance
// fog so the throat recedes into darkness. Rendered as a colour glyph ramp
// on the cell grid.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space twists (kicks the angular velocity), mouse click steers
// the centre to the pointer, 'a' toggles autopilot centre drift, 'c' cycles
// the palette, +/- change the fly speed.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "termpaint.h"
#include "termpaintx.h"

#define FRAME_MS 33
#define SCALE 0.35f     // texture depth per unit radius (1/r mapping)
#define TWO_PI 6.2831853f

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
static bool autopilot = true;
static float speed_mul = 1.0f;
static float phase;            // how far we've flown inward
static float spin;             // accumulated tunnel rotation
static float spin_vel;         // angular velocity, decays toward zero
static float drift;            // autopilot Lissajous clock
static float cx = 0.5f, cy = 0.5f;   // tunnel centre, normalised

static int clamp255(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void palette(float t, const scheme *s, int *r, int *g, int *b) {
    *r = clamp255((int)(255 * (s->a[0] + s->b[0] * cosf(TWO_PI * (s->c[0] * t + s->d[0])))));
    *g = clamp255((int)(255 * (s->a[1] + s->b[1] * cosf(TWO_PI * (s->c[1] * t + s->d[1])))));
    *b = clamp255((int)(255 * (s->a[2] + s->b[2] * cosf(TWO_PI * (s->c[2] * t + s->d[2])))));
}

// Procedural wall texture sampled at (tu, tv): a summed-sine band pattern
// that wraps cleanly around the angular axis. Returns ~[0, 1].
static float texture(float tu, float tv) {
    float a = tu * TWO_PI;
    float band = sinf(12.0f * a) * sinf(7.0f * tv * TWO_PI)
               + sinf(tv * 10.0f + 0.5f * cosf(3.0f * a));
    return band * 0.25f + 0.5f;
}

static void step(float dt) {
    phase += dt * speed_mul * 0.6f;
    spin += (0.25f + spin_vel) * dt;
    spin_vel *= powf(0.2f, dt);   // twist decays toward zero each second
    if (autopilot) {
        drift += dt;
        cx = 0.5f + 0.18f * sinf(drift * 0.7f);
        cy = 0.5f + 0.18f * sinf(drift * 0.53f + 1.3f);
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

    // cells are ~twice as tall as wide; scale dy so the throat stays round
    float pcx = cx * w, pcy = cy * h;
    for (int y = 1; y < h; y++) {
        float dy = ((y + 0.5f) - pcy) * 2.0f;
        for (int x = 0; x < w; x++) {
            float dx = (x + 0.5f) - pcx;
            float radius = hypotf(dx, dy) + 1e-3f;
            float angle = atan2f(dy, dx);
            float tu = angle / TWO_PI + spin;
            float tv = SCALE * (float)h / radius + phase;
            float val = texture(tu, tv);
            // distance fog: the throat (small radius) recedes into black
            float fog = radius / (radius + 0.30f * h);
            val *= fog;
            int r, g, bcol;
            palette(tv * 0.5f + phase * 0.1f, s, &r, &g, &bcol);
            r = (int)(r * val); g = (int)(g * val); bcol = (int)(bcol * val);
            float luma = (0.30f * r + 0.59f * g + 0.11f * bcol) / 255.0f;
            int gi = (int)(luma * NRAMP);
            if (gi >= NRAMP) gi = NRAMP - 1;
            if (gi <= 0) continue;
            termpaint_surface_write_with_colors(surface, x, y, RAMP[gi],
                    TERMPAINT_RGB_COLOR(r, g, bcol), bg);
        }
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             " space: twist  click: steer  a: %s  c: %s  +/-: speed x%.2f  q: quit ",
             autopilot ? "auto  " : "manual", s->name, speed_mul);
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
            case '=': if (speed_mul < 4.0f) speed_mul += 0.25f; break;
            case '-': if (speed_mul > 0.25f) speed_mul -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            spin_vel += 2.5f;
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        int tc = termpaint_surface_width(surface);
        int tr = termpaint_surface_height(surface);
        if (tc > 0 && tr > 0) {
            autopilot = false;
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

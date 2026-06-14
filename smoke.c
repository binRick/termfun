// Rising smoke — a density field advected through a curl-noise flow field,
// rendered as a colour glyph ramp on the cell grid.
// A divergence-free swirl derived from a summed-sine potential, plus a
// buoyancy term that lifts dense, hot cells, carries the smoke upward in
// billowing curls. A semi-Lagrangian backtrace keeps the advection stable.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space puffs a burst at the source, mouse click injects dye
// at the pointer, 'a' toggles the continuous source, 'c' cycles the dye
// colour, +/- change the buoyancy.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "termpaint.h"
#include "termpaintx.h"

#define FRAME_MS 33
#define MAX_SW 256
#define MAX_SH 256

// Dye colours: a light tint for thin smoke, a dark tint for thick smoke.
// Density picks the mix; heat lifts everything toward white at the core.
typedef struct {
    const char *name;
    int lr, lg, lb;     // thin (low density) colour
    int dr, dg, db;     // thick (high density) colour
} scheme;

static const scheme SCHEMES[] = {
    { "smoke",  200, 200, 205,   40,  40,  46 },
    { "ink",     90, 150, 235,   10,  24,  78 },
    { "blood",  225, 110, 110,   90,  10,  14 },
    { "toxic",  150, 235,  90,   18,  70,  16 },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

// Density + temperature fields on a capped grid. dens is advected each
// step; scratch is the semi-Lagrangian backtrace destination. The cell
// grid uses terminal cells x half-cell rows.
static float dens[MAX_SW * MAX_SH];
static float temp[MAX_SW * MAX_SH];
static float scratch[MAX_SW * MAX_SH];
static int sw, sh;

static bool quit_requested;
static int scheme_idx;
static bool source_on = true;
static float buoyancy = 1.0f;
static float phase;            // accumulated animation time
static int puffs;

static uint32_t rng = 0x9e3779b9;

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
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface) * 2;
    if (w > MAX_SW) w = MAX_SW;
    if (h > MAX_SH) h = MAX_SH;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (w != sw || h != sh) {
        sw = w;
        sh = h;
        memset(dens, 0, sizeof(dens));
        memset(temp, 0, sizeof(temp));
    }
}

// Inject a soft disc of density and heat at sim-grid (cx, cy).
static void inject(float cx, float cy, float radius, float amp) {
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
            int i = y * sw + x;
            dens[i] += amp * f;
            if (dens[i] > 1.4f) dens[i] = 1.4f;
            temp[i] += amp * f;
            if (temp[i] > 1.4f) temp[i] = 1.4f;
        }
    }
}

static void puff(void) {
    inject(sw * 0.5f, sh - sh / 12.0f, sw / 16.0f + 2.0f, 1.3f);
    puffs++;
}

// Scalar potential whose curl gives a divergence-free swirling field.
static float potential(float x, float y, float t) {
    return 1.30f * sinf(0.018f * x + 0.7f * t)
                 * cosf(0.022f * y - 0.5f * t)
         + 0.70f * sinf(0.040f * (x + y) + 1.1f * t)
         + 0.50f * cosf(0.055f * x - 0.030f * y + 0.9f * t);
}

// Velocity from the curl of `potential` (vx = dP/dy, vy = -dP/dx) plus a
// buoyancy lift that grows with local density+heat. Sampled at sim coords.
static void velocity(float x, float y, float t, float lift, float *vx, float *vy) {
    const float e = 1.0f;
    float dpdx = (potential(x + e, y, t) - potential(x - e, y, t)) * 0.5f;
    float dpdy = (potential(x, y + e, t) - potential(x, y - e, t)) * 0.5f;
    // scale swirl with grid size so motion looks similar at any resolution
    float s = sw / 96.0f;
    *vx = s * dpdy;
    *vy = -s * dpdx - lift;   // negative y is up
}

// Bilinear sample of `src` at fractional (x, y), clamped to the interior.
static float sample(const float *src, float x, float y) {
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (x > sw - 1.001f) x = sw - 1.001f;
    if (y > sh - 1.001f) y = sh - 1.001f;
    int x0 = (int)x, y0 = (int)y;
    float fx = x - x0, fy = y - y0;
    const float *r0 = &src[y0 * sw];
    const float *r1 = &src[(y0 + 1) * sw];
    float a = r0[x0] + (r0[x0 + 1] - r0[x0]) * fx;
    float b = r1[x0] + (r1[x0 + 1] - r1[x0]) * fx;
    return a + (b - a) * fy;
}

// Semi-Lagrangian advection of `field` by the flow, into `scratch`, then a
// light dissipation and a tiny diffuse blur for smoothness. dis is the
// per-step keep factor.
static void advect(float *field, float t, float dis) {
    for (int y = 1; y < sh - 1; y++) {
        for (int x = 1; x < sw - 1; x++) {
            float lift = buoyancy * (0.6f * dens[y * sw + x]
                                   + 0.9f * temp[y * sw + x]);
            float vx, vy;
            velocity((float)x, (float)y, t, lift, &vx, &vy);
            scratch[y * sw + x] = sample(field, x - vx, y - vy) * dis;
        }
    }
    for (int y = 1; y < sh - 1; y++) {
        float *d = &field[y * sw];
        const float *s = &scratch[y * sw];
        for (int x = 1; x < sw - 1; x++) {
            float c = s[x];
            float n = (s[x - 1] + s[x + 1] + s[x - sw] + s[x + sw]) * 0.25f;
            d[x] = c * 0.86f + n * 0.14f;
        }
    }
    // keep the borders clear so smoke fades out instead of piling on edges
    for (int x = 0; x < sw; x++) {
        field[x] = 0.0f;
        field[(sh - 1) * sw + x] = 0.0f;
    }
    for (int y = 0; y < sh; y++) {
        field[y * sw] = 0.0f;
        field[y * sw + sw - 1] = 0.0f;
    }
}

static void step(float dt) {
    sync_sim();
    phase += dt;

    if (source_on) {
        float wob = sw * 0.04f * sinf(phase * 2.3f);
        inject(sw * 0.5f + wob, sh - sh / 16.0f - 1.0f,
               sw / 22.0f + 1.5f, 0.55f + 0.25f * frand());
    }

    advect(dens, phase, 0.990f);
    advect(temp, phase, 0.965f);   // heat cools faster than smoke clears
}

// Map density + temperature to a dye colour. Thin smoke leans to the light
// tint, thick smoke to the dark tint; heat at the core glows toward white.
static void smoke_rgb(float d, float t, const scheme *sc, int *r, int *g, int *b) {
    float m = d > 1.0f ? 1.0f : d;
    int rr = (int)(sc->lr + (sc->dr - sc->lr) * m);
    int gg = (int)(sc->lg + (sc->dg - sc->lg) * m);
    int bb = (int)(sc->lb + (sc->db - sc->lb) * m);
    float glow = t * 0.6f;
    if (glow > 0.85f) glow = 0.85f;
    rr = (int)(rr + (255 - rr) * glow);
    gg = (int)(gg + (255 - gg) * glow);
    bb = (int)(bb + (255 - bb) * glow);
    *r = rr; *g = gg; *b = bb;
}

static const char *RAMP[] = { "·", ":", "░", "▒", "▓", "█" };
#define NRAMP ((int)(sizeof(RAMP) / sizeof(RAMP[0])))

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *sc = &SCHEMES[scheme_idx];
    int bg = TERMPAINT_RGB_COLOR(0, 0, 0);
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    for (int y = 1; y < h && 2 * y + 1 < sh; y++) {
        for (int x = 0; x < w && x < sw; x++) {
            float d = (dens[2 * y * sw + x] + dens[(2 * y + 1) * sw + x]) * 0.5f;
            if (d <= 0.05f) {
                continue;
            }
            float t = (temp[2 * y * sw + x] + temp[(2 * y + 1) * sw + x]) * 0.5f;
            int gi = (int)(d * NRAMP);
            if (gi >= NRAMP) gi = NRAMP - 1;
            int r, g, b;
            smoke_rgb(d, t, sc, &r, &g, &b);
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
             " space: puff  click: dye  a: source %s  c: %s  +/-: buoyancy x%.2f  q: quit │ puffs %d ",
             source_on ? "on" : "off", sc->name, buoyancy, puffs);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': source_on = !source_on; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': if (buoyancy < 3.0f) buoyancy += 0.25f; break;
            case '-': if (buoyancy > 0.0f) buoyancy -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            puff();
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
            inject((event->mouse.x + 0.5f) * sw / tc,
                   (event->mouse.y + 0.5f) * sh / tr,
                   sw / 18.0f + 2.0f, 1.2f);
            puffs++;
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
            step(FRAME_MS / 1000.0f);
            timeout = FRAME_MS;
        }
    }

    termpaint_terminal_free_with_restore(terminal);
    return 0;
}

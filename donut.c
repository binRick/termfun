// Donut — the iconic spinning shaded torus, in colour, on the cell grid.
// A torus is parametrised by theta (around the tube) and phi (around the
// centre), revolved and tumbled by two spin angles, projected with
// perspective, and shaded by the dot of the surface normal with a light.
// A per-cell z-buffer keeps the near surface in front; each visible point
// is a glyph from a luminance ramp, coloured through a cosine palette.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space tumbles (random spin impulse), mouse click spins
// toward the pointer, 'a' toggles autopilot (steady spin), 'c' cycles
// palette + tube shape, +/- change the spin speed.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "termpaint.h"
#include "termpaintx.h"

#define FRAME_MS 33

// Inigo Quilez cosine palette: colour(t) = a + b * cos(2pi (c*t + d)).
typedef struct {
    const char *name;
    float a[3], b[3], c[3], d[3];
} scheme;

static const scheme SCHEMES[] = {
    { "gold",   {0.55f,0.42f,0.18f}, {0.45f,0.40f,0.20f}, {1.0f,1.0f,1.0f}, {0.00f,0.10f,0.20f} },
    { "neon",   {0.50f,0.40f,0.55f}, {0.50f,0.45f,0.45f}, {1.0f,1.0f,1.0f}, {0.50f,0.20f,0.85f} },
    { "ember",  {0.50f,0.30f,0.15f}, {0.50f,0.32f,0.20f}, {1.0f,1.0f,1.0f}, {0.00f,0.12f,0.20f} },
    { "ice",    {0.40f,0.50f,0.60f}, {0.35f,0.40f,0.40f}, {1.0f,1.0f,1.0f}, {0.60f,0.45f,0.30f} },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

// Tube/centre radius pairs — 'c' cycles through fatter and thinner donuts.
typedef struct { const char *name; float r1, r2; } shape;
static const shape SHAPES[] = {
    { "classic", 1.0f, 2.0f },
    { "fat",     1.2f, 1.8f },
    { "thin",    0.7f, 2.2f },
};
#define NSHAPES ((int)(sizeof(SHAPES) / sizeof(SHAPES[0])))

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static bool quit_requested;
static int scheme_idx;
static int shape_idx;
static bool autopilot = true;
static float spin_mul = 1.0f;
static float A, B;             // the two spin angles
static float vA = 0.55f, vB = 0.32f;   // spin velocities (rad/s base)

// Per-cell z-buffer (1/z, larger is nearer); capped to a static maximum.
#define ZCOLS 512
#define ZROWS 512
#define ZMAX (ZCOLS * ZROWS)
static float zbuf[ZMAX];

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

static void step(float dt) {
    if (autopilot) {
        A += vA * spin_mul * dt;
        B += vB * spin_mul * dt;
    }
}

// Brightness ramp, darkest to brightest (Andy Sloane's classic ".,-~:;=!*#$@").
static const char *RAMP = ".,-~:;=!*#$@";
#define NRAMP 12

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *s = &SCHEMES[scheme_idx];
    const shape *sh = &SHAPES[shape_idx];
    int bg = TERMPAINT_RGB_COLOR(0, 0, 0);
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    if (w > ZCOLS) w = ZCOLS;
    if (h > ZROWS) h = ZROWS;
    for (int i = 0; i < w * h; i++) {
        zbuf[i] = 0.0f;
    }

    // precompute the spin rotation
    float cA = cosf(A), sA = sinf(A);
    float cB = cosf(B), sB = sinf(B);

    // perspective: K2 is the viewer distance, K1 scales to the screen
    float R1 = sh->r1, R2 = sh->r2;
    float K2 = 5.0f;
    float K1 = w * K2 * 0.30f / (R1 + R2);

    // light from the upper-left-front
    float lx = -0.5f, ly = 0.5f, lz = -1.0f;
    float ln = 1.0f / sqrtf(lx * lx + ly * ly + lz * lz);
    lx *= ln; ly *= ln; lz *= ln;

    // step finely enough to fill the cells without gaps
    for (float phi = 0; phi < 6.2831853f; phi += 0.05f) {
        float cphi = cosf(phi), sphi = sinf(phi);
        for (float theta = 0; theta < 6.2831853f; theta += 0.015f) {
            float ct = cosf(theta), st = sinf(theta);

            // point on the tube circle, then revolve about y by phi
            float cx = R2 + R1 * ct;
            float ox = cx * cphi;
            float oy = R1 * st;
            float oz = -cx * sphi;

            // surface normal before tumble
            float nx0 = ct * cphi;
            float ny0 = st;
            float nz0 = -ct * sphi;

            // tumble by A (about x) then B (about z)
            float x1 = ox;
            float y1 = oy * cA - oz * sA;
            float z1 = oy * sA + oz * cA;
            float x = x1 * cB - y1 * sB;
            float y = x1 * sB + y1 * cB;
            float z = z1 + K2 + R2 + R1;

            float nx1 = nx0;
            float ny1 = ny0 * cA - nz0 * sA;
            float nz1 = ny0 * sA + nz0 * cA;
            float nx = nx1 * cB - ny1 * sB;
            float ny = nx1 * sB + ny1 * cB;
            float nz = nz1;

            float ooz = 1.0f / z;
            // cells are ~2:1 (taller than wide), so halve the y projection
            int sx = (int)(w / 2.0f + K1 * ooz * x);
            int sy = (int)(h / 2.0f - K1 * 0.5f * ooz * y);
            if (sx < 0 || sx >= w || sy < 1 || sy >= h) {
                continue;
            }

            int idx = sy * w + sx;
            if (ooz <= zbuf[idx]) {
                continue;
            }
            zbuf[idx] = ooz;

            float L = nx * lx + ny * ly + nz * lz;
            if (L < 0) L = 0;
            // ambient + diffuse so back faces stay dim but visible
            float lum = 0.15f + 0.85f * L;
            if (lum > 1.0f) lum = 1.0f;

            int gi = (int)(lum * NRAMP);
            if (gi >= NRAMP) gi = NRAMP - 1;
            if (gi < 0) gi = 0;
            char glyph[2] = { RAMP[gi], 0 };

            int r, g, b;
            palette(0.30f + lum * 0.55f, s, &r, &g, &b);
            // specular toward white at the bright end
            if (lum > 0.85f) {
                float hot = (lum - 0.85f) / 0.15f;
                r += (int)((255 - r) * hot * 0.7f);
                g += (int)((255 - g) * hot * 0.7f);
                b += (int)((255 - b) * hot * 0.7f);
                r = clamp255(r); g = clamp255(g); b = clamp255(b);
            }
            termpaint_surface_write_with_colors(surface, sx, sy, glyph,
                    TERMPAINT_RGB_COLOR(r, g, b), bg);
        }
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             " space: tumble  click: spin  a: %s  c: %s/%s  +/-: speed x%.2f  q: quit ",
             autopilot ? "auto " : "manual", s->name, sh->name, spin_mul);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': autopilot = !autopilot; break;
            case 'c':
                scheme_idx = (scheme_idx + 1) % NSCHEMES;
                shape_idx = (shape_idx + 1) % NSHAPES;
                break;
            case '+':
            case '=': if (spin_mul < 4.0f) spin_mul += 0.25f; break;
            case '-': if (spin_mul > 0.25f) spin_mul -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            // random spin impulse on both axes
            autopilot = true;
            vA = (frand() * 2.0f - 1.0f) * 1.6f;
            vB = (frand() * 2.0f - 1.0f) * 1.6f;
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        int tc = termpaint_surface_width(surface);
        int tr = termpaint_surface_height(surface);
        if (tc > 0 && tr > 0) {
            // spin toward the pointer: velocity from offset off screen centre
            autopilot = true;
            float ox = (event->mouse.x + 0.5f) / tc - 0.5f;
            float oy = (event->mouse.y + 0.5f) / tr - 0.5f;
            vB = ox * 4.0f;
            vA = -oy * 4.0f;
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

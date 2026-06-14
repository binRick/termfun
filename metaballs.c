// Metaballs — a lava-lamp swarm of soft blobs for the terminal.
// Each blob contributes r^2 / (dist^2 + eps) to a scalar field; where the
// field crosses a threshold the goo "skins over", and overlapping blobs
// merge into one smooth surface. The surface is shaded through a cosine
// palette and drawn as a colour glyph ramp on the cell grid.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space adds a blob with random velocity, mouse click spawns a
// blob at the pointer, 'a' toggles gravity (blobs gently rise/sink vs. free
// bounce), 'c' cycles the palette, +/- change the blob count.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "termpaint.h"
#include "termpaintx.h"

#define FRAME_MS 33
#define MAX_BLOBS 16
#define MIN_BLOBS 3
#define FIELD_EPS 0.0006f   // softens the 1/dist^2 singularity at a blob centre
#define THRESH 1.0f         // field value where the goo surface skins over

// Inigo Quilez cosine palette: colour(t) = a + b * cos(2pi (c*t + d)).
typedef struct {
    const char *name;
    float a[3], b[3], c[3], d[3];
} scheme;

static const scheme SCHEMES[] = {
    { "lava",    {0.55f,0.18f,0.10f}, {0.45f,0.30f,0.12f}, {1.0f,1.0f,1.0f}, {0.00f,0.10f,0.20f} },
    { "ooze",    {0.18f,0.50f,0.20f}, {0.16f,0.45f,0.20f}, {1.0f,1.0f,1.0f}, {0.30f,0.00f,0.40f} },
    { "plasma",  {0.30f,0.20f,0.55f}, {0.30f,0.25f,0.45f}, {1.0f,1.0f,1.0f}, {0.55f,0.30f,0.10f} },
    { "mercury", {0.55f,0.57f,0.62f}, {0.42f,0.42f,0.44f}, {1.0f,1.0f,1.0f}, {0.00f,0.05f,0.12f} },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

typedef struct {
    float x, y;     // position, normalised [0,1]
    float vx, vy;   // velocity, normalised units per second
    float r;        // radius, normalised
} blob;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static blob blobs[MAX_BLOBS];
static int nblobs;
static bool quit_requested;
static int scheme_idx;
static bool gravity;            // false: free bounce; true: gentle buoyancy
static float phase;             // accumulated animation time (palette cycling)

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

static void add_blob(float x, float y, bool random_vel) {
    if (nblobs >= MAX_BLOBS) {
        return;
    }
    blob *b = &blobs[nblobs++];
    b->x = x;
    b->y = y;
    b->r = 0.10f + frand() * 0.07f;
    if (random_vel) {
        float ang = frand() * 2 * (float)M_PI;
        float sp = 0.05f + frand() * 0.10f;
        b->vx = cosf(ang) * sp;
        b->vy = sinf(ang) * sp;
    } else {
        b->vx = (frand() - 0.5f) * 0.06f;
        b->vy = (frand() - 0.5f) * 0.06f;
    }
}

// Field at normalised (u, v); dx is aspect-corrected by `ar` so the blobs
// look round on wide cell grids. Returns the summed metaball potential.
static float field(float u, float v, float ar) {
    float f = 0.0f;
    for (int i = 0; i < nblobs; i++) {
        float dx = (u - blobs[i].x) * ar;
        float dy = (v - blobs[i].y);
        float d2 = dx * dx + dy * dy + FIELD_EPS;
        f += blobs[i].r * blobs[i].r / d2;
    }
    return f;
}

static void step(float dt) {
    phase += dt;
    for (int i = 0; i < nblobs; i++) {
        blob *b = &blobs[i];
        if (gravity) {
            // gentle buoyancy: slow vertical drift with a wobble, free in x
            b->vy += sinf(phase * 0.7f + i) * 0.02f * dt;
            b->vy -= 0.015f * dt;   // a touch of lift so they tend to rise
        }
        b->x += b->vx * dt;
        b->y += b->vy * dt;
        // bounce off the edges, keeping the centre just inside
        if (b->x < 0.04f) { b->x = 0.04f; b->vx = fabsf(b->vx); }
        if (b->x > 0.96f) { b->x = 0.96f; b->vx = -fabsf(b->vx); }
        if (b->y < 0.04f) { b->y = 0.04f; b->vy = fabsf(b->vy); }
        if (b->y > 0.96f) { b->y = 0.96f; b->vy = -fabsf(b->vy); }
    }
}

// Surface ramp: sparse rim glyphs near the threshold, solid core for the
// deep goo.
static const char *RAMP[] = { "·", ":", "+", "*", "o", "O", "@", "█" };
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
            float fval = field(u, v, ar);
            if (fval < THRESH) {
                continue;   // outside the goo stays background
            }
            float t = (fval - THRESH) / THRESH;
            int gi = (int)(t * NRAMP);
            if (gi >= NRAMP) gi = NRAMP - 1;
            int r, g, b;
            palette(t * 0.30f + phase * 0.05f, s, &r, &g, &b);
            float rim = t < 0.35f ? 1.0f - t / 0.35f : 0.0f;   // bright edge
            r = clamp255(r + (int)(rim * (255 - r)));
            g = clamp255(g + (int)(rim * (255 - g)));
            b = clamp255(b + (int)(rim * (255 - b)));
            termpaint_surface_write_with_colors(surface, x, y, RAMP[gi],
                    TERMPAINT_RGB_COLOR(r, g, b), bg);
        }
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             " space: blob  click: blob  a: gravity %s  c: %s  +/-: blobs %d  q: quit ",
             gravity ? "ON " : "off", s->name, nblobs);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': gravity = !gravity; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': add_blob(0.2f + frand() * 0.6f, 0.2f + frand() * 0.6f, true); break;
            case '-': if (nblobs > MIN_BLOBS) nblobs--; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            add_blob(0.2f + frand() * 0.6f, 0.2f + frand() * 0.6f, true);
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        // mouse positions are in cell coordinates; map onto normalised space
        int tc = termpaint_surface_width(surface);
        int tr = termpaint_surface_height(surface);
        if (tc > 0 && tr > 0) {
            add_blob((event->mouse.x + 0.5f) / tc,
                     (event->mouse.y + 0.5f) / tr, true);
        }
    }
}

static void init_blobs(void) {
    nblobs = 0;
    int n = 8;
    for (int i = 0; i < n; i++) {
        add_blob(0.2f + frand() * 0.6f, 0.2f + frand() * 0.6f, false);
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

    init_blobs();

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

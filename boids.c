// Boids — Reynolds flocking swarm for the terminal.
// A swarm of boids obeying the three classic rules — separation, alignment,
// and cohesion — emerges into swirling, organic flocking motion, drawn as
// directional glyphs coloured by speed on a dark cell grid.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space scatters the flock outward, mouse click sets a temporary
// attractor at the pointer, 'a' toggles a roaming predator the boids flee,
// 'c' cycles the colour scheme, +/- change the flock size.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "termpaint.h"
#include "termpaintx.h"

#define FRAME_MS 33
#define MAX_BOIDS 400
#define MIN_BOIDS 40

// Flocking rule weights and ranges, in view-space units.
#define VIS_RADIUS   8.0f    // neighbour vision radius
#define SEP_RADIUS   3.0f    // separation kicks in inside this
#define W_SEP        1.6f
#define W_ALIGN      1.0f
#define W_COHESION   0.9f
#define W_ATTRACT    2.2f
#define W_FLEE       4.0f
#define MAX_SPEED    26.0f
#define MIN_SPEED    10.0f
#define MAX_FORCE    60.0f
#define EDGE_MARGIN  6.0f
#define EDGE_TURN    40.0f

typedef struct {
    const char *name;
    int sr, sg, sb;     // slow boid colour
    int fr, fg, fb_;    // fast boid colour
} scheme;

static const scheme SCHEMES[] = {
    { "aurora",  40, 120,  90,   200, 255, 230 },
    { "ember",  120,  50,  20,   255, 220, 120 },
    { "ion",     40,  80, 150,   170, 230, 255 },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

typedef struct {
    float x, y;     // view-space position
    float vx, vy;   // view-space velocity
} boid;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

// The view: terminal cells wide, half-cell rows tall (so the boids move on a
// roughly square grid, like the fireworks particles).
static int vw, vh;
static int nboids = 200;
static boid boids[MAX_BOIDS];

static bool quit_requested;
static int scheme_idx;

static bool predator_on;
static float pred_ph;
static float pred_x, pred_y;

static bool attract_on;
static float attract_x, attract_y;
static float attract_life;   // seconds remaining

static uint32_t rng = 0x1234567u;

static inline uint32_t xr(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static float frand(void) {
    return (float)(xr() & 0xffffff) / (float)0xffffff;
}

static void spawn(boid *b) {
    b->x = frand() * vw;
    b->y = frand() * vh;
    float ang = frand() * 2.0f * (float)M_PI;
    float sp = MIN_SPEED + frand() * (MAX_SPEED - MIN_SPEED);
    b->vx = cosf(ang) * sp;
    b->vy = sinf(ang) * sp;
}

static void sync_view(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface) * 2;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (w != vw || h != vh) {
        bool first = (vw == 0);
        vw = w;
        vh = h;
        pred_x = vw * 0.5f;
        pred_y = vh * 0.5f;
        if (first) {
            for (int i = 0; i < nboids; i++) {
                spawn(&boids[i]);
            }
        }
    }
}

static void limit(float *vx, float *vy, float max) {
    float m2 = (*vx) * (*vx) + (*vy) * (*vy);
    if (m2 > max * max) {
        float m = sqrtf(m2);
        *vx = *vx / m * max;
        *vy = *vy / m * max;
    }
}

static void step(float dt) {
    sync_view();

    if (predator_on) {
        pred_ph += dt;
        // Lissajous roam across the view
        pred_x = vw * (0.5f + 0.42f * sinf(pred_ph * 0.70f));
        pred_y = vh * (0.5f + 0.40f * sinf(pred_ph * 0.93f + 1.3f));
    }
    if (attract_on) {
        attract_life -= dt;
        if (attract_life <= 0) {
            attract_on = false;
        }
    }

    for (int i = 0; i < nboids; i++) {
        boid *b = &boids[i];

        float sepx = 0, sepy = 0;
        float alx = 0, aly = 0;
        float cohx = 0, cohy = 0;
        int n_align = 0, n_coh = 0;

        for (int j = 0; j < nboids; j++) {
            if (j == i) {
                continue;
            }
            float dx = boids[j].x - b->x;
            float dy = boids[j].y - b->y;
            float d2 = dx * dx + dy * dy;
            if (d2 > VIS_RADIUS * VIS_RADIUS || d2 <= 0.0001f) {
                continue;
            }
            float d = sqrtf(d2);
            if (d < SEP_RADIUS) {
                // steer away, weighted by closeness
                sepx -= dx / d * (SEP_RADIUS - d);
                sepy -= dy / d * (SEP_RADIUS - d);
            }
            alx += boids[j].vx;
            aly += boids[j].vy;
            n_align++;
            cohx += boids[j].x;
            cohy += boids[j].y;
            n_coh++;
        }

        float ax = 0, ay = 0;
        ax += sepx * W_SEP;
        ay += sepy * W_SEP;

        if (n_align > 0) {
            // match average heading
            alx /= n_align;
            aly /= n_align;
            ax += (alx - b->vx) * W_ALIGN;
            ay += (aly - b->vy) * W_ALIGN;
        }
        if (n_coh > 0) {
            // steer toward neighbour centre of mass
            cohx = cohx / n_coh - b->x;
            cohy = cohy / n_coh - b->y;
            ax += cohx * W_COHESION;
            ay += cohy * W_COHESION;
        }

        if (attract_on) {
            float dx = attract_x - b->x;
            float dy = attract_y - b->y;
            float d = sqrtf(dx * dx + dy * dy) + 0.001f;
            ax += dx / d * W_ATTRACT * MAX_SPEED;
            ay += dy / d * W_ATTRACT * MAX_SPEED;
        }

        if (predator_on) {
            float dx = b->x - pred_x;
            float dy = b->y - pred_y;
            float d2 = dx * dx + dy * dy;
            float flee_r = VIS_RADIUS * 2.0f;
            if (d2 < flee_r * flee_r && d2 > 0.0001f) {
                float d = sqrtf(d2);
                float f = (flee_r - d) / flee_r;
                ax += dx / d * W_FLEE * MAX_SPEED * f;
                ay += dy / d * W_FLEE * MAX_SPEED * f;
            }
        }

        // soft edge steering: turn back in before hitting the border
        if (b->x < EDGE_MARGIN)        ax += EDGE_TURN;
        else if (b->x > vw - EDGE_MARGIN) ax -= EDGE_TURN;
        if (b->y < EDGE_MARGIN)        ay += EDGE_TURN;
        else if (b->y > vh - EDGE_MARGIN) ay -= EDGE_TURN;

        limit(&ax, &ay, MAX_FORCE);
        b->vx += ax * dt;
        b->vy += ay * dt;

        // speed clamp: stay between min and max so they keep moving
        limit(&b->vx, &b->vy, MAX_SPEED);
        float sp2 = b->vx * b->vx + b->vy * b->vy;
        if (sp2 < MIN_SPEED * MIN_SPEED && sp2 > 0.0001f) {
            float sp = sqrtf(sp2);
            b->vx = b->vx / sp * MIN_SPEED;
            b->vy = b->vy / sp * MIN_SPEED;
        }

        b->x += b->vx * dt;
        b->y += b->vy * dt;

        // hard clamp as a safety net against the soft edge force
        if (b->x < 0)        { b->x = 0;        if (b->vx < 0) b->vx = -b->vx; }
        if (b->x > vw - 1)   { b->x = vw - 1;   if (b->vx > 0) b->vx = -b->vx; }
        if (b->y < 0)        { b->y = 0;        if (b->vy < 0) b->vy = -b->vy; }
        if (b->y > vh - 1)   { b->y = vh - 1;   if (b->vy > 0) b->vy = -b->vy; }
    }
}

static void scatter(void) {
    // outward impulse from the flock centre
    float cx = 0, cy = 0;
    for (int i = 0; i < nboids; i++) {
        cx += boids[i].x;
        cy += boids[i].y;
    }
    cx /= nboids;
    cy /= nboids;
    for (int i = 0; i < nboids; i++) {
        boid *b = &boids[i];
        float dx = b->x - cx;
        float dy = b->y - cy;
        float d = sqrtf(dx * dx + dy * dy) + 0.001f;
        b->vx += dx / d * MAX_SPEED * 1.5f;
        b->vy += dy / d * MAX_SPEED * 1.5f;
    }
}

// 8-direction glyph chosen from heading.
static const char *DIR_GLYPHS[8] = {
    "→", "↘", "↓", "↙", "←", "↖", "↑", "↗"
};

static const char *dir_glyph(float vx, float vy) {
    float ang = atan2f(vy, vx);   // -pi..pi
    int sect = (int)floorf((ang / (2.0f * (float)M_PI) + 1.0f) * 8.0f + 0.5f);
    sect &= 7;
    return DIR_GLYPHS[sect];
}

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *s = &SCHEMES[scheme_idx];
    int bg = TERMPAINT_RGB_COLOR(6, 8, 14);
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    for (int i = 0; i < nboids; i++) {
        boid *b = &boids[i];
        int x = (int)b->x;
        int y = (int)(b->y / 2.0f);
        if (x < 0 || x >= w || y < 1 || y >= h) {
            continue;
        }
        float sp = sqrtf(b->vx * b->vx + b->vy * b->vy);
        float t = (sp - MIN_SPEED) / (MAX_SPEED - MIN_SPEED);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        int r = (int)(s->sr + (s->fr - s->sr) * t);
        int g = (int)(s->sg + (s->fg - s->sg) * t);
        int bl = (int)(s->sb + (s->fb_ - s->sb) * t);
        termpaint_surface_write_with_colors(surface, x, y, dir_glyph(b->vx, b->vy),
                TERMPAINT_RGB_COLOR(r, g, bl), bg);
    }

    // predator marker
    if (predator_on) {
        int px = (int)pred_x;
        int py = (int)(pred_y / 2.0f);
        if (px >= 0 && px < w && py >= 1 && py < h) {
            termpaint_surface_write_with_colors(surface, px, py, "✦",
                    TERMPAINT_RGB_COLOR(255, 70, 70), bg);
        }
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             " space: scatter  click: attract  a: predator %s  c: %s  +/-: boids %d  q: quit ",
             predator_on ? "ON " : "off", s->name, nboids);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': predator_on = !predator_on; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=':
                if (nboids < MAX_BOIDS) {
                    int prev = nboids;
                    nboids += 20;
                    if (nboids > MAX_BOIDS) nboids = MAX_BOIDS;
                    for (int i = prev; i < nboids; i++) {
                        spawn(&boids[i]);
                    }
                }
                break;
            case '-':
                if (nboids > MIN_BOIDS) {
                    nboids -= 20;
                    if (nboids < MIN_BOIDS) nboids = MIN_BOIDS;
                }
                break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            scatter();
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        // mouse positions are in cell coordinates; map onto the view
        int tc = termpaint_surface_width(surface);
        int tr = termpaint_surface_height(surface);
        if (tc > 0 && tr > 0) {
            attract_x = (event->mouse.x + 0.5f) * vw / tc;
            attract_y = (event->mouse.y + 0.5f) * vh / tr;
            attract_on = true;
            attract_life = 3.0f;
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

    sync_view();

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

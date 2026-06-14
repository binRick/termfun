// Boids at pixel resolution — termpaint + kitty graphics protocol.
// A swarm of boids obeying the three classic Reynolds rules — separation,
// alignment, and cohesion — emerges into swirling, organic flocking motion.
// Each boid draws an additive glow into a framebuffer that decays instead of
// clearing, so the swarm leaves fading motion trails. Falls back to
// directional-glyph cell rendering in terminals without graphics support.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space scatters the flock outward, mouse click sets a temporary
// attractor at the pointer, 'a' toggles a roaming predator the boids flee,
// 'c' cycles the colour scheme, +/- change the flock size.

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "termpaint.h"
#include "termpaintx.h"

#include "kitty_gfx.h"

#define FRAME_MS 50     // 20fps — kitty RGBA frames are large; tune with BOIDS_FPS
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

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

// The view: pixel grid in pixel mode, terminal cells x half-cell rows in
// cell mode (so the boids move on a roughly square grid either way).
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
    int w, h;
    if (pixel_mode) {
        w = kctx.fb_w;
        h = kctx.fb_h;
    } else {
        w = termpaint_surface_width(surface);
        h = termpaint_surface_height(surface) * 2;
    }
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
        } else {
            // rescale positions so the flock stays in view after a resize
            for (int i = 0; i < nboids; i++) {
                if (boids[i].x > w) boids[i].x = frand() * w;
                if (boids[i].y > h) boids[i].y = frand() * h;
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

// Map a heading to a hue (0..359) so colour reflects direction of travel.
static void hue_to_rgb(int hue, float v, int *r, int *g, int *b) {
    float h = (hue % 360) / 60.0f;
    float x = (1 - fabsf(fmodf(h, 2) - 1));
    float rf = 0, gf = 0, bf = 0;
    if (h < 1)      { rf = 1; gf = x; }
    else if (h < 2) { rf = x; gf = 1; }
    else if (h < 3) { gf = 1; bf = x; }
    else if (h < 4) { gf = x; bf = 1; }
    else if (h < 5) { rf = x; bf = 1; }
    else            { rf = 1; bf = x; }
    *r = (int)(255 * v * rf);
    *g = (int)(255 * v * gf);
    *b = (int)(255 * v * bf);
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// The framebuffer decays toward zero instead of clearing, so each boid's
// glow from previous frames fades into a soft motion trail. Boids float over
// the terminal background: empty pixels stay transparent.

static void px_glow(uint8_t *fb, int W, int H, float fx, float fy,
                    float radius, int r, int g, int b) {
    int ir = (int)radius + 1;
    int cx_ = (int)fx, cy_ = (int)fy;
    for (int dy = -ir; dy <= ir; dy++) {
        int y = cy_ + dy;
        if (y < 0 || y >= H) {
            continue;
        }
        for (int dx = -ir; dx <= ir; dx++) {
            int x = cx_ + dx;
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
            // alpha composites against the terminal background; saturate fast
            // so overlapping glows don't double-dim
            int lum = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int af = (int)(lum * f * 3.0f);
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
    const scheme *s = &SCHEMES[scheme_idx];

    // decay, don't clear: this is what turns motion into trails
    size_t n = (size_t)W * H * 4;
    for (size_t i = 0; i < n; i++) {
        fb[i] = (uint8_t)((fb[i] * 205) >> 8);
    }

    // hue range walks with the scheme so each palette looks distinct
    int hue_base = scheme_idx == 0 ? 120 : scheme_idx == 1 ? 20 : 200;

    for (int i = 0; i < nboids; i++) {
        boid *b = &boids[i];
        float sp = sqrtf(b->vx * b->vx + b->vy * b->vy);
        float t = (sp - MIN_SPEED) / (MAX_SPEED - MIN_SPEED);
        if (t < 0) t = 0;
        if (t > 1) t = 1;

        // colour by heading, tinted toward the scheme's hue band
        float ang = atan2f(b->vy, b->vx);          // -pi..pi
        int hue = hue_base + (int)(ang / (float)M_PI * 60.0f);
        int r, g, bl;
        hue_to_rgb(hue, 0.6f + 0.4f * t, &r, &g, &bl);
        // blend toward the scheme's speed colour so each palette stays legible
        int sr = (int)(s->sr + (s->fr  - s->sr) * t);
        int sg = (int)(s->sg + (s->fg  - s->sg) * t);
        int sb = (int)(s->sb + (s->fb_ - s->sb) * t);
        r  = (r  + sr) / 2;
        g  = (g  + sg) / 2;
        bl = (bl + sb) / 2;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (bl > 255) bl = 255;

        float radius = 1.4f + t * 1.6f;
        px_glow(fb, W, H, b->x, b->y, radius, r, g, bl);
    }

    // predator: an angry red glow
    if (predator_on) {
        px_glow(fb, W, H, pred_x, pred_y, 5.0f, 255, 40, 40);
    }
    // attractor: a soft beacon while it lives
    if (attract_on) {
        float a = attract_life / 3.0f;
        px_glow(fb, W, H, attract_x, attract_y, 4.0f,
                (int)(200 * a), (int)(200 * a), (int)(120 * a));
    }
}

// ---- cell renderer / overlay ----

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
    char buf[160];

    if (pixel_mode) {
        // the image supplies the swarm; cells only carry the status bar
        termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR,
                                TERMPAINT_DEFAULT_COLOR);
        snprintf(buf, sizeof(buf),
                 " space: scatter  click: attract  a: predator %s  c: %s  +/-: boids %d  q: quit │ kitty gfx %dx%d ",
                 predator_on ? "ON " : "off", s->name, nboids,
                 kctx.fb_w, kctx.fb_h);
        termpaint_surface_write_with_colors(surface, 0, 0, buf,
                TERMPAINT_RGB_COLOR(180, 180, 200), TERMPAINT_DEFAULT_COLOR);
        return;
    }

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
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("BOIDS_CELLS")) {
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
        int max_dim = 512;  // ~1MB RGBA per frame; use BOIDS_MAXDIM=1024 for sharper
        const char *s = getenv("BOIDS_MAXDIM");
        if (s && atoi(s) >= 128) {
            max_dim = atoi(s);
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    sync_view();

    int frame_ms = FRAME_MS;
    const char *fps_env = getenv("BOIDS_FPS");
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

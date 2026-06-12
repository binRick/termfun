// Fireworks at pixel resolution — termpaint + kitty graphics protocol.
// Falls back to cell rendering in terminals without graphics support.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space launches a rocket, mouse click launches one at the
// pointer, 'a' toggles the auto show, +/- change the auto launch rate.

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

#define MAX_PARTICLES 4096
#define MAX_STARS 256
#define FRAME_MS 33
#define GRAVITY 12.0f

typedef enum { P_NONE, P_ROCKET, P_SPARK } ptype;

typedef struct {
    ptype type;
    float x, y;    // position in cell coordinates (y has 2x resolution, see below)
    float vx, vy;
    float life;    // seconds remaining
    float max_life;
    int hue;       // 0..359
    int burst;     // sparks spawned on rocket detonation
} particle;

typedef struct {
    int x, y;
    int phase;
} star;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

static particle particles[MAX_PARTICLES];
static star stars[MAX_STARS];
static int nstars;
static bool quit_requested;
static bool auto_show = true;
static int auto_rate = 2;     // average launches per second-ish
static int tick;
static int launched, exploded;

static float frand(void) {
    return (float)rand() / (float)RAND_MAX;
}

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
    // fade towards white at full brightness for a hot "flash" look
    float w = v > 0.85f ? (v - 0.85f) / 0.15f * 0.6f : 0;
    *r = (int)(255 * v * (rf + (1 - rf) * w));
    *g = (int)(255 * v * (gf + (1 - gf) * w));
    *b = (int)(255 * v * (bf + (1 - bf) * w));
}

static particle *alloc_particle(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].type == P_NONE) {
            return &particles[i];
        }
    }
    return NULL;
}

static void launch_rocket(int target_x) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    particle *p = alloc_particle();
    if (!p) {
        return;
    }
    p->type = P_ROCKET;
    p->x = target_x >= 0 ? (float)target_x : 4 + frand() * (w - 8);
    p->y = 2.0f * h;             // y is in half-cell units for smoother motion
    p->vx = (frand() - 0.5f) * 3;
    p->vy = -(14 + frand() * 10);
    p->max_life = p->life = 1.0f + frand() * 1.2f;
    p->hue = rand() % 360;
    p->burst = 30 + rand() % 50;
    launched++;
}

static void explode(particle *rocket) {
    float speed_base = 6 + frand() * 8;
    bool two_tone = frand() < 0.3f;
    int hue2 = (rocket->hue + 120 + rand() % 120) % 360;
    for (int i = 0; i < rocket->burst; i++) {
        particle *p = alloc_particle();
        if (!p) {
            break;
        }
        float ang = frand() * 2 * (float)M_PI;
        float speed = speed_base * (0.3f + 0.7f * frand());
        p->type = P_SPARK;
        p->x = rocket->x;
        p->y = rocket->y;
        p->vx = cosf(ang) * speed * 2;   // cells are ~2x taller than wide
        p->vy = sinf(ang) * speed;
        p->max_life = p->life = 0.8f + frand() * 1.4f;
        p->hue = (two_tone && i % 2) ? hue2 : rocket->hue;
        p->burst = 0;
    }
    exploded++;
}

static void step(float dt) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particle *p = &particles[i];
        if (p->type == P_NONE) {
            continue;
        }
        p->life -= dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vy += GRAVITY * dt * (p->type == P_ROCKET ? 0.3f : 1.0f);
        if (p->type == P_ROCKET && (p->life <= 0 || p->vy > -2)) {
            explode(p);
            p->type = P_NONE;
            continue;
        }
        if (p->life <= 0 || p->x < 0 || p->x >= w || p->y >= 2 * h) {
            p->type = P_NONE;
        }
    }

    if (auto_show && rand() % (30 / (auto_rate > 0 ? auto_rate : 1) + 1) == 0) {
        launch_rocket(-1);
    }
    tick++;
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// Each pixel is RGBA. The image is placed at z=1 (on top of cells).
// Transparent (alpha=0) pixels reveal the terminal content below —
// the status bar at row 0 shows through the empty sky, and the terminal's
// own background colour forms the night sky.

// Additive colour blend clamped to 255; alpha uses max() to stay opaque
// once a pixel is lit without double-counting overlapping glows.
static void px_glow(uint8_t *fb, int W, int H, float fx, float fy,
                    float radius, int r, int g, int b) {
    int ir = (int)radius + 1;
    int cx = (int)fx, cy = (int)fy;
    for (int dy = -ir; dy <= ir; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= H) {
            continue;
        }
        for (int dx = -ir; dx <= ir; dx++) {
            int x = cx + dx;
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
            // alpha = max(current, glow intensity) so overlapping glows
            // don't push alpha above what the brightest one alone would give
            uint8_t a = (uint8_t)((r + g + b) * f / 3);
            if (px[3] < a) {
                px[3] = a;
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
    int cols = kctx.cols, rows = kctx.rows;
    float sx = (float)W / cols;        // pixels per cell, x
    float sy = (float)H / (rows * 2); // pixels per half-cell (particle y unit)

    // Decay all channels toward 0: old glow trails fade to transparency,
    // revealing the terminal background (the "sky") as they die.
    size_t total = (size_t)W * H * 4;
    for (size_t i = 0; i < total; i++) {
        fb[i] = (uint8_t)((fb[i] * 205) >> 8);
    }

    // Twinkling stars — redrawn every frame so they don't decay away.
    for (int i = 0; i < nstars; i++) {
        star *s = &stars[i];
        if (s->x >= cols || s->y >= rows - 2) {
            continue;
        }
        int tw = ((tick / 4) + s->phase) % 8;
        int v  = tw < 4 ? 60 + tw * 30 : 60 + (7 - tw) * 30;
        float fx = (s->x + 0.5f) * sx;
        float fy = (s->y * 2 + 1.0f) * sy;
        px_glow(fb, W, H, fx, fy, tw == 3 ? 2.2f : 1.3f, v / 3, v / 3, v / 2);
    }

    // Skyline — solid opaque buildings redrawn every frame.
    for (int cx = 0; cx < cols; cx++) {
        int hh = 1 + ((cx * 7919 / 13) % 3);
        int x0 = (int)(cx * sx);
        int x1 = (int)((cx + 1) * sx);
        int y0 = (int)((rows - hh) * 2 * sy);
        for (int y = y0; y < H; y++) {
            for (int x = x0; x < x1 && x < W; x++) {
                uint8_t *px = &fb[((size_t)y * W + x) * 4];
                px[0] = 14; px[1] = 14; px[2] = 28; px[3] = 255;
            }
        }
        // glowing windows
        for (int cy = rows - hh; cy < rows; cy++) {
            if (((cx * 31 + cy * 17) % 11) != 0) {
                continue;
            }
            float wx = (cx + 0.5f) * sx;
            float wy = (cy + 0.5f) * 2.0f * sy;
            px_glow(fb, W, H, wx, wy, sx * 0.35f + 1.0f, 220, 175, 70);
        }
    }

    // Particles — additive glow; trails from decay give a natural fade.
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particle *p = &particles[i];
        if (p->type == P_NONE) {
            continue;
        }
        float fx = p->x * sx;
        float fy = p->y * sy;
        int r, g, b;
        if (p->type == P_ROCKET) {
            hue_to_rgb(p->hue, 1.0f, &r, &g, &b);
            px_glow(fb, W, H, fx, fy, sx * 0.35f + 2.0f, r, g, b);
            // exhaust flame
            px_glow(fb, W, H, fx - p->vx * sx * 0.06f, fy + 3.5f * sy,
                    sx * 0.25f + 1.5f, 255, 180, 55);
        } else {
            float v = p->life / p->max_life;
            hue_to_rgb(p->hue, v, &r, &g, &b);
            float radius = (0.2f + 0.6f * v) * sx + 1.0f;
            px_glow(fb, W, H, fx, fy, radius, r, g, b);
        }
    }
}

// ---- cell renderer / overlay ----

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    int night = TERMPAINT_RGB_COLOR(5, 5, 16);

    if (pixel_mode) {
        // the image supplies the scene; cells only carry the status bar
        termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR,
                                TERMPAINT_DEFAULT_COLOR);
        char buf[160];
        snprintf(buf, sizeof(buf),
                 " space/click: launch  a: auto %s  +/-: rate %d  q: quit │ kitty gfx %dx%d │ rockets %d bursts %d ",
                 auto_show ? "ON " : "off", auto_rate,
                 kctx.fb_w, kctx.fb_h, launched, exploded);
        termpaint_surface_write_with_colors(surface, 0, 0, buf,
                TERMPAINT_RGB_COLOR(170, 170, 200), TERMPAINT_DEFAULT_COLOR);
        return;
    }

    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, night);

    // twinkling stars
    for (int i = 0; i < nstars; i++) {
        star *s = &stars[i];
        if (s->x >= w || s->y >= h - 2) {
            continue;
        }
        int tw = ((tick / 4) + s->phase) % 8;
        int v = tw < 4 ? 60 + tw * 30 : 60 + (7 - tw) * 30;
        termpaint_surface_write_with_colors(surface, s->x, s->y,
                tw == 3 ? "✦" : "·",
                TERMPAINT_RGB_COLOR(v, v, v + 20), night);
    }

    // skyline
    for (int x = 0; x < w; x++) {
        int hh = 1 + ((x * 7919 / 13) % 3);
        for (int y = h - hh; y < h; y++) {
            bool window = ((x * 31 + y * 17) % 11) == 0;
            termpaint_surface_write_with_colors(surface, x, y,
                    window ? "▪" : "█",
                    window ? TERMPAINT_RGB_COLOR(255, 220, 120)
                           : TERMPAINT_RGB_COLOR(18, 18, 28),
                    TERMPAINT_RGB_COLOR(18, 18, 28));
        }
    }

    // particles (y is in half cells: pick glyph by intensity)
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particle *p = &particles[i];
        if (p->type == P_NONE) {
            continue;
        }
        int cx = (int)p->x;
        int cy = (int)(p->y / 2);
        if (cx < 0 || cx >= w || cy < 0 || cy >= h) {
            continue;
        }
        float v = p->life / p->max_life;
        int r, g, b;
        const char *glyph;
        if (p->type == P_ROCKET) {
            hue_to_rgb(p->hue, 1.0f, &r, &g, &b);
            glyph = "│";
            // little exhaust trail
            int ty = (int)((p->y + 3) / 2);
            if (ty >= 0 && ty < h) {
                termpaint_surface_write_with_colors(surface, cx, ty, "·",
                        TERMPAINT_RGB_COLOR(255, 180, 60), night);
            }
        } else {
            hue_to_rgb(p->hue, v, &r, &g, &b);
            glyph = v > 0.75f ? "✸" : v > 0.5f ? "●" : v > 0.25f ? "•" : "·";
        }
        termpaint_surface_write_with_colors(surface, cx, cy, glyph,
                TERMPAINT_RGB_COLOR(r, g, b), night);
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             " space/click: launch  a: auto %s  +/-: rate %d  q: quit │ rockets %d bursts %d ",
             auto_show ? "ON " : "off", auto_rate, launched, exploded);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(140, 140, 170), night);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': auto_show = !auto_show; break;
            case '+': if (auto_rate < 10) auto_rate++; break;
            case '-': if (auto_rate > 1) auto_rate--; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            launch_rocket(-1);
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        launch_rocket(event->mouse.x);
    }
}

static void init_stars(void) {
    nstars = MAX_STARS;
    for (int i = 0; i < nstars; i++) {
        stars[i].x = rand() % 500;
        stars[i].y = rand() % 200;
        stars[i].phase = rand() % 8;
    }
}

int main(void) {
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("FIREWORKS_CELLS")) {
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
        int max_dim = 1024;
        const char *s = getenv("FIREWORKS_MAXDIM");
        if (s && atoi(s) >= 256) {
            max_dim = atoi(s);
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    init_stars();
    launch_rocket(-1);

    int timeout = FRAME_MS;
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
            step(FRAME_MS / 1000.0f);
            timeout = FRAME_MS;
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

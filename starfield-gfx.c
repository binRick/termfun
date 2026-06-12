// Starfield at pixel resolution — termpaint + kitty graphics.
// Stars fly past the camera toward a steerable vanishing point. Each
// star draws an additive glow segment from where it was last frame to
// where it is now, into a framebuffer that decays instead of clearing —
// so motion leaves warp streaks that lengthen with speed. Falls back to
// glyph-ramp cell rendering in terminals without graphics support.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, mouse click steers toward the pointer, space hyperjumps,
// 'a' toggles the drifting autopilot, 'c' cycles the colour scheme,
// +/- change the warp speed.

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "termpaint.h"
#include "termpaintx.h"

#include "kitty_gfx.h"

#define MAX_STARS 800
#define FRAME_MS_CELLS 33
#define FRAME_MS_GFX 33      // tune with STARFIELD_FPS if frames are too heavy
#define Z_NEAR 0.05f
#define BASE_SPEED 0.30f     // z units per second at warp x1.00

typedef struct {
    const char *name;
    int fr, fg, fb_;    // far star colour
    int nr, ng, nb;     // near star colour
    int br, bg, bb;     // background tint
} scheme;

static const scheme SCHEMES[] = {
    { "starlight",  90, 110, 150,  255, 255, 255,   2, 4, 10 },
    { "ion",        40,  90, 140,  160, 240, 255,   0, 6, 12 },
    { "ember",     120,  60,  20,  255, 220, 160,   8, 4,  0 },
    { "aurora",     30, 110,  70,  180, 255, 200,   0, 8,  4 },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

typedef struct {
    float x, y, z;      // world position, z shrinks toward the camera
    float px, py;       // projected position last frame
    float glint;        // per-star brightness jitter
} star;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

// The view: pixel grid in pixel mode, terminal cells x half-cell rows in
// cell mode (so steering and projection treat cells as square).
static int vw, vh;
static int nstars;
static star stars[MAX_STARS];
static float cx, cy;            // vanishing point
static float tx, ty;            // where it is steering toward
static bool autopilot = true;
static float auto_ph;

static bool quit_requested;
static int scheme_idx;
static float warp_mul = 1.0f;
static float surge;
static int jumps;

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

static void respawn(star *s, bool anywhere) {
    s->x = (frand() * 2.0f - 1.0f) * 1.1f;
    s->y = (frand() * 2.0f - 1.0f) * 1.1f;
    s->z = anywhere ? Z_NEAR + frand() * (1.0f - Z_NEAR) : 0.85f + frand() * 0.15f;
    s->px = -1;
    s->glint = 0.55f + frand() * 0.45f;
}

static void project(const star *s, float *sx, float *sy) {
    float f = 0.55f * (vw < vh ? vh : vw);
    *sx = cx + s->x * f / s->z;
    *sy = cy + s->y * f / s->z;
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
        vw = w;
        vh = h;
        cx = tx = vw / 2.0f;
        cy = ty = vh / 2.0f;
        nstars = pixel_mode ? vw * vh / 380 : vw * vh / 22;
        if (nstars > MAX_STARS) nstars = MAX_STARS;
        if (nstars < 40) nstars = 40;
        for (int i = 0; i < nstars; i++) {
            respawn(&stars[i], true);
        }
    }
}

static void step(float dt) {
    sync_view();

    surge -= dt * 0.8f;
    if (surge < 0) {
        surge = 0;
    }

    if (autopilot) {
        auto_ph += dt;
        tx = vw * (0.5f + 0.16f * sinf(auto_ph * 0.90f));
        ty = vh * (0.5f + 0.13f * sinf(auto_ph * 0.53f + 1.7f));
    }
    float ease = 2.2f * dt;
    if (ease > 1) ease = 1;
    cx += (tx - cx) * ease;
    cy += (ty - cy) * ease;

    float sp = BASE_SPEED * warp_mul * (1.0f + 5.0f * surge) * dt;
    for (int i = 0; i < nstars; i++) {
        star *s = &stars[i];
        s->z -= sp;
        float sx, sy;
        project(s, &sx, &sy);
        if (s->z <= Z_NEAR || sx < -8 || sx >= vw + 8 || sy < -8 || sy >= vh + 8) {
            respawn(s, false);
        }
    }
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// The framebuffer decays toward zero instead of clearing, so star
// segments from previous frames fade into warp streaks. The image is
// at z=1 with alpha following the glow, like the fireworks sky.

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
            // alpha composites against the terminal background, so let
            // it saturate fast or every star gets double-dimmed
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
    const scheme *sc = &SCHEMES[scheme_idx];

    // decay, don't clear: this is what turns motion into streaks
    size_t n = (size_t)W * H * 4;
    for (size_t i = 0; i < n; i++) {
        fb[i] = (uint8_t)(fb[i] * 205 >> 8);
    }

    for (int i = 0; i < nstars; i++) {
        star *s = &stars[i];
        float sx, sy;
        project(s, &sx, &sy);
        float t = 1.0f - (s->z - Z_NEAR) / (1.0f - Z_NEAR);
        float b = (0.38f + 0.62f * t * t) * s->glint;
        int r = (int)((sc->fr + (sc->nr - sc->fr) * t) * b);
        int g = (int)((sc->fg + (sc->ng - sc->fg) * t) * b);
        int bl = (int)((sc->fb_ + (sc->nb - sc->fb_) * t) * b);
        float radius = 1.0f + t * t * 2.4f;

        float fx = s->px < 0 ? sx : s->px;
        float fy = s->px < 0 ? sy : s->py;
        float dx = sx - fx, dy = sy - fy;
        float dist = sqrtf(dx * dx + dy * dy);
        int samples = (int)(dist / 1.5f) + 1;
        if (samples > 24) samples = 24;
        for (int k = 0; k < samples; k++) {
            float u = samples > 1 ? (float)k / (samples - 1) : 1.0f;
            float fade = 0.35f + 0.65f * u;   // bright head, dimmer tail
            px_glow(fb, W, H, fx + dx * u, fy + dy * u, radius,
                    (int)(r * fade), (int)(g * fade), (int)(bl * fade));
        }
        // remember where this star was drawn so the next frame's segment
        // streaks from here, including any camera pan
        s->px = sx;
        s->py = sy;
    }
}

// ---- cell renderer / overlay ----

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *sc = &SCHEMES[scheme_idx];
    int bg = TERMPAINT_RGB_COLOR(sc->br, sc->bg, sc->bb);
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    char buf[160];
    if (pixel_mode) {
        // the image supplies the stars; cells only carry the status bar
        snprintf(buf, sizeof(buf),
                 " click: steer  space: jump  a: cruise %s  c: %s  +/-: warp x%.2f  q: quit │ kitty gfx %dx%d │ jumps %d ",
                 autopilot ? "on" : "off", sc->name, warp_mul,
                 kctx.fb_w, kctx.fb_h, jumps);
        termpaint_surface_write_with_colors(surface, 0, 0, buf,
                TERMPAINT_RGB_COLOR(150, 165, 190), bg);
        return;
    }

    for (int i = 0; i < nstars; i++) {
        star *s = &stars[i];
        float sx, sy;
        project(s, &sx, &sy);
        int x = (int)sx;
        int y = (int)(sy / 2.0f);
        if (x < 0 || x >= w || y < 1 || y >= h) {
            continue;
        }
        float t = 1.0f - (s->z - Z_NEAR) / (1.0f - Z_NEAR);
        const char *glyph = t > 0.85f ? "✦" : t > 0.60f ? "*"
                          : t > 0.35f ? "•" : "·";
        float b = (0.35f + 0.65f * t) * s->glint;
        int r = (int)((sc->fr + (sc->nr - sc->fr) * t) * b);
        int g = (int)((sc->fg + (sc->ng - sc->fg) * t) * b);
        int bl = (int)((sc->fb_ + (sc->nb - sc->fb_) * t) * b);
        // cells are sparse, so lift the glyph colour for legibility
        r = r * 3 / 2 + 20; if (r > 255) r = 255;
        g = g * 3 / 2 + 20; if (g > 255) g = 255;
        bl = bl * 3 / 2 + 20; if (bl > 255) bl = 255;
        termpaint_surface_write_with_colors(surface, x, y, glyph,
                TERMPAINT_RGB_COLOR(r, g, bl), bg);
    }

    snprintf(buf, sizeof(buf),
             " click: steer  space: jump  a: cruise %s  c: %s  +/-: warp x%.2f  q: quit │ jumps %d ",
             autopilot ? "on" : "off", sc->name, warp_mul, jumps);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(150, 165, 190), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': autopilot = !autopilot; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': if (warp_mul < 4.0f) warp_mul += 0.25f; break;
            case '-': if (warp_mul > 0.25f) warp_mul -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            surge = 1.0f;
            jumps++;
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
            autopilot = false;
            tx = (event->mouse.x + 0.5f) * vw / tc;
            ty = (event->mouse.y + 0.5f) * vh / tr;
        }
    }
}

int main(void) {
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("STARFIELD_CELLS")) {
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
        int max_dim = 512;  // ~480KB RGBA per frame; use STARFIELD_MAXDIM=1024 for sharper
        const char *s = getenv("STARFIELD_MAXDIM");
        if (s && atoi(s) >= 128) {
            max_dim = atoi(s);
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    sync_view();

    int frame_ms = pixel_mode ? FRAME_MS_GFX : FRAME_MS_CELLS;
    const char *fps_env = getenv("STARFIELD_FPS");
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

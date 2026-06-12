// Matrix digital rain — termpaint cells with a kitty graphics glow layer.
// The glyphs are always text; terminals with graphics support get a soft
// phosphor bloom around each stream head on top.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space spawns a wave of drops, mouse click drops one at the
// pointer, 'c' cycles the colour scheme, +/- change the fall speed.

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

#define MAX_COLS 1024
#define MAX_ROWS 400
#define FRAME_MS_CELLS 33
#define FRAME_MS_GFX 33      // tune with MATRIX_FPS if frames are too heavy
#define HEAT_DECAY 1.5f      // heat per second; tail length ≈ speed / decay

static const char *GLYPHS[] = {
    "ｦ", "ｱ", "ｲ", "ｳ", "ｴ", "ｵ", "ｶ", "ｷ", "ｸ", "ｹ", "ｺ", "ｻ", "ｼ", "ｽ",
    "ｾ", "ｿ", "ﾀ", "ﾁ", "ﾂ", "ﾃ", "ﾄ", "ﾅ", "ﾆ", "ﾇ", "ﾈ", "ﾉ", "ﾊ", "ﾋ",
    "ﾌ", "ﾍ", "ﾎ", "ﾏ", "ﾐ", "ﾑ", "ﾒ", "ﾓ", "ﾔ", "ﾕ", "ﾖ", "ﾗ", "ﾘ", "ﾙ",
    "ﾚ", "ﾛ", "ﾜ", "ﾝ",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Z", "*", "+", ":", "=", "<", ">", "¦",
};
#define NGLYPHS ((int)(sizeof(GLYPHS) / sizeof(GLYPHS[0])))

typedef struct {
    const char *name;
    int hr, hg, hb;     // head colour
    int tr, tg, tb;     // tail colour at full heat
    int br, bg, bb;     // background tint
} scheme;

static const scheme SCHEMES[] = {
    { "green", 200, 255, 200,    0, 220,  70,    0, 10,  4 },
    { "amber", 255, 240, 200,  255, 176,   0,   12,  7,  0 },
    { "cyan",  220, 255, 255,    0, 200, 255,    0,  8, 12 },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

typedef struct {
    bool active;
    float head;     // row position of the head
    float speed;    // rows per second
    float delay;    // seconds until the next spawn while inactive
} drop;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

// cell heat: 1.0 where a head just passed, cooling to 0 along the tail
static float heat[MAX_ROWS * MAX_COLS];
static uint8_t glyph_idx[MAX_ROWS * MAX_COLS];
static drop drops[MAX_COLS];

static bool quit_requested;
static int scheme_idx;
static float speed_mul = 1.0f;
static int spawned;

static float frand(void) {
    return (float)rand() / (float)RAND_MAX;
}

static void spawn_drop(int x, bool at_top) {
    drop *d = &drops[x];
    d->active = true;
    d->head = at_top ? 0 : -frand() * 6;
    // screen-relative so tall terminals don't read as slower
    int h = termpaint_surface_height(surface);
    if (h > MAX_ROWS) h = MAX_ROWS;
    if (h < 10) h = 30;
    d->speed = (0.2f + frand() * 0.45f) * h;
    spawned++;
}

static void step(float dt) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    if (w > MAX_COLS) w = MAX_COLS;
    if (h > MAX_ROWS) h = MAX_ROWS;

    // cool every cell; tails fade behind the heads
    for (size_t i = 0; i < (size_t)MAX_ROWS * MAX_COLS; i++) {
        if (heat[i] > 0) {
            heat[i] -= HEAT_DECAY * dt;
            if (heat[i] < 0) {
                heat[i] = 0;
            }
        }
    }

    for (int x = 0; x < w; x++) {
        drop *d = &drops[x];
        if (!d->active) {
            d->delay -= dt;
            if (d->delay <= 0) {
                spawn_drop(x, false);
            }
            continue;
        }
        int prev = (int)floorf(d->head);
        d->head += d->speed * speed_mul * dt;
        int cur = (int)floorf(d->head);
        for (int y = prev + 1; y <= cur; y++) {
            if (y >= 0 && y < h) {
                heat[y * MAX_COLS + x] = 1.0f;
                glyph_idx[y * MAX_COLS + x] = (uint8_t)(rand() % NGLYPHS);
            }
        }
        // recycle once the whole tail has scrolled off the bottom
        if (d->head - d->speed * speed_mul / HEAT_DECAY > h) {
            d->active = false;
            d->delay = frand() * 3.0f;
        }
    }

    // the classic flicker: lit glyphs occasionally mutate in place
    int mutations = w * h / 96 + 1;
    for (int i = 0; i < mutations; i++) {
        int x = rand() % w;
        int y = rand() % h;
        if (heat[y * MAX_COLS + x] > 0.05f) {
            glyph_idx[y * MAX_COLS + x] = (uint8_t)(rand() % NGLYPHS);
        }
    }
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// The image sits at z=1 on top of the cells with additive glow and alpha
// from intensity, so it reads as phosphor bloom over the glyphs. Frame
// decay leaves a fading streak behind each moving head.

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
    int cols = kctx.cols < MAX_COLS ? kctx.cols : MAX_COLS;
    float sx = (float)W / kctx.cols;
    float sy = (float)H / kctx.rows;
    const scheme *sc = &SCHEMES[scheme_idx];

    // decay toward transparent: phosphor persistence for the head streaks
    size_t total = (size_t)W * H * 4;
    for (size_t i = 0; i < total; i++) {
        fb[i] = (uint8_t)((fb[i] * 222) >> 8);   // tuned for ~30fps streaks
    }

    for (int x = 0; x < cols; x++) {
        drop *d = &drops[x];
        if (!d->active || d->head < -1 || d->head > kctx.rows) {
            continue;
        }
        float fx = (x + 0.5f) * sx;
        float fy = (d->head + 0.5f) * sy;
        // wide halo in the tail colour, hot core in the head colour
        px_glow(fb, W, H, fx, fy, sx * 0.8f + 2.5f,
                sc->tr * 3 / 5, sc->tg * 3 / 5, sc->tb * 3 / 5);
        px_glow(fb, W, H, fx, fy, sx * 0.35f + 1.2f,
                sc->hr / 2, sc->hg / 2, sc->hb / 2);
    }
}

// ---- cell renderer ----

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *sc = &SCHEMES[scheme_idx];
    int bg = TERMPAINT_RGB_COLOR(sc->br, sc->bg, sc->bb);
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    int gw = w < MAX_COLS ? w : MAX_COLS;
    int gh = h < MAX_ROWS ? h : MAX_ROWS;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            float v = heat[y * MAX_COLS + x];
            if (v <= 0.04f) {
                continue;
            }
            float f = v * v;   // quadratic falloff keeps the tail tip dim
            termpaint_surface_write_with_colors(surface, x, y,
                    GLYPHS[glyph_idx[y * MAX_COLS + x]],
                    TERMPAINT_RGB_COLOR((int)(sc->tr * f), (int)(sc->tg * f),
                                        (int)(sc->tb * f)),
                    bg);
        }
    }

    // heads stay bright for as long as they occupy a cell
    for (int x = 0; x < gw; x++) {
        drop *d = &drops[x];
        if (!d->active) {
            continue;
        }
        int y = (int)floorf(d->head);
        if (y < 0 || y >= gh) {
            continue;
        }
        termpaint_surface_write_with_colors(surface, x, y,
                GLYPHS[glyph_idx[y * MAX_COLS + x]],
                TERMPAINT_RGB_COLOR(sc->hr, sc->hg, sc->hb), bg);
    }

    char buf[160];
    if (pixel_mode) {
        snprintf(buf, sizeof(buf),
                 " space: wave  click: drop  c: %s  +/-: speed x%.2f  q: quit │ kitty gfx %dx%d │ drops %d ",
                 sc->name, speed_mul, kctx.fb_w, kctx.fb_h, spawned);
    } else {
        snprintf(buf, sizeof(buf),
                 " space: wave  click: drop  c: %s  +/-: speed x%.2f  q: quit │ drops %d ",
                 sc->name, speed_mul, spawned);
    }
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(140, 160, 140), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': if (speed_mul < 3.0f) speed_mul += 0.25f; break;
            case '-': if (speed_mul > 0.3f) speed_mul -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            int w = termpaint_surface_width(surface);
            if (w > MAX_COLS) w = MAX_COLS;
            for (int x = 0; x < w; x++) {
                if (!drops[x].active && drops[x].delay > 0.4f) {
                    drops[x].delay = frand() * 0.4f;
                }
            }
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS
            && event->mouse.x < MAX_COLS) {
        spawn_drop(event->mouse.x, true);
    }
}

int main(void) {
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("MATRIX_CELLS")) {
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
        int max_dim = 512;  // ~480KB RGBA per frame; use MATRIX_MAXDIM=1024 for sharper
        const char *s = getenv("MATRIX_MAXDIM");
        if (s && atoi(s) >= 128) {
            max_dim = atoi(s);
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    // stagger the columns so the rain builds up over the first seconds
    for (int x = 0; x < MAX_COLS; x++) {
        drops[x].active = false;
        drops[x].delay = frand() * 4.0f;
    }

    int frame_ms = pixel_mode ? FRAME_MS_GFX : FRAME_MS_CELLS;
    const char *fps_env = getenv("MATRIX_FPS");
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

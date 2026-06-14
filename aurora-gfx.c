// Aurora (northern lights) at pixel resolution — termpaint + kitty graphics.
// Several slow horizontal "curtain" layers sway across a dark transparent
// sky; each contributes light by its vertical distance from a swaying base
// height, soft and tall above the crest and quick to nothing below. The
// summed light is tinted green -> teal -> violet by intensity and height,
// striated by faint vertical rays, and floats over the terminal background
// (alpha = light intensity), so only the glow is opaque. Falls back to a
// sparse glowy glyph field on terminals without graphics support.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space pulses the curtains brighter, mouse click disturbs the
// nearest curtain at the pointer, 'a' toggles stars, 'c' cycles the
// palette, +/- change the activity (sway speed/amplitude).

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

#define FRAME_MS 50     // 20fps — kitty RGBA frames are large; tune with AURORA_FPS
#define NCURTAINS 4
#define MAX_BUMPS 16
#define NSTARS 220

// Aurora palette: light tinted by intensity AND height. Three stops sampled
// along the curtain's vertical glow — low (green), mid (teal/cyan), high
// tips (violet/magenta).
typedef struct {
    const char *name;
    float lo[3], mid[3], hi[3];   // 0..1 RGB at low / mid / high
} scheme;

static const scheme SCHEMES[] = {
    { "aurora", {0.10f,0.95f,0.35f}, {0.20f,0.85f,0.75f}, {0.75f,0.30f,0.95f} },
    { "solar",  {0.95f,0.25f,0.15f}, {0.95f,0.70f,0.20f}, {0.35f,0.90f,0.35f} },
    { "nebula", {0.20f,0.40f,0.95f}, {0.45f,0.45f,0.95f}, {0.95f,0.35f,0.85f} },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

// A drifting light curtain. Two sines (one fast, one slow) sway the base
// height; freq/phase/speed differ per curtain so they never line up.
typedef struct {
    float mid;       // base centre, normalised 0..1 (0 top)
    float amp;       // sway amplitude, normalised
    float freq;      // primary horizontal frequency
    float speed;     // primary sway speed
    float freq2;     // secondary (slow waviness) frequency
    float speed2;    // secondary sway speed
    float phase;     // accumulated phase
    float weight;    // brightness weight
} curtain;

// A localised click-bump on a curtain's height; relaxes over time.
typedef struct {
    int curtain;
    float u;         // normalised x
    float amp;       // current height push (decays)
} bump;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

static curtain curtains[NCURTAINS];
static bump bumps[MAX_BUMPS];
static int star_x[NSTARS], star_y[NSTARS], star_phase[NSTARS];

static bool quit_requested;
static int scheme_idx;
static bool stars_on = true;
static float activity = 1.0f;   // sway speed/amplitude multiplier, clamped
static float phase;             // accumulated animation time
static float pulse;             // decaying brightness boost from space

static float frand(void) {
    return (float)rand() / (float)RAND_MAX;
}

static int clamp255(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void init_curtains(void) {
    for (int i = 0; i < NCURTAINS; i++) {
        curtain *c = &curtains[i];
        float t = (i + 0.5f) / NCURTAINS;
        c->mid = 0.30f + 0.42f * t;           // stacked down the sky
        c->amp = 0.05f + 0.04f * frand();
        c->freq = 4.0f + 3.0f * frand();
        c->speed = 0.18f + 0.16f * frand();
        c->freq2 = 1.3f + 1.2f * frand();
        c->speed2 = 0.07f + 0.06f * frand();
        c->phase = frand() * 6.2831853f;
        c->weight = 0.7f + 0.5f * (1.0f - t);  // upper curtains a touch brighter
    }
}

static void init_stars(void) {
    for (int i = 0; i < NSTARS; i++) {
        star_x[i] = rand() % 4096;
        star_y[i] = rand() % 4096;
        star_phase[i] = rand() % 8;
    }
}

static void add_bump(int ci, float u) {
    bump *slot = &bumps[0];
    for (int i = 0; i < MAX_BUMPS; i++) {
        if (fabsf(bumps[i].amp) < 0.001f) {
            slot = &bumps[i];
            break;
        }
        if (fabsf(bumps[i].amp) < fabsf(slot->amp)) {
            slot = &bumps[i];     // recycle the weakest
        }
    }
    slot->curtain = ci;
    slot->u = u;
    slot->amp = -0.10f;           // push the crest upward (lower y)
}

// Pick the curtain whose swaying crest is nearest the click height.
static int nearest_curtain(float u, float v) {
    int best = 0;
    float bestd = 1e9f;
    for (int i = 0; i < NCURTAINS; i++) {
        curtain *c = &curtains[i];
        float h = c->mid
                + c->amp * sinf(c->freq * u + c->phase)
                + c->amp * 0.6f * sinf(c->freq2 * u - 0.7f * c->phase);
        float d = fabsf(h - v);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

static void step(float dt) {
    for (int i = 0; i < NCURTAINS; i++) {
        curtain *c = &curtains[i];
        // slow, calm drift; the activity control scales the sway speed
        c->phase += dt * c->speed * (0.6f + 0.4f * activity)
                  + dt * c->speed2 * 0.5f;
    }
    for (int i = 0; i < MAX_BUMPS; i++) {
        // exponential relaxation back to the resting curtain
        bumps[i].amp *= powf(0.25f, dt);
        if (fabsf(bumps[i].amp) < 0.001f) {
            bumps[i].amp = 0.0f;
        }
    }
    if (pulse > 0.0f) {
        pulse *= powf(0.12f, dt);
        if (pulse < 0.002f) {
            pulse = 0.0f;
        }
    }
    phase += dt;
}

// Crest height of a curtain at normalised x, including click bumps.
static float curtain_height(const curtain *c, int ci, float u) {
    float h = c->mid
            + c->amp * (0.7f + 0.6f * activity)
                     * sinf(c->freq * u + c->phase)
            + c->amp * 0.6f * sinf(c->freq2 * u - 0.7f * c->phase);
    for (int i = 0; i < MAX_BUMPS; i++) {
        if (bumps[i].amp == 0.0f || bumps[i].curtain != ci) {
            continue;
        }
        float du = (u - bumps[i].u) * 22.0f;
        h += bumps[i].amp * expf(-du * du);
    }
    return h;
}

// Faint vertical "ray" striations from a cheap hash of x; keeps curtains
// looking filamentary rather than like a smooth gradient.
static float rays(float u) {
    float s = sinf(u * 60.0f) * 0.5f + 0.5f;
    float s2 = sinf(u * 173.0f + 1.7f) * 0.5f + 0.5f;
    return 0.75f + 0.25f * (s * 0.6f + s2 * 0.4f);
}

// Light intensity (0..~) a curtain casts on a pixel at normalised (u, v),
// and the fraction of the curtain's vertical glow this height sits at
// (0 at the crest, 1 high above) for the height tint. Asymmetric falloff:
// soft and tall above the crest, quick to nothing below.
static float curtain_light(const curtain *c, int ci, float u, float v,
                           float *htint) {
    float h = curtain_height(c, ci, u);
    float d = v - h;            // >0 below the crest, <0 above
    float inten, frac;
    if (d <= 0.0f) {
        float up = -d / 0.34f;          // tall soft glow upward
        inten = expf(-up * up);
        frac = up;
        if (frac > 1.0f) frac = 1.0f;
    } else {
        float dn = d / 0.045f;          // quick fade below
        inten = expf(-dn * dn);
        frac = 0.0f;
    }
    *htint = frac;
    return inten * c->weight * rays(u);
}

// Mix the palette by height fraction (0 low .. 1 high tip).
static void tint(const scheme *s, float frac, float *r, float *g, float *b) {
    if (frac < 0.5f) {
        float t = frac / 0.5f;
        *r = s->lo[0] + (s->mid[0] - s->lo[0]) * t;
        *g = s->lo[1] + (s->mid[1] - s->lo[1]) * t;
        *b = s->lo[2] + (s->mid[2] - s->lo[2]) * t;
    } else {
        float t = (frac - 0.5f) / 0.5f;
        *r = s->mid[0] + (s->hi[0] - s->mid[0]) * t;
        *g = s->mid[1] + (s->hi[1] - s->mid[1]) * t;
        *b = s->mid[2] + (s->hi[2] - s->mid[2]) * t;
    }
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// Each pixel is RGBA at z=1 (on top of cells). Transparent (alpha=0) pixels
// reveal the terminal background, which forms the dark sky; only the glow is
// opaque. Alpha fades out over the top cell row so the status bar shows
// through, like fireworks/ripples.

static void render_px(void) {
    if (!kitty_sync_size(&kctx)) {
        return;
    }
    int W = kctx.fb_w, H = kctx.fb_h;
    uint8_t *fb = kctx.fb;
    const scheme *s = &SCHEMES[scheme_idx];
    float band = kctx.rows > 0 ? (float)H / kctx.rows * 1.6f : 24.0f;
    float boost = 1.0f + pulse;

    for (int y = 0; y < H; y++) {
        float v = (float)y / H;
        float af = y < band ? (float)y / band : 1.0f;
        af = af * af;
        uint8_t *px = &fb[(size_t)y * W * 4];
        for (int x = 0; x < W; x++, px += 4) {
            float u = (float)x / W;
            float lr = 0, lg = 0, lb = 0, sum = 0;
            for (int i = 0; i < NCURTAINS; i++) {
                float frac;
                float in = curtain_light(&curtains[i], i, u, v, &frac);
                if (in < 0.004f) {
                    continue;
                }
                float cr, cg, cb;
                tint(s, frac, &cr, &cg, &cb);
                lr += cr * in;
                lg += cg * in;
                lb += cb * in;
                sum += in;
            }
            sum *= boost;
            if (sum > 0.0f) {
                lr *= boost; lg *= boost; lb *= boost;
            }
            // faint star speckle where the sky is dark
            if (stars_on && sum < 0.05f) {
                int hsh = (x * 73856093) ^ (y * 19349663);
                if ((hsh & 0x3ff) == 0) {
                    int tw = ((int)(phase * 6.0f) + (hsh >> 11)) % 8;
                    float sv = (tw < 4 ? tw : 7 - tw) / 4.0f;
                    float a = 0.25f + 0.45f * sv;
                    px[0] = (uint8_t)(200 * a);
                    px[1] = (uint8_t)(210 * a);
                    px[2] = (uint8_t)(255 * a);
                    px[3] = (uint8_t)(255 * a * af);
                    continue;
                }
            }
            if (sum <= 0.0f) {
                px[0] = px[1] = px[2] = px[3] = 0;   // fully transparent sky
                continue;
            }
            float norm = sum > 1.0f ? 1.0f / sum : 1.0f;
            float br = sum > 1.0f ? 1.0f : sum;      // luminance for alpha
            px[0] = (uint8_t)clamp255((int)(255 * lr * norm));
            px[1] = (uint8_t)clamp255((int)(255 * lg * norm));
            px[2] = (uint8_t)clamp255((int)(255 * lb * norm));
            float a = br * 0.92f;
            if (a > 1.0f) a = 1.0f;
            px[3] = (uint8_t)(255 * a * af);
        }
    }
}

// ---- cell renderer / overlay ----

// Glow ramp: sparse shading glyphs, dense only at the bright crest.
static const char *RAMP[] = { "░", "▒", "▓", "█" };
#define NRAMP ((int)(sizeof(RAMP) / sizeof(RAMP[0])))

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    const scheme *s = &SCHEMES[scheme_idx];
    int bg = TERMPAINT_RGB_COLOR(2, 3, 10);
    char buf[180];

    if (pixel_mode) {
        // the image supplies the aurora; cells only carry the status bar
        termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR,
                                TERMPAINT_DEFAULT_COLOR);
        snprintf(buf, sizeof(buf),
                 " space: pulse  click: disturb  a: stars %s  c: %s  +/-: activity x%.2f  q: quit │ kitty gfx %dx%d ",
                 stars_on ? "ON " : "off", s->name, activity,
                 kctx.fb_w, kctx.fb_h);
        termpaint_surface_write_with_colors(surface, 0, 0, buf,
                TERMPAINT_RGB_COLOR(180, 190, 205), TERMPAINT_DEFAULT_COLOR);
        return;
    }

    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);
    float boost = 1.0f + pulse;

    // faint star speckles
    if (stars_on) {
        for (int i = 0; i < NSTARS; i++) {
            int sx = star_x[i] % (w > 0 ? w : 1);
            int sy = star_y[i] % (h > 1 ? h - 1 : 1) + 1;
            int tw = ((int)(phase * 6.0f) + star_phase[i]) % 8;
            int sv = tw < 4 ? 40 + tw * 22 : 40 + (7 - tw) * 22;
            termpaint_surface_write_with_colors(surface, sx, sy,
                    tw == 3 ? "✦" : "·",
                    TERMPAINT_RGB_COLOR(sv, sv, sv + 25), bg);
        }
    }

    // curtains: per column draw glyphs coloured by curtain intensity, only
    // where intensity clears a small threshold (sparse, glowy)
    for (int x = 0; x < w; x++) {
        float u = (x + 0.5f) / w;
        for (int y = 1; y < h; y++) {
            float v = (y + 0.5f) / h;
            float lr = 0, lg = 0, lb = 0, sum = 0;
            for (int i = 0; i < NCURTAINS; i++) {
                float frac;
                float in = curtain_light(&curtains[i], i, u, v, &frac);
                if (in < 0.004f) {
                    continue;
                }
                float cr, cg, cb;
                tint(s, frac, &cr, &cg, &cb);
                lr += cr * in; lg += cg * in; lb += cb * in;
                sum += in;
            }
            sum *= boost;
            if (sum < 0.10f) {
                continue;   // below threshold: dark transparent sky
            }
            lr *= boost; lg *= boost; lb *= boost;
            float norm = sum > 1.0f ? 1.0f / sum : 1.0f;
            float br = sum > 1.0f ? 1.0f : sum;
            int r = clamp255((int)(255 * lr * norm * (0.5f + 0.5f * br)));
            int g = clamp255((int)(255 * lg * norm * (0.5f + 0.5f * br)));
            int b = clamp255((int)(255 * lb * norm * (0.5f + 0.5f * br)));
            int gi = (int)(br * NRAMP);
            if (gi >= NRAMP) gi = NRAMP - 1;
            if (gi < 0) gi = 0;
            termpaint_surface_write_with_colors(surface, x, y, RAMP[gi],
                    TERMPAINT_RGB_COLOR(r, g, b), bg);
        }
    }

    snprintf(buf, sizeof(buf),
             " space: pulse  click: disturb  a: stars %s  c: %s  +/-: activity x%.2f  q: quit ",
             stars_on ? "ON " : "off", s->name, activity);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 190, 205), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': stars_on = !stars_on; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': if (activity < 3.0f) activity += 0.25f; break;
            case '-': if (activity > 0.25f) activity -= 0.25f; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            pulse = 0.9f;       // decaying brightness boost
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        // mouse positions are cell coordinates; map to normalised x/y
        int tc = termpaint_surface_width(surface);
        int tr = termpaint_surface_height(surface);
        if (tc > 0 && tr > 0) {
            float u = (event->mouse.x + 0.5f) / tc;
            float v = (event->mouse.y + 0.5f) / tr;
            add_bump(nearest_curtain(u, v), u);
        }
    }
}

int main(void) {
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("AURORA_CELLS")) {
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
        int max_dim = 512;  // ~1MB RGBA per frame; use AURORA_MAXDIM=1024 for sharper
        const char *s = getenv("AURORA_MAXDIM");
        if (s && atoi(s) >= 128) {
            max_dim = atoi(s);
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    init_curtains();
    init_stars();

    int frame_ms = FRAME_MS;
    const char *fps_env = getenv("AURORA_FPS");
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

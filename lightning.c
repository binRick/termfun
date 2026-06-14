// Lightning — branching bolts forked across the terminal cell grid.
// Each strike is built by midpoint displacement: a segment from a top
// point to an end point is subdivided, its midpoint kicked perpendicular
// by a shrinking random offset, occasionally spawning a dimmer branch.
// The resulting segments are rasterised onto a per-cell brightness buffer
// that decays each frame, so the fork flashes then fades. Strikes also
// flood the sky with a brief tinted flash.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space strikes a bolt, mouse click strikes down to the
// pointer, 'a' toggles the auto-storm, 'c' cycles the tint
// (blue/violet/white/red), +/- change the branchiness.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "termpaint.h"
#include "termpaintx.h"

#define FRAME_MS 33

#define MAX_SEGS  2048   // line segments across all live bolts
#define MAX_BOLTS 16
#define MAXW 512
#define MAXH 256

typedef struct {
    const char *name;
    int r, g, b;        // core tint (mixed toward white for the hot core)
} scheme;

static const scheme SCHEMES[] = {
    { "blue",   120, 170, 255 },
    { "violet", 190, 130, 255 },
    { "white",  235, 235, 255 },
    { "red",    255, 110,  90 },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

typedef struct {
    float x0, y0, x1, y1;   // endpoints in cell coordinates (y in half-cells)
    float bright;           // 0..1 segment brightness
    int bolt;               // owning bolt index
} segment;

typedef struct {
    bool active;
    float life, max_life;
} bolt;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static segment segs[MAX_SEGS];
static int nsegs;
static bolt bolts[MAX_BOLTS];

// per-cell brightness buffer (decays each frame) and the cell's tint
static float cell_b[MAXH][MAXW];
static unsigned char cell_s[MAXH][MAXW];   // scheme index that lit the cell

static bool quit_requested;
static int scheme_idx;
static bool auto_storm = true;
static int branchiness = 4;   // 1..8: branch spawn likelihood / count
static float flash;           // 0..1 whole-screen flash level
static int strikes;
static int tick;

static float frand(void) {
    return (float)rand() / (float)RAND_MAX;
}

static int clamp255(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static bolt *alloc_bolt(void) {
    for (int i = 0; i < MAX_BOLTS; i++) {
        if (!bolts[i].active) {
            return &bolts[i];
        }
    }
    return NULL;
}

static void add_seg(float x0, float y0, float x1, float y1, float bright, int bolt) {
    if (nsegs >= MAX_SEGS) {
        return;
    }
    segment *s = &segs[nsegs++];
    s->x0 = x0; s->y0 = y0; s->x1 = x1; s->y1 = y1;
    s->bright = bright;
    s->bolt = bolt;
}

// Recursively subdivide one segment by midpoint displacement, kicking the
// midpoint perpendicular by an offset that shrinks each level. With some
// probability spawn a dimmer branch that veers off and subdivides too.
static void subdivide(float x0, float y0, float x1, float y1,
                      float offset, int level, float bright, int bolt) {
    if (level <= 0 || nsegs >= MAX_SEGS) {
        add_seg(x0, y0, x1, y1, bright, bolt);
        return;
    }
    float mx = (x0 + x1) * 0.5f;
    float my = (y0 + y1) * 0.5f;
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 0.0001f) {
        // perpendicular unit vector
        float px = -dy / len, py = dx / len;
        float k = (frand() * 2.0f - 1.0f) * offset;
        mx += px * k;
        my += py * k;
    }

    // chance to spawn a branch from the midpoint
    if (level >= 2 && frand() < 0.06f * branchiness && nsegs < MAX_SEGS - 32) {
        float ang = atan2f(dy, dx) + (frand() < 0.5f ? -1.0f : 1.0f) * (0.4f + frand() * 0.7f);
        float blen = len * (0.4f + frand() * 0.4f);
        float bx = mx + cosf(ang) * blen;
        float by = my + sinf(ang) * blen;
        subdivide(mx, my, bx, by, offset * 0.6f, level - 1, bright * 0.55f, bolt);
    }

    subdivide(x0, y0, mx, my, offset * 0.55f, level - 1, bright, bolt);
    subdivide(mx, my, x1, y1, offset * 0.55f, level - 1, bright, bolt);
}

// Build a bolt from (x0,y0) to (x1,y1) in cell coords (y in half-cells).
static void make_bolt(float x0, float y0, float x1, float y1) {
    bolt *bt = alloc_bolt();
    if (!bt) {
        return;
    }
    int idx = (int)(bt - bolts);
    bt->active = true;
    bt->max_life = bt->life = 0.30f + frand() * 0.20f;
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    float offset = len * 0.18f;
    subdivide(x0, y0, x1, y1, offset, 6, 1.0f, idx);
    flash = 1.0f;
    strikes++;
}

static void strike_random(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    float x0 = frand() * w;
    float x1 = 0.2f * w + frand() * 0.6f * w;
    make_bolt(x0, 2.0f, x1, (h - 1) * 2.0f);
}

static void strike_to(int cell_x, int cell_y) {
    int w = termpaint_surface_width(surface);
    float x0 = 0.25f * w + frand() * 0.5f * w;
    make_bolt(x0, 2.0f, (float)cell_x, cell_y * 2.0f + 1.0f);
}

static void rebuild_bolts(void) {
    // collapse the segment list, dropping segments of dead bolts
    int out = 0;
    for (int i = 0; i < nsegs; i++) {
        if (bolts[segs[i].bolt].active) {
            segs[out++] = segs[i];
        }
    }
    nsegs = out;
}

static void step(float dt) {
    bool any_died = false;
    for (int i = 0; i < MAX_BOLTS; i++) {
        if (!bolts[i].active) {
            continue;
        }
        bolts[i].life -= dt;
        if (bolts[i].life <= 0) {
            bolts[i].active = false;
            any_died = true;
        }
    }
    if (any_died) {
        rebuild_bolts();
    }

    flash -= dt * 3.0f;
    if (flash < 0) {
        flash = 0;
    }

    // decay the per-cell brightness buffer
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    if (w > MAXW) w = MAXW;
    if (h > MAXH) h = MAXH;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            cell_b[y][x] *= 0.80f;
            if (cell_b[y][x] < 0.01f) {
                cell_b[y][x] = 0;
            }
        }
    }

    // an auto-storm fires occasional strikes
    if (auto_storm && rand() % 22 == 0) {
        strike_random();
    }
    tick++;
}

// Pick a line glyph from the segment slope (y in half-cells: scale for the
// ~2:1 cell aspect so a "vertical" bolt reads as vertical).
static const char *slope_glyph(float dx, float dy) {
    float ady = fabsf(dy) * 0.5f;   // half-cells -> cells
    float adx = fabsf(dx);
    if (adx < 0.5f * ady) {
        return "┃";            // ┃ steep
    }
    if (ady < 0.5f * adx) {
        return "━";            // ━ shallow
    }
    return (dx * dy < 0) ? "╱" : "╲";   // ╱ ╲
}

// Rasterise every live segment onto the cell brightness buffer.
static void rasterise(int w, int h) {
    for (int i = 0; i < nsegs; i++) {
        segment *s = &segs[i];
        float life = bolts[s->bolt].life / bolts[s->bolt].max_life;
        float bright = s->bright * (0.4f + 0.6f * life);
        float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
        float len = sqrtf(dx * dx + dy * dy);
        int n = (int)(len) + 1;
        const char *g = slope_glyph(dx, dy);
        (void)g;
        for (int k = 0; k <= n; k++) {
            float t = n > 0 ? (float)k / n : 0;
            int cx = (int)(s->x0 + dx * t);
            int cy = (int)((s->y0 + dy * t) * 0.5f);
            if (cx < 0 || cx >= w || cy < 1 || cy >= h) {
                continue;
            }
            if (bright > cell_b[cy][cx]) {
                cell_b[cy][cx] = bright;
                cell_s[cy][cx] = (unsigned char)scheme_idx;
            }
        }
    }
}

static const char *RAMP[] = { ".", ":", "*", "┃", "█" };
#define NRAMP ((int)(sizeof(RAMP) / sizeof(RAMP[0])))

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    if (w > MAXW) w = MAXW;
    if (h > MAXH) h = MAXH;

    // whole-sky flash as a dim tint behind everything
    const scheme *fs = &SCHEMES[scheme_idx];
    int fr = (int)(fs->r * flash * 0.18f);
    int fg = (int)(fs->g * flash * 0.18f);
    int fb = (int)(fs->b * flash * 0.18f);
    int bg = TERMPAINT_RGB_COLOR(clamp255(fr), clamp255(fg), clamp255(fb));
    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);

    rasterise(w, h);

    for (int y = 1; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float b = cell_b[y][x];
            if (b <= 0.02f) {
                continue;
            }
            const scheme *s = &SCHEMES[cell_s[y][x]];
            // hot core mixes toward white; dimmer cells keep the tint
            float white = b > 0.7f ? (b - 0.7f) / 0.3f : 0;
            int r = clamp255((int)((s->r + (255 - s->r) * white) * b));
            int g = clamp255((int)((s->g + (255 - s->g) * white) * b));
            int bl = clamp255((int)((s->b + (255 - s->b) * white) * b));
            int gi = (int)(b * NRAMP);
            if (gi >= NRAMP) gi = NRAMP - 1;
            termpaint_surface_write_with_colors(surface, x, y, RAMP[gi],
                    TERMPAINT_RGB_COLOR(r, g, bl), bg);
        }
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             " space/click: strike  a: storm %s  c: %s  +/-: branch %d  q: quit │ strikes %d ",
             auto_storm ? "ON " : "off", fs->name, branchiness, strikes);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': auto_storm = !auto_storm; break;
            case 'c': scheme_idx = (scheme_idx + 1) % NSCHEMES; break;
            case '+':
            case '=': if (branchiness < 8) branchiness++; break;
            case '-': if (branchiness > 1) branchiness--; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            strike_random();
        } else if (event->key.atom == termpaint_input_escape()) {
            quit_requested = true;
        }
    }
    if (event->type == TERMPAINT_EV_MOUSE
            && event->mouse.action == TERMPAINT_MOUSE_PRESS) {
        strike_to(event->mouse.x, event->mouse.y);
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

    strike_random();

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

// Falling sand at pixel resolution — termpaint + kitty graphics.
// A falling-sand cellular automaton: a grid of material cells updated
// bottom-up. Powders pile at their angle of repose, liquids level out,
// oil floats on water, embers glow and cool to ash. A steady stream pours
// from the top so there's always motion. Renders one framebuffer block per
// grid cell; falls back to glyph + colour cell rendering in terminals
// without graphics support.
// SPDX-License-Identifier: 0BSD
//
// q/Esc quit, space drops a chunk of the current material from the top
// centre, mouse click pours the current material at the pointer, 'a'
// clears the field, 'c' cycles the material, +/- change the brush/pour
// size.

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "termpaint.h"
#include "termpaintx.h"

#include "kitty_gfx.h"

#define MAX_GW 256           // CA cost is O(w*h) per frame — cap both dims
#define MAX_GH 256
#define FRAME_MS_CELLS 33
#define FRAME_MS_GFX 50      // tune with SAND_FPS if frames are too heavy

// Material ids packed into the low nibble of each cell; the high nibble of
// EMBER cells carries a heat value (0..15) so they can cool over time.
enum { EMPTY = 0, WALL, SAND, WATER, OIL, EMBER };
#define NMAT 4               // selectable materials (sand, water, oil, ember)

typedef struct {
    const char *name;
    int mat;
    int r, g, b;             // base colour
    const char *glyph;       // cell-mode glyph
} material;

static const material MATS[NMAT] = {
    { "sand",  SAND,  194, 168,  98, "▒" },   // ▒ tan powder
    { "water", WATER,  40, 110, 210, "≈" },   // ≈ blue liquid
    { "oil",   OIL,    60,  46,  30, "~"        },  // ~ dark liquid
    { "ember", EMBER, 240, 170,  40, "▓" },   // ▓ glowing powder
};

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static kitty_ctx kctx;
static bool pixel_mode;
static int tty_fd = -1;

// Material grid: low nibble = material id, high nibble = ember heat. Updated
// in place bottom-up; the scan bias alternates per frame to avoid drift.
static uint8_t grid[MAX_GW * MAX_GH];
static int gw, gh;

static bool quit_requested;
static int mat_idx;          // index into MATS for the current material
static int brush = 3;        // pour radius in grid cells
static bool ambient = true;  // steady stream from the top
static int frame_no;

static uint32_t rng = 0x9e3779b9u;

static inline uint32_t xr(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static inline int mat_of(uint8_t c) { return c & 0x0f; }
static inline int heat_of(uint8_t c) { return c >> 4; }
static inline uint8_t make_cell(int mat, int heat) {
    return (uint8_t)((mat & 0x0f) | ((heat & 0x0f) << 4));
}

static void sync_sim(void) {
    int w, h;
    if (pixel_mode) {
        // one grid cell per framebuffer block; pick a block that keeps the
        // grid under the cap while staying close to the cell aspect
        int block = 1;
        while (kctx.fb_w / block > MAX_GW || kctx.fb_h / block > MAX_GH) {
            block++;
        }
        w = kctx.fb_w / block;
        h = kctx.fb_h / block;
    } else {
        w = termpaint_surface_width(surface);
        h = termpaint_surface_height(surface) * 2;   // half-cell rows
    }
    if (w > MAX_GW) w = MAX_GW;
    if (h > MAX_GH) h = MAX_GH;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (w != gw || h != gh) {
        gw = w;
        gh = h;
        memset(grid, 0, sizeof(grid));
    }
}

// Drop a round blob of `mat` centred on grid cell (cx, cy).
static void pour(int mat, float cx, float cy, float radius) {
    int ir = (int)radius + 1;
    for (int dy = -ir; dy <= ir; dy++) {
        int y = (int)cy + dy;
        if (y < 0 || y >= gh) {
            continue;
        }
        for (int dx = -ir; dx <= ir; dx++) {
            int x = (int)cx + dx;
            if (x < 0 || x >= gw) {
                continue;
            }
            if (dx * dx + dy * dy >= radius * radius) {
                continue;
            }
            int heat = mat == EMBER ? 15 : 0;
            grid[y * gw + x] = make_cell(mat, heat);
        }
    }
}

static inline bool is_liquid(int m) { return m == WATER || m == OIL; }

// Bottom-up update. Each cell tries to move down, then diagonally, then
// (liquids only) sideways; oil density-swaps upward through water.
static void step(void) {
    sync_sim();
    frame_no++;
    bool left_first = (frame_no & 1) != 0;

    for (int y = gh - 2; y >= 0; y--) {
        // alternate horizontal scan direction so flow has no fixed bias
        int xs = left_first ? 0 : gw - 1;
        int xe = left_first ? gw : -1;
        int xd = left_first ? 1 : -1;
        for (int x = xs; x != xe; x += xd) {
            uint8_t c = grid[y * gw + x];
            int m = mat_of(c);
            if (m == EMPTY || m == WALL) {
                continue;
            }

            if (m == EMBER) {
                // cool over time; flicker the rate a little
                int heat = heat_of(c);
                if ((xr() & 7) == 0 && heat > 0) {
                    heat--;
                    if (heat == 0) {
                        // burnt out: vanish, or leave dim ash now and then
                        c = (xr() & 3) == 0 ? make_cell(SAND, 0) : EMPTY;
                        grid[y * gw + x] = c;
                        if (c == EMPTY) {
                            continue;
                        }
                        m = mat_of(c);
                    } else {
                        c = make_cell(EMBER, heat);
                        grid[y * gw + x] = c;
                    }
                }
            }

            int below = (y + 1) * gw + x;
            int bm = mat_of(grid[below]);

            // straight down into empty (or, for oil, up through denser water)
            if (bm == EMPTY) {
                grid[below] = c;
                grid[y * gw + x] = EMPTY;
                continue;
            }
            if (m == OIL && bm == WATER) {
                // oil is lighter: swap so it rises above the water
                grid[below] = c;
                grid[y * gw + x] = make_cell(WATER, 0);
                continue;
            }

            // diagonal slide (down-left / down-right), repose for powders
            int first = left_first ? -1 : 1;
            for (int t = 0; t < 2; t++, first = -first) {
                int nx = x + first;
                if (nx < 0 || nx >= gw) {
                    continue;
                }
                int dm = mat_of(grid[(y + 1) * gw + nx]);
                if (dm == EMPTY) {
                    grid[(y + 1) * gw + nx] = c;
                    grid[y * gw + x] = EMPTY;
                    goto moved;
                }
                if (m == OIL && dm == WATER) {
                    grid[(y + 1) * gw + nx] = c;
                    grid[y * gw + x] = make_cell(WATER, 0);
                    goto moved;
                }
            }

            // liquids level out: spread sideways into an adjacent empty cell
            if (is_liquid(m)) {
                int side = left_first ? -1 : 1;
                for (int t = 0; t < 2; t++, side = -side) {
                    int nx = x + side;
                    if (nx < 0 || nx >= gw) {
                        continue;
                    }
                    if (mat_of(grid[y * gw + nx]) == EMPTY) {
                        grid[y * gw + nx] = c;
                        grid[y * gw + x] = EMPTY;
                        break;
                    }
                }
            }
        moved:;
        }
    }

    // steady ambient pour: a thin stream of the current material at top centre
    if (ambient && gw > 0) {
        int cx = gw / 2;
        int span = 1 + gw / 48;
        for (int dx = -span; dx <= span; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= gw) {
                continue;
            }
            if ((xr() & 3) != 0) {
                continue;
            }
            if (mat_of(grid[x]) == EMPTY) {
                int mat = MATS[mat_idx].mat;
                grid[x] = make_cell(mat, mat == EMBER ? 15 : 0);
            }
        }
    }
}

// Per-cell colour, with a tiny deterministic jitter so piles look granular.
static void cell_rgb(uint8_t c, int gx, int gy, int *r, int *g, int *b) {
    int m = mat_of(c);
    if (m == EMBER) {
        // bright yellow -> orange -> dark red as heat fades
        float t = heat_of(c) / 15.0f;
        *r = (int)(80 + 175 * t);
        *g = (int)(20 + 150 * t * t);
        *b = (int)(10 + 30 * t);
    } else {
        const material *mt = NULL;
        for (int i = 0; i < NMAT; i++) {
            if (MATS[i].mat == m) { mt = &MATS[i]; break; }
        }
        if (mt == NULL) {
            *r = *g = *b = 0;
            return;
        }
        // hash the cell position for a stable +/- granular jitter
        uint32_t h = (uint32_t)(gx * 73856093) ^ (uint32_t)(gy * 19349663);
        h ^= h >> 13;
        int j = (int)(h & 31) - 16;
        *r = mt->r + j;
        *g = mt->g + j;
        *b = mt->b + j;
    }
    if (*r < 0) *r = 0; if (*r > 255) *r = 255;
    if (*g < 0) *g = 0; if (*g > 255) *g = 255;
    if (*b < 0) *b = 0; if (*b > 255) *b = 255;
}

// ---- pixel renderer (kitty graphics, RGBA) ----
//
// One framebuffer block per grid cell, opaque where filled and alpha 0
// where empty, so the sand sits over the terminal background.

static void render_px(void) {
    if (!kitty_sync_size(&kctx)) {
        return;
    }
    int W = kctx.fb_w, H = kctx.fb_h;
    uint8_t *fb = kctx.fb;

    memset(fb, 0, (size_t)W * H * 4);
    if (gw <= 0 || gh <= 0) {
        return;
    }

    for (int y = 0; y < H; y++) {
        int gy = (int)((int64_t)y * gh / H);
        if (gy >= gh) gy = gh - 1;
        uint8_t *px = &fb[(size_t)y * W * 4];
        for (int x = 0; x < W; x++, px += 4) {
            int gx = (int)((int64_t)x * gw / W);
            if (gx >= gw) gx = gw - 1;
            uint8_t c = grid[gy * gw + gx];
            if (mat_of(c) == EMPTY) {
                continue;   // leave transparent
            }
            int r, g, b;
            cell_rgb(c, gx, gy, &r, &g, &b);
            px[0] = (uint8_t)r;
            px[1] = (uint8_t)g;
            px[2] = (uint8_t)b;
            px[3] = 255;
        }
    }
}

// ---- cell renderer / overlay ----

static const char *glyph_for(int m) {
    if (m == EMBER) return "▓";   // ▓
    for (int i = 0; i < NMAT; i++) {
        if (MATS[i].mat == m) return MATS[i].glyph;
    }
    return "▒";                   // ash / fallback ▒
}

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    int bg = TERMPAINT_RGB_COLOR(8, 8, 12);
    const material *mt = &MATS[mat_idx];
    char buf[200];

    if (pixel_mode) {
        // the image supplies the sand; cells only carry the status bar
        termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR,
                                TERMPAINT_DEFAULT_COLOR);
        snprintf(buf, sizeof(buf),
                 " space: drop  click: pour  a: clear  c: %s  +/-: brush %d  q: quit │ kitty gfx %dx%d ",
                 mt->name, brush, kctx.fb_w, kctx.fb_h);
        termpaint_surface_write_with_colors(surface, 0, 0, buf,
                TERMPAINT_RGB_COLOR(180, 180, 200), TERMPAINT_DEFAULT_COLOR);
        return;
    }

    termpaint_surface_clear(surface, TERMPAINT_DEFAULT_COLOR, bg);
    // two half-cell rows per terminal row; the denser cell takes the glyph
    for (int y = 1; y < h && 2 * y + 1 < gh; y++) {
        for (int x = 0; x < w && x < gw; x++) {
            uint8_t top = grid[2 * y * gw + x];
            uint8_t bot = grid[(2 * y + 1) * gw + x];
            uint8_t c = mat_of(top) != EMPTY ? top : bot;
            int m = mat_of(c);
            if (m == EMPTY) {
                continue;
            }
            int r, g, b;
            cell_rgb(c, x, 2 * y, &r, &g, &b);
            // cells are sparse, so lift the glyph colour for legibility
            r = r * 3 / 2 + 20; if (r > 255) r = 255;
            g = g * 3 / 2 + 20; if (g > 255) g = 255;
            b = b * 3 / 2 + 20; if (b > 255) b = 255;
            termpaint_surface_write_with_colors(surface, x, y, glyph_for(m),
                    TERMPAINT_RGB_COLOR(r, g, b), bg);
        }
    }

    snprintf(buf, sizeof(buf),
             " space: drop  click: pour  a: clear  c: %s  +/-: brush %d  q: quit ",
             mt->name, brush);
    termpaint_surface_write_with_colors(surface, 0, 0, buf,
            TERMPAINT_RGB_COLOR(180, 180, 200), bg);
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'a': memset(grid, 0, sizeof(grid)); break;
            case 'c': mat_idx = (mat_idx + 1) % NMAT; break;
            case '+':
            case '=': if (brush < 16) brush++; break;
            case '-': if (brush > 1) brush--; break;
        }
    }
    if (event->type == TERMPAINT_EV_KEY) {
        if (event->key.atom == termpaint_input_space()) {
            // drop a chunk of the current material from the top centre
            pour(MATS[mat_idx].mat, gw / 2.0f, gh / 12.0f + 2.0f,
                 brush + 2.0f);
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
            pour(MATS[mat_idx].mat,
                 (event->mouse.x + 0.5f) * gw / tc,
                 (event->mouse.y + 0.5f) * gh / tr, brush);
        }
    }
}

int main(void) {
    // kitty graphics detection talks to the tty in raw mode, so it has to
    // happen before termpaint takes over the terminal
    bool kitty_ok = false;
    if (!getenv("SAND_CELLS")) {
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
        int max_dim = 512;  // grid is capped well below this; SAND_MAXDIM tunes sharpness
        const char *s = getenv("SAND_MAXDIM");
        if (s && atoi(s) >= 128) {
            max_dim = atoi(s);
        }
        pixel_mode = kitty_init(&kctx, tty_fd, max_dim);
    }

    sync_sim();

    int frame_ms = pixel_mode ? FRAME_MS_GFX : FRAME_MS_CELLS;
    const char *fps_env = getenv("SAND_FPS");
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
            step();
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

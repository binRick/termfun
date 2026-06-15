// 2048 — the sliding-tile puzzle for your terminal, built on termpaint.
// Slide the tiles; equal ones merge; reach 2048. The board floats over an
// animated kitty-graphics plasma where supported (cells everywhere else).
// Your best score is kept in ~/.termfun-2048.
//
// SPDX-License-Identifier: 0BSD
//
//   ↑↓←→ / WASD / HJKL  slide      u  undo (1 step)
//   n or r  new game               q/Esc  quit
//
// Build: see Makefile. 2048 = cells; 2048-gfx = kitty backdrop.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "termpaint.h"
#include "termpaintx.h"

#include "tui.h"

#define N 4
#define ACCENT TERMPAINT_RGB_COLOR(237, 194, 46)
#define BACK1  TERMPAINT_RGB_COLOR(28, 22, 40)      // dark plum
#define BACK2  TERMPAINT_RGB_COLOR(190, 96, 42)     // ember orange

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static int grid[N][N];
static int score, best;
static bool won, over;
static int undo_grid[N][N], undo_score;
static bool have_undo;
static bool quit_requested;
static double anim_t;

// ---------------------------------------------------------------- best score
static const char *best_path(void) {
    static char p[1024];
    const char *env = getenv("G2048_FILE");
    if (env && *env) { snprintf(p, sizeof(p), "%s", env); return p; }
    const char *home = getenv("HOME");
    snprintf(p, sizeof(p), "%s/.termfun-2048", home ? home : ".");
    return p;
}
static void load_best(void) {
    FILE *f = fopen(best_path(), "r");
    if (f) { if (fscanf(f, "%d", &best) != 1) best = 0; fclose(f); }
}
static void save_best(void) {
    FILE *f = fopen(best_path(), "w");
    if (f) { fprintf(f, "%d\n", best); fclose(f); }
}

// ---------------------------------------------------------------- game logic
static void spawn_tile(void) {
    int empties[N * N], n = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (!grid[r][c]) empties[n++] = r * N + c;
    if (!n) return;
    int idx = empties[rand() % n];
    grid[idx / N][idx % N] = (rand() % 10) ? 2 : 4;
}

static bool slide_row(int *a) {
    int tmp[N], n = 0;
    for (int i = 0; i < N; i++) if (a[i]) tmp[n++] = a[i];
    int out[N] = {0}, m = 0;
    for (int i = 0; i < n; i++) {
        if (i + 1 < n && tmp[i] == tmp[i + 1]) {
            out[m] = tmp[i] * 2;
            score += out[m];
            if (out[m] == 2048) won = true;
            i++;
            m++;
        } else {
            out[m++] = tmp[i];
        }
    }
    bool changed = false;
    for (int i = 0; i < N; i++) {
        if (a[i] != out[i]) changed = true;
        a[i] = out[i];
    }
    return changed;
}

static void cell_of(int dir, int line, int i, int *r, int *c) {
    switch (dir) {
        case 0: *r = i;       *c = line;    break;   // up
        case 1: *r = N - 1 - i; *c = line;  break;   // down
        case 2: *r = line;    *c = i;       break;   // left
        default: *r = line;   *c = N - 1 - i; break; // right
    }
}

static bool move_dir(int dir) {
    bool moved = false;
    for (int line = 0; line < N; line++) {
        int a[N];
        for (int i = 0; i < N; i++) { int r, c; cell_of(dir, line, i, &r, &c); a[i] = grid[r][c]; }
        if (slide_row(a)) moved = true;
        for (int i = 0; i < N; i++) { int r, c; cell_of(dir, line, i, &r, &c); grid[r][c] = a[i]; }
    }
    return moved;
}

static bool can_move(void) {
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (!grid[r][c]) return true;
            if (c + 1 < N && grid[r][c] == grid[r][c + 1]) return true;
            if (r + 1 < N && grid[r][c] == grid[r + 1][c]) return true;
        }
    return false;
}

static void new_game(void) {
    memset(grid, 0, sizeof(grid));
    score = 0;
    won = over = have_undo = false;
    spawn_tile();
    spawn_tile();
}

static void do_move(int dir) {
    if (over) return;
    int save[N][N];
    memcpy(save, grid, sizeof(grid));
    int sc = score;
    if (move_dir(dir)) {
        memcpy(undo_grid, save, sizeof(grid));
        undo_score = sc;
        have_undo = true;
        spawn_tile();
        if (score > best) { best = score; save_best(); }
        if (!can_move()) over = true;
    }
}

static void do_undo(void) {
    if (!have_undo) return;
    memcpy(grid, undo_grid, sizeof(grid));
    score = undo_score;
    over = false;
    have_undo = false;
}

// ---------------------------------------------------------------- drawing
static void tile_colors(int v, int *bg, int *fg) {
    *fg = (v == 2 || v == 4) ? TERMPAINT_RGB_COLOR(80, 70, 60) : TUI_WHITE;
    switch (v) {
        case 0:    *bg = TERMPAINT_RGB_COLOR(42, 48, 60); *fg = TERMPAINT_RGB_COLOR(70, 78, 92); break;
        case 2:    *bg = TERMPAINT_RGB_COLOR(238, 228, 218); break;
        case 4:    *bg = TERMPAINT_RGB_COLOR(237, 224, 200); break;
        case 8:    *bg = TERMPAINT_RGB_COLOR(242, 177, 121); break;
        case 16:   *bg = TERMPAINT_RGB_COLOR(245, 149, 99);  break;
        case 32:   *bg = TERMPAINT_RGB_COLOR(246, 124, 95);  break;
        case 64:   *bg = TERMPAINT_RGB_COLOR(246, 94, 59);   break;
        case 128:  *bg = TERMPAINT_RGB_COLOR(237, 207, 114); break;
        case 256:  *bg = TERMPAINT_RGB_COLOR(237, 204, 97);  break;
        case 512:  *bg = TERMPAINT_RGB_COLOR(237, 200, 80);  break;
        case 1024: *bg = TERMPAINT_RGB_COLOR(237, 197, 63);  break;
        case 2048: *bg = TERMPAINT_RGB_COLOR(237, 194, 46);  break;
        default:   *bg = TERMPAINT_RGB_COLOR(60, 58, 50);    break;   // beyond 2048
    }
}

static void draw_tile(int x, int y, int tw, int th, int v) {
    int bg, fg;
    tile_colors(v, &bg, &fg);
    tui_fill(surface, x, y, tw, th, bg);
    if (v) {
        char s[12];
        snprintf(s, sizeof(s), "%d", v);
        int sw = (int)strlen(s);
        tui_text(surface, x + (tw - sw) / 2, y + th / 2, s, fg, bg);
    }
}

static void overlay(const char *msg, const char *hint) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    int bw = (int)strlen(msg) + 8;
    if (bw < (int)strlen(hint) + 6) bw = (int)strlen(hint) + 6;
    int bh = 5, bx = (w - bw) / 2, by = (h - bh) / 2;
    tui_panel(surface, bx, by, bw, bh, NULL, ACCENT, ACCENT, TUI_PANEL2);
    tui_text(surface, bx + (bw - tui_strwidth(msg)) / 2, by + 1, msg, ACCENT, TUI_PANEL2);
    tui_text(surface, bx + (bw - tui_strwidth(hint)) / 2, by + 3, hint, TUI_DIM, TUI_PANEL2);
}

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    tui_background(surface, TUI_BACK_PLASMA, anim_t, BACK1, BACK2);

    // title / score bar
    tui_fill(surface, 0, 0, w, 1, TUI_PANEL2);
    tui_text(surface, 1, 0, won ? "▦ 2048  ★ solved!" : "▦ 2048", ACCENT, TUI_PANEL2);
    char sc[80];
    snprintf(sc, sizeof(sc), "score %d   best %d ", score, best);
    tui_text(surface, w - tui_strwidth(sc) - 1, 0, sc, TUI_TEXT, TUI_PANEL2);

    // board geometry, centred
    int gap = 1;
    int tw = 9, th = 3;
    int boardw = N * tw + (N - 1) * gap;
    int boardh = N * th + (N - 1) * gap;
    while ((boardw > w - 4 || boardh > h - 5) && tw > 5) {
        tw--; if (th > 3 && boardh > h - 5) th--;
        boardw = N * tw + (N - 1) * gap;
        boardh = N * th + (N - 1) * gap;
    }
    int bx = (w - boardw) / 2;
    int by = (h - boardh) / 2 + 1;
    if (by < 2) by = 2;

    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            draw_tile(bx + c * (tw + gap), by + r * (th + gap), tw, th, grid[r][c]);

    // status bar
    tui_fill(surface, 0, h - 1, w, 1, TUI_PANEL2);
    tui_text_clip(surface, 1, h - 1, w - 2,
                  " ↑↓←→ / WASD / HJKL slide · u undo · n new game · q quit ",
                  TUI_DIM, TUI_PANEL2);

    if (over) overlay("Game over", "press  n  for a new game");
    else if (won) overlay("You made 2048!", "keep going · n for a new game");

    tui_backdrop(TUI_BACK_PLASMA, anim_t, BACK1, BACK2);
}

// ---------------------------------------------------------------- input
static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'n': case 'r': new_game(); break;
            case 'u': do_undo(); break;
            case 'w': case 'k': do_move(0); break;
            case 's': case 'j': do_move(1); break;
            case 'a': case 'h': do_move(2); break;
            case 'd': case 'l': do_move(3); break;
        }
        return;
    }
    if (event->type != TERMPAINT_EV_KEY) return;
    const char *a = event->key.atom;
    if (a == termpaint_input_arrow_up()) do_move(0);
    else if (a == termpaint_input_arrow_down()) do_move(1);
    else if (a == termpaint_input_arrow_left()) do_move(2);
    else if (a == termpaint_input_arrow_right()) do_move(3);
    else if (a == termpaint_input_escape()) quit_requested = true;
    else if (a == termpaint_input_enter() && over) new_game();
}

int main(void) {
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
#ifdef TUI_BUILD_GFX
    tui_gfx_detect("G2048");
#endif
    integration = termpaintx_full_integration_setup_terminal_fullscreen(
            "+kbdsig +kbdsigint", event_callback, NULL, &terminal);
    surface = termpaint_terminal_get_surface(terminal);
    termpaint_terminal_set_cursor_visible(terminal, false);
#ifdef TUI_BUILD_GFX
    tui_gfx_start("G2048");
#endif

    load_best();
    new_game();

    int frame_ms = 66;
    const char *fps = getenv("G2048_FPS");
    if (fps && atoi(fps) > 0) frame_ms = 1000 / atoi(fps);

    int timeout = frame_ms;
    while (!quit_requested) {
        tui_frame_begin(surface);
        redraw();
        tui_frame_end(surface, terminal);
        if (!termpaintx_full_integration_do_iteration_with_timeout(integration, &timeout)) {
            break;
        }
        if (timeout == 0) {
            anim_t += frame_ms / 1000.0;
            timeout = frame_ms;
        }
    }

    tui_gfx_stop();
    termpaint_terminal_free_with_restore(terminal);
    return 0;
}

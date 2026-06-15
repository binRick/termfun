// taskman — a menu-driven to-do list for your terminal, built on termpaint.
// Tasks live in a list you drive with the keyboard; the panel floats over an
// animated kitty-graphics backdrop on terminals that support it (cells
// everywhere else). Tasks persist to ~/.termfun-tasks between runs.
//
// SPDX-License-Identifier: 0BSD
//
//   ↑/↓ or j/k  move      Space/Enter  toggle done    a  add
//   e  edit     d/Del  delete          c  clear done
//   J/K or Shift+↑/↓  reorder          g/G  top/bottom   q/Esc  quit
//
// Build: see Makefile. taskman = cells; taskman-gfx = kitty backdrop.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "termpaint.h"
#include "termpaintx.h"

#include "tui.h"

#define MAX_TASKS 1000
#define ACCENT  TERMPAINT_RGB_COLOR(88, 166, 255)
#define SEL_BG  TERMPAINT_RGB_COLOR(31, 111, 235)
#define BACK1   TERMPAINT_RGB_COLOR(16, 22, 46)     // deep indigo (top)
#define BACK2   TERMPAINT_RGB_COLOR(34, 116, 128)   // teal (bottom)

typedef struct {
    char text[200];
    bool done;
} task;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static task tasks[MAX_TASKS];
static int ntasks;
static int sel;
static int top;
static int list_rows = 1;     // visible task rows, updated each redraw
static bool quit_requested;
static double anim_t;

static enum { MODE_LIST, MODE_ADD, MODE_EDIT } mode = MODE_LIST;
static tui_input input;

// ---------------------------------------------------------------- storage
static const char *task_path(void) {
    static char path[1024];
    const char *env = getenv("TASKMAN_FILE");
    if (env && *env) {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/.termfun-tasks", home ? home : ".");
    }
    return path;
}

static void add_task(const char *txt, bool done) {
    if (ntasks >= MAX_TASKS || !txt || !*txt) {
        return;
    }
    snprintf(tasks[ntasks].text, sizeof(tasks[ntasks].text), "%s", txt);
    tasks[ntasks].done = done;
    ntasks++;
}

static void seed_samples(void) {
    add_task("Welcome to taskman — your terminal to-do list", false);
    add_task("Press  a  to add a task, then type and hit Enter", false);
    add_task("Move with  up/down , toggle done with  Space", false);
    add_task("Edit with  e , delete with  d , reorder with  J/K", false);
    add_task("Star termfun on GitHub", true);
}

static void load_tasks(void) {
    FILE *f = fopen(task_path(), "r");
    if (!f) {
        seed_samples();
        return;
    }
    char line[256];
    while (ntasks < MAX_TASKS && fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = 0;
        }
        if (n < 2 || line[1] != ' ') {
            continue;
        }
        add_task(line + 2, line[0] == '1');
    }
    fclose(f);
}

static void save_tasks(void) {
    FILE *f = fopen(task_path(), "w");
    if (!f) {
        return;
    }
    for (int i = 0; i < ntasks; i++) {
        fprintf(f, "%d %s\n", tasks[i].done ? 1 : 0, tasks[i].text);
    }
    fclose(f);
}

// ---------------------------------------------------------------- actions
static void clamp_sel(void) {
    if (sel >= ntasks) {
        sel = ntasks - 1;
    }
    if (sel < 0) {
        sel = 0;
    }
}

static void toggle_done(void) {
    if (ntasks) {
        tasks[sel].done = !tasks[sel].done;
        save_tasks();
    }
}

static void delete_sel(void) {
    if (!ntasks) {
        return;
    }
    memmove(&tasks[sel], &tasks[sel + 1], (size_t)(ntasks - sel - 1) * sizeof(task));
    ntasks--;
    clamp_sel();
    save_tasks();
}

static void clear_done(void) {
    int w = 0;
    for (int i = 0; i < ntasks; i++) {
        if (!tasks[i].done) {
            tasks[w++] = tasks[i];
        }
    }
    ntasks = w;
    clamp_sel();
    save_tasks();
}

static void reorder(int dir) {
    int j = sel + dir;
    if (j < 0 || j >= ntasks) {
        return;
    }
    task tmp = tasks[sel];
    tasks[sel] = tasks[j];
    tasks[j] = tmp;
    sel = j;
    save_tasks();
}

static void commit_input(void) {
    if (mode == MODE_ADD && input.len > 0) {
        add_task(input.buf, false);
        sel = ntasks - 1;
    } else if (mode == MODE_EDIT && ntasks) {
        snprintf(tasks[sel].text, sizeof(tasks[sel].text), "%s", input.buf);
    }
    mode = MODE_LIST;
    save_tasks();
}

// ---------------------------------------------------------------- drawing
static void draw_progress(int x, int y, int w, int done, int total, int bg) {
    char label[32];
    int pct = total ? done * 100 / total : 0;
    snprintf(label, sizeof(label), " %d/%d  %d%%", done, total, pct);
    int lw = tui_strwidth(label);
    int bw = w - lw;
    if (bw < 4) {
        tui_text_clip(surface, x, y, w, label, TUI_DIM, bg);
        return;
    }
    int filled = total ? bw * done / total : 0;
    for (int i = 0; i < bw; i++) {
        tui_text(surface, x + i, y, i < filled ? "█" : "─",
                 i < filled ? TUI_OK : TUI_FAINT, bg);
    }
    tui_text(surface, x + bw, y, label, TUI_DIM, bg);
}

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    tui_background(surface, TUI_BACK_AURORA, anim_t, BACK1, BACK2);

    int done = 0;
    for (int i = 0; i < ntasks; i++) {
        done += tasks[i].done;
    }

    // title bar
    tui_fill(surface, 0, 0, w, 1, TUI_PANEL2);
    tui_text(surface, 1, 0, "◧ taskman", ACCENT, TUI_PANEL2);
    char counts[64];
    snprintf(counts, sizeof(counts), "%d tasks · %d done ", ntasks, done);
    tui_text(surface, w - tui_strwidth(counts) - 1, 0, counts, TUI_DIM, TUI_PANEL2);

    // centred panel with margins so the backdrop shows around it
    int mx = w / 6;
    if (mx < 4) mx = 4;
    if (mx > 24) mx = 24;
    int x0 = mx, y0 = 2;
    int pw = w - 2 * mx, ph = h - 4;
    if (pw < 24 || ph < 8) {                       // tiny terminal: use it all
        x0 = 0; y0 = 1; pw = w; ph = h - 2;
    }
    tui_panel(surface, x0, y0, pw, ph, "To-do", TUI_BORDER, TUI_WHITE, TUI_PANEL);

    int ix = x0 + 2, iw = pw - 4;
    draw_progress(ix, y0 + 1, iw, done, ntasks, TUI_PANEL);
    tui_hline(surface, x0 + 1, y0 + 2, pw - 2, TUI_BORDER, TUI_PANEL);

    int ly0 = y0 + 3;
    int input_row = y0 + ph - 2;
    list_rows = input_row - ly0;
    if (list_rows < 1) list_rows = 1;

    // keep the selection on screen
    if (sel < top) top = sel;
    if (sel >= top + list_rows) top = sel - list_rows + 1;
    if (top > ntasks - list_rows) top = ntasks - list_rows;
    if (top < 0) top = 0;

    if (ntasks == 0) {
        tui_text(surface, ix, ly0 + 1, "No tasks yet — press  a  to add one.",
                 TUI_DIM, TUI_PANEL);
    }
    for (int r = 0; r < list_rows; r++) {
        int i = top + r;
        if (i >= ntasks) {
            break;
        }
        int ry = ly0 + r;
        bool issel = (i == sel) && mode != MODE_ADD;
        int rbg = issel ? SEL_BG : TUI_PANEL;
        tui_fill(surface, x0 + 1, ry, pw - 2, 1, rbg);
        const char *box = tasks[i].done ? "✓" : "○";
        int boxfg = issel ? TUI_WHITE : (tasks[i].done ? TUI_OK : ACCENT);
        int txtfg = issel ? TUI_WHITE : (tasks[i].done ? TUI_DIM : TUI_TEXT);
        tui_text(surface, ix, ry, issel ? "▌" : " ", ACCENT, rbg);
        tui_text(surface, ix + 1, ry, box, boxfg, rbg);
        tui_text_clip(surface, ix + 3, ry, iw - 3, tasks[i].text, txtfg, rbg);
    }

    // scroll hint
    if (top + list_rows < ntasks) {
        tui_text(surface, x0 + pw - 3, input_row - 1, "▾", TUI_DIM, TUI_PANEL);
    }
    if (top > 0) {
        tui_text(surface, x0 + pw - 3, ly0, "▴", TUI_DIM, TUI_PANEL);
    }

    // input / hint bar inside the panel
    if (mode == MODE_ADD || mode == MODE_EDIT) {
        tui_fill(surface, x0 + 1, input_row, pw - 2, 1, SEL_BG);
        const char *lbl = mode == MODE_ADD ? " new ›" : " edit ›";
        int lx = tui_text(surface, x0 + 1, input_row, lbl, TUI_WHITE, SEL_BG);
        bool blink = ((int)(anim_t * 2)) & 1;
        char shown[300];
        snprintf(shown, sizeof(shown), " %s%s", input.buf, blink ? "▏" : " ");
        tui_text_clip(surface, x0 + 1 + lx, input_row, pw - 3 - lx, shown,
                      TUI_WHITE, SEL_BG);
    } else {
        tui_text_clip(surface, ix, input_row,
                      iw, "Space done · a add · e edit · d delete · J/K move",
                      TUI_FAINT, TUI_PANEL);
    }

    // status bar
    tui_fill(surface, 0, h - 1, w, 1, TUI_PANEL2);
    const char *help = (mode == MODE_LIST)
        ? " ↑↓ move · Space toggle · a add · e edit · d delete · c clear done · q quit "
        : " type your task · Enter save · Esc cancel ";
    tui_text_clip(surface, 1, h - 1, w - 2, help, TUI_DIM, TUI_PANEL2);

    tui_backdrop(TUI_BACK_AURORA, anim_t, BACK1, BACK2);
}

// ---------------------------------------------------------------- input
static void on_list_char(char c) {
    switch (c) {
        case 'q': quit_requested = true; break;
        case 'a': mode = MODE_ADD; tui_input_reset(&input); break;
        case 'e':
            if (ntasks) { mode = MODE_EDIT; tui_input_set(&input, tasks[sel].text); }
            break;
        case 'x': toggle_done(); break;
        case 'd': delete_sel(); break;
        case 'c': clear_done(); break;
        case 'j': sel++; clamp_sel(); break;
        case 'k': sel--; clamp_sel(); break;
        case 'J': reorder(1); break;
        case 'K': reorder(-1); break;
        case 'g': sel = 0; break;
        case 'G': sel = ntasks - 1; clamp_sel(); break;
    }
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR) {
        if (mode == MODE_LIST) {
            if (event->c.length == 1) {
                on_list_char(event->c.string[0]);
            }
        } else {
            tui_input_char(&input, event->c.string, event->c.length);
        }
        return;
    }
    if (event->type != TERMPAINT_EV_KEY) {
        return;
    }
    const char *a = event->key.atom;
    bool shift = event->key.modifier & TERMPAINT_MOD_SHIFT;
    if (mode == MODE_LIST) {
        if (a == termpaint_input_arrow_up()) {
            if (shift) reorder(-1); else { sel--; clamp_sel(); }
        } else if (a == termpaint_input_arrow_down()) {
            if (shift) reorder(1); else { sel++; clamp_sel(); }
        } else if (a == termpaint_input_space() || a == termpaint_input_enter()) {
            toggle_done();
        } else if (a == termpaint_input_delete()) {
            delete_sel();
        } else if (a == termpaint_input_home()) {
            sel = 0;
        } else if (a == termpaint_input_end()) {
            sel = ntasks - 1; clamp_sel();
        } else if (a == termpaint_input_page_up()) {
            sel -= list_rows; clamp_sel();
        } else if (a == termpaint_input_page_down()) {
            sel += list_rows; clamp_sel();
        } else if (a == termpaint_input_escape()) {
            quit_requested = true;
        }
    } else {
        if (a == termpaint_input_enter()) {
            commit_input();
        } else if (a == termpaint_input_escape()) {
            mode = MODE_LIST;
        } else if (a == termpaint_input_backspace()) {
            tui_input_backspace(&input);
        } else if (a == termpaint_input_space()) {
            tui_input_char(&input, " ", 1);
        }
    }
}

int main(void) {
#ifdef TUI_BUILD_GFX
    tui_gfx_detect("TASKMAN");
#endif
    integration = termpaintx_full_integration_setup_terminal_fullscreen(
            "+kbdsig +kbdsigint", event_callback, NULL, &terminal);
    surface = termpaint_terminal_get_surface(terminal);
    termpaint_terminal_set_cursor_visible(terminal, false);
#ifdef TUI_BUILD_GFX
    tui_gfx_start("TASKMAN");
#endif

    load_tasks();

    int frame_ms = 66;
    const char *fps = getenv("TASKMAN_FPS");
    if (fps && atoi(fps) > 0) {
        frame_ms = 1000 / atoi(fps);
    }

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

    save_tasks();
    tui_gfx_stop();
    termpaint_terminal_free_with_restore(terminal);
    return 0;
}

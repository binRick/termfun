// filer — a menu-driven file browser for your terminal, built on termpaint.
// A scrolling directory list on the left, a live preview/details pane on the
// right. The panels float over an animated kitty-graphics backdrop where
// supported (cells everywhere else).
//
// SPDX-License-Identifier: 0BSD
//
//   ↑/↓ or j/k  move        Enter/→  open dir      ←/Backspace/u  up
//   g/G  top/bottom         PgUp/PgDn  page        .  toggle hidden
//   r  refresh              q/Esc  quit
//
// Build: see Makefile. filer = cells; filer-gfx = kitty backdrop.
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "termpaint.h"
#include "termpaintx.h"

#include "tui.h"

#define MAX_ENTRIES 8192
#define ACCENT TERMPAINT_RGB_COLOR(86, 182, 220)
#define LINKC  TERMPAINT_RGB_COLOR(123, 200, 255)
#define SEL_BG TERMPAINT_RGB_COLOR(20, 84, 112)
#define BACK1  TERMPAINT_RGB_COLOR(13, 19, 30)      // slate (top)
#define BACK2  TERMPAINT_RGB_COLOR(26, 92, 112)     // cyan (bottom)

typedef struct {
    char name[256];
    bool isdir;
    bool islink;
    bool exec;
    off_t size;
    time_t mtime;
} entry;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static entry entries[MAX_ENTRIES];
static int nentries;
static int sel, top;
static int list_rows = 1;
static bool show_hidden;
static bool quit_requested;
static double anim_t;
static char cwd[PATH_MAX] = "/";

// preview cache (rebuilt when the selection changes)
static int preview_sel = -2;
static char pv_buf[8192];
static int pv_len;
static bool pv_binary;
static long pv_diritems = -1;

// ---------------------------------------------------------------- helpers
static void hsize(off_t n, char *out, size_t cap) {
    const char *u[] = {"B", "K", "M", "G", "T"};
    double d = (double)n;
    int i = 0;
    while (d >= 1024.0 && i < 4) {
        d /= 1024.0;
        i++;
    }
    if (i == 0) {
        snprintf(out, cap, "%lld B", (long long)n);
    } else {
        snprintf(out, cap, "%.1f%s", d, u[i]);
    }
}

static int cmp_entry(const void *a, const void *b) {
    const entry *x = a, *y = b;
    if (x->isdir != y->isdir) {
        return x->isdir ? -1 : 1;            // directories first
    }
    return strcasecmp(x->name, y->name);
}

static void build_list(void) {
    nentries = 0;
    DIR *d = opendir(".");
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && nentries < MAX_ENTRIES) {
        const char *nm = de->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) {
            continue;
        }
        if (!show_hidden && nm[0] == '.') {
            continue;
        }
        entry *e = &entries[nentries];
        snprintf(e->name, sizeof(e->name), "%s", nm);
        struct stat st, lst;
        e->islink = (lstat(nm, &lst) == 0 && S_ISLNK(lst.st_mode));
        if (stat(nm, &st) == 0) {
            e->isdir = S_ISDIR(st.st_mode);
            e->exec = !e->isdir && (st.st_mode & 0111);
            e->size = st.st_size;
            e->mtime = st.st_mtime;
        } else {
            e->isdir = false;
            e->exec = false;
            e->size = lst.st_size;
            e->mtime = lst.st_mtime;
        }
        nentries++;
    }
    closedir(d);
    qsort(entries, nentries, sizeof(entry), cmp_entry);
    if (sel >= nentries) sel = nentries - 1;
    if (sel < 0) sel = 0;
    top = 0;
    preview_sel = -2;
}

static void refresh_dir(void) {
    if (!getcwd(cwd, sizeof(cwd))) {
        snprintf(cwd, sizeof(cwd), "?");
    }
    build_list();
}

static void enter_dir(void) {
    if (nentries && entries[sel].isdir && chdir(entries[sel].name) == 0) {
        sel = 0;
        refresh_dir();
    }
}

static void go_up(void) {
    char cur[PATH_MAX];
    char want[256] = "";
    if (getcwd(cur, sizeof(cur))) {
        char *base = strrchr(cur, '/');
        if (base && base[1]) {
            snprintf(want, sizeof(want), "%s", base + 1);
        }
    }
    if (chdir("..") != 0) {
        return;
    }
    refresh_dir();
    for (int i = 0; want[0] && i < nentries; i++) {
        if (!strcmp(entries[i].name, want)) {
            sel = i;
            break;
        }
    }
}

static long count_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        return -1;
    }
    long n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
            n++;
        }
    }
    closedir(d);
    return n;
}

static void update_preview(void) {
    pv_len = 0;
    pv_binary = false;
    pv_diritems = -1;
    if (!nentries) {
        return;
    }
    entry *e = &entries[sel];
    if (e->isdir) {
        pv_diritems = count_dir(e->name);
        return;
    }
    int fd = open(e->name, O_RDONLY);
    if (fd < 0) {
        return;
    }
    ssize_t n = read(fd, pv_buf, sizeof(pv_buf) - 1);
    close(fd);
    if (n > 0) {
        pv_len = (int)n;
        pv_buf[pv_len] = 0;
        for (int i = 0; i < pv_len; i++) {
            if (pv_buf[i] == 0) {
                pv_binary = true;
                break;
            }
        }
    }
}

// copy one preview line into out, expanding tabs and dropping control bytes
static void sanitize(const char *src, int len, char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < len && o < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\t') {
            for (int s = 0; s < 4 && o < cap - 1; s++) out[o++] = ' ';
        } else if (c >= 0x20 || c >= 0x80) {
            out[o++] = (char)c;
        } else {
            out[o++] = ' ';
        }
    }
    out[o] = 0;
}

// ---------------------------------------------------------------- drawing
static void draw_list(int x, int y, int w, int h) {
    char title[64];
    snprintf(title, sizeof(title), "Files · %d", nentries);
    tui_panel(surface, x, y, w, h, title, TUI_BORDER, TUI_WHITE, TUI_PANEL);
    int ix = x + 2, iw = w - 4;
    int ly0 = y + 1;
    list_rows = h - 2;
    if (list_rows < 1) list_rows = 1;

    if (sel < top) top = sel;
    if (sel >= top + list_rows) top = sel - list_rows + 1;
    if (top > nentries - list_rows) top = nentries - list_rows;
    if (top < 0) top = 0;

    if (nentries == 0) {
        tui_text(surface, ix, ly0, "(empty directory)", TUI_DIM, TUI_PANEL);
        return;
    }
    for (int r = 0; r < list_rows; r++) {
        int i = top + r;
        if (i >= nentries) break;
        entry *e = &entries[i];
        int ry = ly0 + r;
        bool issel = (i == sel);
        int rbg = issel ? SEL_BG : TUI_PANEL;
        tui_fill(surface, x + 1, ry, w - 2, 1, rbg);
        const char *glyph = e->isdir ? "▸" : e->islink ? "↪" : " ";
        int fg = issel ? TUI_WHITE
               : e->isdir ? ACCENT : e->islink ? LINKC
               : e->exec ? TUI_OK : TUI_TEXT;
        tui_text(surface, ix, ry, glyph, issel ? TUI_WHITE : ACCENT, rbg);
        char nm[300];
        snprintf(nm, sizeof(nm), "%s%s", e->name, e->isdir ? "/" : "");
        // right-aligned size for files
        if (!e->isdir) {
            char sz[24];
            hsize(e->size, sz, sizeof(sz));
            int szw = tui_strwidth(sz);
            tui_text(surface, x + w - 2 - szw, ry, sz, issel ? TUI_WHITE : TUI_DIM, rbg);
            tui_text_clip(surface, ix + 2, ry, iw - 4 - szw, nm, fg, rbg);
        } else {
            tui_text_clip(surface, ix + 2, ry, iw - 4, nm, fg, rbg);
        }
    }
    if (top > 0) tui_text(surface, x + w - 3, ly0, "▴", TUI_DIM, TUI_PANEL);
    if (top + list_rows < nentries)
        tui_text(surface, x + w - 3, y + h - 2, "▾", TUI_DIM, TUI_PANEL);
}

static void draw_preview(int x, int y, int w, int h) {
    const char *ptitle = nentries ? entries[sel].name : "Preview";
    tui_panel(surface, x, y, w, h, ptitle, TUI_BORDER, ACCENT, TUI_PANEL);
    int ix = x + 2, iw = w - 4;
    int iy = y + 1;
    if (!nentries) {
        tui_text(surface, ix, iy, "Nothing selected.", TUI_DIM, TUI_PANEL);
        return;
    }
    entry *e = &entries[sel];
    char buf[256], when[40];
    struct tm tmv;
    time_t mt = e->mtime;
    localtime_r(&mt, &tmv);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);

    const char *kind = e->isdir ? "directory" : e->islink ? "symlink"
                     : e->exec ? "executable" : "file";
    snprintf(buf, sizeof(buf), "%s", kind);
    tui_text(surface, ix, iy, "type", TUI_DIM, TUI_PANEL);
    tui_text(surface, ix + 10, iy, buf, ACCENT, TUI_PANEL);
    if (!e->isdir) {
        char sz[24];
        hsize(e->size, sz, sizeof(sz));
        tui_text(surface, ix, iy + 1, "size", TUI_DIM, TUI_PANEL);
        tui_text(surface, ix + 10, iy + 1, sz, TUI_TEXT, TUI_PANEL);
    }
    tui_text(surface, ix, iy + 2, "modified", TUI_DIM, TUI_PANEL);
    tui_text(surface, ix + 10, iy + 2, when, TUI_TEXT, TUI_PANEL);
    tui_hline(surface, x + 1, iy + 3, w - 2, TUI_BORDER, TUI_PANEL);

    int by = iy + 4;
    int rows = (y + h - 1) - by;
    if (e->isdir) {
        if (pv_diritems < 0)
            tui_text(surface, ix, by, "(cannot read directory)", TUI_BAD, TUI_PANEL);
        else {
            snprintf(buf, sizeof(buf), "%ld item%s — press Enter to open",
                     pv_diritems, pv_diritems == 1 ? "" : "s");
            tui_text_clip(surface, ix, by, iw, buf, TUI_DIM, TUI_PANEL);
        }
        return;
    }
    if (pv_len == 0) {
        tui_text(surface, ix, by, "(empty file)", TUI_DIM, TUI_PANEL);
        return;
    }
    if (pv_binary) {
        tui_text(surface, ix, by, "(binary file — no preview)", TUI_WARN, TUI_PANEL);
        return;
    }
    // render text lines
    int line = 0;
    int i = 0;
    char out[1024];
    while (i < pv_len && line < rows) {
        int j = i;
        while (j < pv_len && pv_buf[j] != '\n') j++;
        sanitize(pv_buf + i, j - i, out, sizeof(out));
        tui_text_clip(surface, ix, by + line, iw, out, TUI_TEXT, TUI_PANEL);
        line++;
        i = j + 1;
    }
}

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    tui_background(surface, TUI_BACK_AURORA, anim_t, BACK1, BACK2);

    if (sel != preview_sel) {
        update_preview();
        preview_sel = sel;
    }

    // title bar with the current path
    tui_fill(surface, 0, 0, w, 1, TUI_PANEL2);
    int px = tui_text(surface, 1, 0, "▤ filer", ACCENT, TUI_PANEL2);
    tui_text_clip(surface, px + 2, 0, w - px - 3, cwd, TUI_DIM, TUI_PANEL2);

    int mx = w / 14;
    if (mx < 2) mx = 2;
    if (mx > 8) mx = 8;
    int x0 = mx, y0 = 2;
    int iw = w - 2 * mx, ph = h - 4;
    if (iw < 30 || ph < 6) { x0 = 0; y0 = 1; iw = w; ph = h - 2; }
    int lw = iw * 42 / 100;
    if (lw < 22) lw = 22;
    if (lw > iw - 16) lw = iw - 16;
    draw_list(x0, y0, lw, ph);
    draw_preview(x0 + lw + 1, y0, iw - lw - 1, ph);

    // status bar
    tui_fill(surface, 0, h - 1, w, 1, TUI_PANEL2);
    char st[160];
    snprintf(st, sizeof(st),
             " ↑↓ move · Enter open · ← up · . %s · r refresh · q quit ",
             show_hidden ? "hide dotfiles" : "show dotfiles");
    tui_text_clip(surface, 1, h - 1, w - 2, st, TUI_DIM, TUI_PANEL2);

    tui_backdrop(TUI_BACK_AURORA, anim_t, BACK1, BACK2);
}

// ---------------------------------------------------------------- input
static void clamp_sel(void) {
    if (sel >= nentries) sel = nentries - 1;
    if (sel < 0) sel = 0;
}

static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'j': sel++; clamp_sel(); break;
            case 'k': sel--; clamp_sel(); break;
            case 'g': sel = 0; break;
            case 'G': sel = nentries - 1; clamp_sel(); break;
            case 'u': case 'h': go_up(); break;
            case 'l': enter_dir(); break;
            case 'r': refresh_dir(); break;
            case '.': show_hidden = !show_hidden; refresh_dir(); break;
        }
        return;
    }
    if (event->type != TERMPAINT_EV_KEY) {
        return;
    }
    const char *a = event->key.atom;
    if (a == termpaint_input_arrow_up()) { sel--; clamp_sel(); }
    else if (a == termpaint_input_arrow_down()) { sel++; clamp_sel(); }
    else if (a == termpaint_input_arrow_right() || a == termpaint_input_enter()) enter_dir();
    else if (a == termpaint_input_arrow_left() || a == termpaint_input_backspace()) go_up();
    else if (a == termpaint_input_home()) sel = 0;
    else if (a == termpaint_input_end()) { sel = nentries - 1; clamp_sel(); }
    else if (a == termpaint_input_page_up()) { sel -= list_rows; clamp_sel(); }
    else if (a == termpaint_input_page_down()) { sel += list_rows; clamp_sel(); }
    else if (a == termpaint_input_escape()) quit_requested = true;
}

int main(void) {
#ifdef TUI_BUILD_GFX
    tui_gfx_detect("FILER");
#endif
    integration = termpaintx_full_integration_setup_terminal_fullscreen(
            "+kbdsig +kbdsigint", event_callback, NULL, &terminal);
    surface = termpaint_terminal_get_surface(terminal);
    termpaint_terminal_set_cursor_visible(terminal, false);
#ifdef TUI_BUILD_GFX
    tui_gfx_start("FILER");
#endif

    const char *start = getenv("FILER_DIR");
    if (start && *start) {
        if (chdir(start) != 0) { /* fall back to cwd */ }
    }
    refresh_dir();

    int frame_ms = 66;
    const char *fps = getenv("FILER_FPS");
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

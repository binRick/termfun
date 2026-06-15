// sysmon — a live system dashboard for your terminal, built on termpaint.
// CPU / memory / disk gauges, load average, uptime and the top processes,
// refreshing on a timer. Panels float over an animated kitty-graphics grid
// where supported (cells everywhere else).
//
// System stats come from mach/sysctl on macOS; other platforms show what is
// portably available (load average and disk).
//
// SPDX-License-Identifier: 0BSD
//
//   r  refresh now      +/-  refresh interval      q/Esc  quit
//
// Build: see Makefile. sysmon = cells; sysmon-gfx = kitty backdrop.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/param.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#include "termpaint.h"
#include "termpaintx.h"

#include "tui.h"

#define PROCN 64
#define ACCENT TERMPAINT_RGB_COLOR(88, 200, 160)
#define BACK1  TERMPAINT_RGB_COLOR(8, 18, 16)       // near-black green
#define BACK2  TERMPAINT_RGB_COLOR(40, 140, 104)    // grid glow

typedef struct {
    int pid;
    double cpu, mem;
    char name[64];
} proc;

static termpaint_integration *integration;
static termpaint_terminal *terminal;
static termpaint_surface *surface;

static bool quit_requested;
static double anim_t;
static double since_refresh = 1e9;     // force an immediate sample
static double interval = 1.5;

// sampled stats
static double cpu_pct, mem_pct, disk_pct;
static double mem_used_gb, mem_total_gb, disk_used_gb, disk_total_gb;
static double loadavg[3];
static long uptime_sec;
static int ncpu;
static char hostname[128] = "localhost";
static char osname[64] = "this machine";
static proc procs[PROCN];
static int nprocs;

#ifdef __APPLE__
static unsigned long long prev_total, prev_busy;
#endif

// ---------------------------------------------------------------- sampling
static void sample_cpu(void) {
#ifdef __APPLE__
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&info, &cnt) == KERN_SUCCESS) {
        unsigned long long busy = (unsigned long long)info.cpu_ticks[CPU_STATE_USER]
                                + info.cpu_ticks[CPU_STATE_SYSTEM]
                                + info.cpu_ticks[CPU_STATE_NICE];
        unsigned long long total = busy + info.cpu_ticks[CPU_STATE_IDLE];
        unsigned long long dt = total - prev_total, db = busy - prev_busy;
        if (dt > 0 && prev_total) {
            cpu_pct = 100.0 * (double)db / (double)dt;
        }
        prev_total = total;
        prev_busy = busy;
    }
#endif
}

static void sample_mem(void) {
#ifdef __APPLE__
    int64_t total = 0;
    size_t len = sizeof(total);
    sysctlbyname("hw.memsize", &total, &len, NULL, 0);
    vm_size_t page = 4096;
    host_page_size(mach_host_self(), &page);
    vm_statistics64_data_t vm;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &cnt) == KERN_SUCCESS) {
        unsigned long long avail = (unsigned long long)(vm.free_count + vm.inactive_count) * page;
        unsigned long long used = (unsigned long long)total > avail
                                ? (unsigned long long)total - avail : 0;
        mem_total_gb = total / 1e9;
        mem_used_gb = used / 1e9;
        mem_pct = total ? 100.0 * (double)used / (double)total : 0;
    }
#endif
}

static void sample_disk(void) {
    struct statfs s;
    if (statfs("/", &s) == 0) {
        double total = (double)s.f_blocks * s.f_bsize;
        double avail = (double)s.f_bavail * s.f_bsize;
        double used = total - avail;
        disk_total_gb = total / 1e9;
        disk_used_gb = used / 1e9;
        disk_pct = total ? 100.0 * used / total : 0;
    }
}

static void sample_meta(void) {
    getloadavg(loadavg, 3);
    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = 0;
#ifdef __APPLE__
    size_t l = sizeof(ncpu);
    sysctlbyname("hw.ncpu", &ncpu, &l, NULL, 0);
    char ver[32] = "";
    l = sizeof(ver);
    if (sysctlbyname("kern.osproductversion", ver, &l, NULL, 0) == 0 && ver[0]) {
        snprintf(osname, sizeof(osname), "macOS %s", ver);
    }
    struct timeval bt;
    l = sizeof(bt);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &bt, &l, NULL, 0) == 0) {
        uptime_sec = (long)(time(NULL) - bt.tv_sec);
    }
#else
    ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

static void sample_procs(void) {
    nprocs = 0;
    FILE *f = popen("ps -Aceo pid=,pcpu=,pmem=,comm= -r 2>/dev/null", "r");
    if (!f) {
        return;
    }
    char line[512];
    while (nprocs < PROCN && fgets(line, sizeof(line), f)) {
        proc p;
        memset(&p, 0, sizeof(p));
        if (sscanf(line, "%d %lf %lf %63[^\n]", &p.pid, &p.cpu, &p.mem, p.name) >= 4) {
            char *n = p.name;
            while (*n == ' ') {
                memmove(p.name, p.name + 1, strlen(p.name));   // trim leading space
            }
            procs[nprocs++] = p;
        }
    }
    pclose(f);
}

static void sample_all(void) {
    sample_cpu();
    sample_mem();
    sample_disk();
    sample_meta();
    sample_procs();
}

// ---------------------------------------------------------------- drawing
static int gauge_color(double pct) {
    return pct < 60 ? TUI_OK : pct < 85 ? TUI_WARN : TUI_BAD;
}

static void draw_gauge(int x, int y, int w, const char *label, double pct,
                       const char *detail) {
    tui_text(surface, x, y, label, TUI_DIM, TUI_PANEL);
    int bx = x + 6;
    char tail[40];
    snprintf(tail, sizeof(tail), "%5.1f%% %s", pct, detail ? detail : "");
    int tw = tui_strwidth(tail);
    int bw = w - 6 - tw - 1;
    if (bw < 4) {
        tui_text_clip(surface, bx, y, w - 6, tail, TUI_TEXT, TUI_PANEL);
        return;
    }
    int filled = (int)(bw * pct / 100.0 + 0.5);
    if (filled > bw) filled = bw;
    if (filled < 0) filled = 0;
    int col = gauge_color(pct);
    for (int i = 0; i < bw; i++) {
        tui_text(surface, bx + i, y, i < filled ? "█" : "─",
                 i < filled ? col : TUI_FAINT, TUI_PANEL);
    }
    tui_text(surface, bx + bw + 1, y, tail, TUI_TEXT, TUI_PANEL);
}

static void draw_system(int x, int y, int w, int h) {
    tui_panel(surface, x, y, w, h, "System", TUI_BORDER, ACCENT, TUI_PANEL);
    int ix = x + 2, iw = w - 4, iy = y + 1;
    char d[40];

    snprintf(d, sizeof(d), "%.1f/%.0fG", mem_used_gb, mem_total_gb);
    char dd[40];
    snprintf(dd, sizeof(dd), "%.0f/%.0fG", disk_used_gb, disk_total_gb);
    draw_gauge(ix, iy, iw, "cpu", cpu_pct, "");
    draw_gauge(ix, iy + 1, iw, "mem", mem_pct, d);
    draw_gauge(ix, iy + 2, iw, "disk", disk_pct, dd);
    tui_hline(surface, x + 1, iy + 3, w - 2, TUI_BORDER, TUI_PANEL);

    char buf[120];
    snprintf(buf, sizeof(buf), "load   %.2f  %.2f  %.2f", loadavg[0], loadavg[1], loadavg[2]);
    tui_text_clip(surface, ix, iy + 4, iw, buf, TUI_TEXT, TUI_PANEL);

    long up = uptime_sec;
    long dys = up / 86400, hrs = (up % 86400) / 3600, mins = (up % 3600) / 60;
    if (up > 0) {
        snprintf(buf, sizeof(buf), "uptime %ldd %ldh %ldm", dys, hrs, mins);
    } else {
        snprintf(buf, sizeof(buf), "uptime n/a");
    }
    tui_text_clip(surface, ix, iy + 5, iw, buf, TUI_TEXT, TUI_PANEL);

    snprintf(buf, sizeof(buf), "cores  %d", ncpu);
    tui_text_clip(surface, ix, iy + 6, iw, buf, TUI_TEXT, TUI_PANEL);

    tui_text_clip(surface, ix, iy + 7, iw, osname, TUI_DIM, TUI_PANEL);
}

static void draw_procs(int x, int y, int w, int h) {
    tui_panel(surface, x, y, w, h, "Top processes (by CPU)", TUI_BORDER, ACCENT, TUI_PANEL);
    int ix = x + 2, iw = w - 4, iy = y + 1;
    char hdr[120];
    snprintf(hdr, sizeof(hdr), "%6s %6s %6s  %s", "PID", "CPU%", "MEM%", "COMMAND");
    tui_text_clip(surface, ix, iy, iw, hdr, TUI_DIM, TUI_PANEL);
    tui_hline(surface, x + 1, iy + 1, w - 2, TUI_BORDER, TUI_PANEL);
    int rows = h - 4;
    for (int i = 0; i < rows && i < nprocs; i++) {
        char line[200];
        snprintf(line, sizeof(line), "%6d %6.1f %6.1f  %s",
                 procs[i].pid, procs[i].cpu, procs[i].mem, procs[i].name);
        int col = procs[i].cpu >= 50 ? TUI_WARN : TUI_TEXT;
        tui_text_clip(surface, ix, iy + 2 + i, iw, line, col, TUI_PANEL);
    }
}

static void redraw(void) {
    int w = termpaint_surface_width(surface);
    int h = termpaint_surface_height(surface);
    tui_background(surface, TUI_BACK_GRID, anim_t, BACK1, BACK2);

    // title bar
    tui_fill(surface, 0, 0, w, 1, TUI_PANEL2);
    int px = tui_text(surface, 1, 0, "▤ sysmon", ACCENT, TUI_PANEL2);
    tui_text_clip(surface, px + 2, 0, w - px - 3, hostname, TUI_DIM, TUI_PANEL2);

    int mx = w / 12;
    if (mx < 2) mx = 2;
    if (mx > 8) mx = 8;
    int x0 = mx, y0 = 2, iw = w - 2 * mx, ph = h - 4;
    if (iw < 40 || ph < 8) { x0 = 0; y0 = 1; iw = w; ph = h - 2; }
    int lw = 40;
    if (lw > iw - 24) lw = iw - 24;
    if (lw < 24) lw = 24;
    int sysh = 11;
    if (sysh > ph) sysh = ph;
    draw_system(x0, y0, lw, sysh);
    draw_procs(x0 + lw + 1, y0, iw - lw - 1, ph);

    // status bar
    tui_fill(surface, 0, h - 1, w, 1, TUI_PANEL2);
    char st[120];
    snprintf(st, sizeof(st), " r refresh · +/- interval %.1fs · q quit ", interval);
    tui_text_clip(surface, 1, h - 1, w - 2, st, TUI_DIM, TUI_PANEL2);

    tui_backdrop(TUI_BACK_GRID, anim_t, BACK1, BACK2);
}

// ---------------------------------------------------------------- input
static void event_callback(void *userdata, termpaint_event *event) {
    (void)userdata;
    if (event->type == TERMPAINT_EV_CHAR && event->c.length == 1) {
        switch (event->c.string[0]) {
            case 'q': quit_requested = true; break;
            case 'r': since_refresh = 1e9; break;
            case '+': case '=': if (interval < 10) interval += 0.5; break;
            case '-': if (interval > 0.5) interval -= 0.5; break;
        }
        return;
    }
    if (event->type == TERMPAINT_EV_KEY && event->key.atom == termpaint_input_escape()) {
        quit_requested = true;
    }
}

int main(void) {
#ifdef TUI_BUILD_GFX
    tui_gfx_detect("SYSMON");
#endif
    integration = termpaintx_full_integration_setup_terminal_fullscreen(
            "+kbdsig +kbdsigint", event_callback, NULL, &terminal);
    surface = termpaint_terminal_get_surface(terminal);
    termpaint_terminal_set_cursor_visible(terminal, false);
#ifdef TUI_BUILD_GFX
    tui_gfx_start("SYSMON");
#endif

    const char *iv = getenv("SYSMON_INTERVAL");
    if (iv && atof(iv) >= 0.5) interval = atof(iv);

    int frame_ms = 66;
    const char *fps = getenv("SYSMON_FPS");
    if (fps && atoi(fps) > 0) frame_ms = 1000 / atoi(fps);

    int timeout = frame_ms;
    while (!quit_requested) {
        if (since_refresh >= interval) {
            sample_all();
            since_refresh = 0;
        }
        tui_frame_begin(surface);
        redraw();
        tui_frame_end(surface, terminal);
        if (!termpaintx_full_integration_do_iteration_with_timeout(integration, &timeout)) {
            break;
        }
        if (timeout == 0) {
            anim_t += frame_ms / 1000.0;
            since_refresh += frame_ms / 1000.0;
            timeout = frame_ms;
        }
    }

    tui_gfx_stop();
    termpaint_terminal_free_with_restore(terminal);
    return 0;
}

// Standalone kitty graphics protocol probe — dumps the raw terminal response.
// Build: cc -o build/kitty_probe kitty_probe.c
// Run:   ./build/kitty_probe
// SPDX-License-Identifier: 0BSD

#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static void dump_escaped(const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0x1b)       { printf("\\e"); }
        else if (c == 0x9c)  { printf("\\ST"); }  // 8-bit ST
        else if (c == 0x9b)  { printf("\\CSI"); } // 8-bit CSI
        else if (c == '_')   { printf("_"); }
        else if (c >= 32 && c < 127) { putchar(c); }
        else                 { printf("\\x%02x", c); }
    }
    putchar('\n');
}

int main(void) {
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/tty");
        return 1;
    }

    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
        fprintf(stderr, "terminal: %dx%d cells, %dx%d pixels\n",
                ws.ws_col, ws.ws_row, ws.ws_xpixel, ws.ws_ypixel);
        if (ws.ws_xpixel == 0 || ws.ws_ypixel == 0) {
            fprintf(stderr, "WARNING: pixel dimensions are 0 — "
                    "terminal may not report them\n");
        }
    }

    struct termios saved, raw;
    if (tcgetattr(fd, &saved) != 0) { perror("tcgetattr"); return 1; }
    raw = saved;
    cfmakeraw(&raw);
    tcsetattr(fd, TCSAFLUSH, &raw);

    // Send: validate-only 1x1 RGB image (a=q) + DA1 sentinel (\e[c)
    const char *probe = "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\\x1b[c";
    fprintf(stderr, "sending probe: ");
    dump_escaped(probe, strlen(probe));
    write(fd, probe, strlen(probe));

    char buf[2048];
    size_t n = 0;
    int rounds = 0;
    bool got_graphics = false;
    bool got_da1 = false;

    while (!got_da1 && rounds < 20) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 500);
        if (ready <= 0) {
            fprintf(stderr, "poll timeout (round %d)\n", rounds);
            break;
        }
        ssize_t r = read(fd, buf + n, sizeof(buf) - 1 - n);
        if (r <= 0) {
            fprintf(stderr, "read returned %zd (errno=%d)\n", r, errno);
            break;
        }
        n += (size_t)r;
        buf[n] = 0;
        rounds++;

        fprintf(stderr, "received %zd bytes (total %zu): ", r, n);
        dump_escaped(buf + n - (size_t)r, (size_t)r);

        if (strstr(buf, "_Gi=31;OK")) { got_graphics = true; }
        if (strstr(buf, "_Gi=31;ENOTSUPPORTED") ||
            strstr(buf, "_Gi=31;ERROR")) {
            fprintf(stderr, "terminal explicitly rejected graphics\n");
        }

        // DA1 reply is ESC [ ? ... c
        const char *p = buf;
        while ((p = strstr(p, "\x1b[?")) != NULL) {
            const char *end = strchr(p + 3, 'c');
            if (end) { got_da1 = true; break; }
            p += 3;
        }
    }

    tcsetattr(fd, TCSAFLUSH, &saved);
    close(fd);

    fprintf(stderr, "\n--- result ---\n");
    fprintf(stderr, "got kitty graphics reply : %s\n", got_graphics ? "YES" : "NO");
    fprintf(stderr, "got DA1 sentinel         : %s\n", got_da1 ? "YES" : "NO");
    fprintf(stderr, "kitty_detect() would return: %s\n",
            (got_graphics && got_da1) ? "true (pixel mode ENABLED)" :
            got_da1 ? "false (FALLBACK to cell mode)" :
            "false — DA1 never arrived (terminal not responding?)");
    return (got_graphics && got_da1) ? 0 : 1;
}

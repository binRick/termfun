# termfun

Small terminal demos written in C.

## Conventions

- Demos must use termpaint (vendored submodule in `termpaint/`) for terminal setup, input, and cell rendering — do not hand-roll escape sequences or use other TUI libraries.
- For pixel effects, use the kitty graphics layer in `kitty_gfx.c`/`kitty_gfx.h` alongside termpaint; it presents an RGB framebuffer underneath the text layer (see `fireworks-gfx.c` for the pattern).
- Build with `make`; binaries land in `build/`. Add new demos as targets in the Makefile following the existing `fireworks` rules.
- Every demo ships as a `<demo>.c` / `<demo>-gfx.c` pair plus matching `<demo>.sh` / `<demo>-gfx.sh` launchers at the repo root (copy `fireworks.sh`: fetch the termpaint submodule if missing, `make`, `exec` the binary).

### Menu-driven TUI apps

Interactive apps (`taskman`, `filer`, `2048`, `sysmon`) are still termpaint cell
programs; they just share a small toolkit and add a decorative pixel layer.

- They build on `tui.c` / `tui.h`: panels, lists, a single-line text input, a
  colour theme, and the kitty backdrop engine. The backdrop fills the
  framebuffer everywhere the app did *not* draw a panel (a per-cell cut-out
  mask → alpha 0 there), so the UI reads as floating over an animated
  wallpaper. In cell mode `tui_background` paints a quiet dark gradient
  instead — the lively wallpaper is the kitty mode's job.
- DRY pair convention: `<app>.c` is the whole program and renders cells;
  `<app>-gfx.c` is a one-line shim — `#define TUI_BUILD_GFX 1` then
  `#include "<app>.c"` — that turns on the kitty backdrop. Both binaries fall
  back to cells on terminals without graphics support.
- Each app honours `<APP>_CELLS` / `<APP>_MAXDIM` / `<APP>_FPS` like the
  effects, plus app-specific vars: `TASKMAN_FILE`, `FILER_DIR`, `G2048_FILE`,
  `SYSMON_INTERVAL`. 2048's env prefix is `G2048` because env var names can't
  begin with a digit.

## README recordings

Every demo gets a README section with BOTH rendering modes broken out, each
shown as a RECORDING of the real demo running in iTerm2 (never mockups,
synthetic images, or static stills):

- `docs/<demo>-kitty.gif` — kitty pixel mode (`build/<demo>-gfx`)
- `docs/<demo>-cells.gif` — ASCII cell mode (`build/<demo>`, with `*_CELLS=1`
  if the cell binary also has a graphics layer)

README layout per demo: `### <demo>` intro, then `#### kitty graphics` and
`#### ASCII cells` subsections, each with its recording. The hero GIF at the
top reuses the flagship demo's kitty recording. Also extend the Controls and
Tuning tables and Project layout when adding a demo.

### Recording (tools/record_iterm.py)

```sh
python3 tools/record_iterm.py '<ENV=...> ./build/<demo>-gfx' docs/<demo>-kitty.gif \
    --dur 12 --warmup 4 --skip 2 --key '0.5: ' --key '4.0:c'
```

The tool opens an iTerm2 window, launches the demo, captures frames via
`screencapture` run from a second iTerm2 window, and assembles a GIF with
real frame timing. Quirks it already handles (don't regress them):

- The capture loop must run INSIDE iTerm2 — macOS screen-recording permission
  (TCC) belongs to iTerm2, not to this shell. Permission is already granted.
- iTerm2 throttles redraws of unfocused windows (recordings collapse into a
  slideshow), so the tool refocuses the demo window after the recorder starts.
- `write text` into a freshly created window races shell startup and gets
  lost; the tool waits and retries.

Per-demo tuning: `--warmup` covers termpaint's detection (~2s) plus scene
build-up; `--skip` drops dull lead-in from the GIF (fireworks rockets need
~3.5s to apex — volley early via `--key`, then skip the climb). Record at
`--fps 10` (the house playback rate) and verify the assembled GIF really hit
it: PIL merges duplicate frames, so `n_frames` shrinking and avg duration
exceeding 100ms means iTerm2 couldn't render that fast. At `*_MAXDIM=1024`
iTerm2 decodes the ~3 MB kitty payloads at only ~7.5 fps — use
`*_MAXDIM=768` for smooth motion (still ~1:1 with the 800px GIF). Trim size
with `--colors 80-96`; dense scenes land around 4-5 MB per GIF.

Full-frame gradient scenes (e.g. ripples' water) overshoot that even at
`--colors 96`; an ffmpeg palette post-pass shrinks them ~3x with no visible
loss (keep the recording's fps, record to a tmp file first):

```sh
ffmpeg -t 7 -i /tmp/raw.gif -filter_complex \
  '[0:v]split[b][c];[b]palettegen=max_colors=40:stats_mode=diff[p];[c][p]paletteuse=dither=none:diff_mode=rectangle' \
  docs/<demo>-kitty.gif
```

There is also a headless harness from before the recordings existed:
`tools/capture_kitty.py` + `tools/compose_kitty.py` decode stills straight
from the demo's kitty protocol stream in a PTY, and `tools/render_cells.py`
renders tmux `capture-pane -e` dumps. Useful for stills or machines without
iTerm2/TCC.

The interactive apps need keystrokes, not just time. Drive a recording with
`record_iterm.py --key '2.0:a' --key '2.4:Buy milk' ...` (the warmup must
clear termpaint's ~2s auto-detection — the first keystroke after it is
otherwise swallowed). For headless smoke tests, `tools/drive_tui.py <prog>
'<keys>'` runs an app in a PTY and feeds it a `;`-separated key script
(supports `UP`/`DOWN`/`\r`/`SP`/`WAIT`), then reports the exit status — set
`TUI_WARMUP=5` and `<APP>_CELLS=1`. For faithful cell-mode screenshots prefer
`tmux capture-pane -p` (real terminal emulation): pyte mis-measures the
box-drawing/symbol glyph widths and skews these panel layouts.

Python deps: `pip install pyte pillow` (`--break-system-packages` on this
machine's Homebrew Python).

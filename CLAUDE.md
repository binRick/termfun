# termfun

Small terminal demos written in C.

## Conventions

- Demos must use termpaint (vendored submodule in `termpaint/`) for terminal setup, input, and cell rendering — do not hand-roll escape sequences or use other TUI libraries.
- For pixel effects, use the kitty graphics layer in `kitty_gfx.c`/`kitty_gfx.h` alongside termpaint; it presents an RGB framebuffer underneath the text layer (see `fireworks-gfx.c` for the pattern).
- Build with `make`; binaries land in `build/`. Add new demos as targets in the Makefile following the existing `fireworks` rules.

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
~3.5s to apex — volley early via `--key`, then skip the climb). Keep files
small for dense scenes with `--fps 6 --colors 96`; use `*_MAXDIM=1024` for
sharp pixel frames. Aim for ≤3 MB per GIF.

There is also a headless harness from before the recordings existed:
`tools/capture_kitty.py` + `tools/compose_kitty.py` decode stills straight
from the demo's kitty protocol stream in a PTY, and `tools/render_cells.py`
renders tmux `capture-pane -e` dumps. Useful for stills or machines without
iTerm2/TCC.

Python deps: `pip install pyte pillow` (`--break-system-packages` on this
machine's Homebrew Python).

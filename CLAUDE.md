# termfun

Small terminal demos written in C.

## Conventions

- Demos must use termpaint (vendored submodule in `termpaint/`) for terminal setup, input, and cell rendering — do not hand-roll escape sequences or use other TUI libraries.
- For pixel effects, use the kitty graphics layer in `kitty_gfx.c`/`kitty_gfx.h` alongside termpaint; it presents an RGB framebuffer underneath the text layer (see `fireworks-gfx.c` for the pattern).
- Build with `make`; binaries land in `build/`. Add new demos as targets in the Makefile following the existing `fireworks` rules.

## README screenshots

Every demo gets a README section with BOTH rendering modes broken out, using
real captures (never mockups or synthetic images):

- `docs/<demo>-kitty.png` — kitty pixel mode, decoded from the demo's actual
  graphics protocol stream
- `docs/<demo>-cells.png` — ASCII cell mode, captured from real terminal output
- `docs/<demo>.gif` — optional animated clip (kitty mode); the repo hero GIF
  at the top of the README

README layout per demo: `### <demo>` intro, then `#### kitty graphics` and
`#### ASCII cells` subsections, each with its screenshot. See the fireworks
section for the pattern.

### Regenerating screenshots (tools/)

Kitty mode — `tools/capture_kitty.py build/<demo>-gfx` runs the demo in a PTY
that impersonates a kitty terminal (answers the graphics probe and termpaint's
detection queries) and decodes the transmitted RGBA frames into
`/tmp/gfx_frames/`. Then `tools/compose_kitty.py <demo>` scores the frames,
composites them over the night background, overlays the real status bar, and
writes the still + GIF into `docs/`.

Cell mode — run `build/<demo>` in tmux, dump with `tmux capture-pane -p -e`,
render with `tools/render_cells.py` (usage in its docstring). Sample several
frames and keep the densest one.

Capture quirks worth knowing:

- The pty throttles multi-MB kitty frames to ~2.5 fps regardless of
  `FIREWORKS_FPS`, so sim time runs slower than wall time — schedule scripted
  input by captured frame count, not wall clock (see `INPUTS` in
  `capture_kitty.py`, which needs adjusting per demo).
- For burst-style demos, send input volleys and account for in-flight time
  (fireworks rockets need 15-25 frames to reach apex) so the still catches
  the payoff. Pick the best frame by scoring, not the first one that looks
  non-empty.
- Python deps: `pip install pyte pillow` (`--break-system-packages` on this
  machine's Homebrew Python).

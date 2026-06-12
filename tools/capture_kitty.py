#!/usr/bin/env python3
"""Capture real pixel frames from a demo by running it in a PTY.

Impersonates a kitty-graphics-capable terminal: answers the kitty probe
and termpaint's detection queries, then decodes the chunked base64 RGBA
frames the demo transmits. Frames land in /tmp/gfx_frames/ for
compose_kitty.py to turn into README images.

Usage: python3 tools/capture_kitty.py [build/fireworks-gfx]

Tune INPUTS for the demo being captured. Schedule by captured frame
count, not wall time: the pty caps throughput well below the demo's
target fps, so simulation time runs slower than wall time.
"""

import base64
import fcntl
import os
import pty
import re
import select
import struct
import sys
import termios
import time

from PIL import Image

BINARY = sys.argv[1] if len(sys.argv) > 1 else "build/fireworks-gfx"
COLS, ROWS = 140, 40
XPIX, YPIX = 1400, 800
RUN_SECONDS = 45.0   # wall-clock backstop; normally we quit by frame count
QUIT_FRAME = 80      # each frame = 0.1 sim seconds at FIREWORKS_FPS=10
ERRLOG = "/tmp/gfx_child_err.txt"
FRAME_DIR = "/tmp/gfx_frames"

# (frame_number, bytes_to_send) — demo-specific input script.
# fireworks: '+' raises the auto rate; volleys of spaces launch rockets
# that fly 15-25 frames before bursting, so volleys at frames 8 and 38
# put peak bursts around frames 25-35 and 55-65.
INPUTS = [(3, b'+++')]
for _volley_f in (8, 38):
    for _ in range(12):
        INPUTS.append((_volley_f, b' '))
INPUTS.sort(key=lambda x: x[0])

QUERY_RESPONSES = [
    (re.compile(rb'\x1b\[c'),            b'\x1b[?62;4c'),                 # DA1
    (re.compile(rb'\x1b\[>0?c'),         b'\x1b[>41;370;0c'),             # DA2
    (re.compile(rb'\x1b\[=0?c'),         b'\x1bP!|00000000\x1b\\'),       # DA3
    (re.compile(rb'\x1b\[5n'),           b'\x1b[0n'),                     # DSR
    (re.compile(rb'\x1b\[6n'),           b'\x1b[1;1R'),                   # CPR
    (re.compile(rb'\x1b\[\?6n'),         b'\x1b[?1;1;1R'),                # DECXCPR
    (re.compile(rb'\x1b\[>0?q'),         b'\x1bP>|xterm(370)\x1b\\'),     # XTVERSION
    (re.compile(rb'\x1b\[\?u'),          b'\x1b[?0u'),                    # kitty kbd
    (re.compile(rb'\x1bP\+q[0-9a-fA-F;]*\x1b\\'), b'\x1bP0+r\x1b\\'),     # XTGETTCAP
    (re.compile(rb'\x1b\[14t'),          b'\x1b[4;%d;%dt' % (YPIX, XPIX)),
    (re.compile(rb'\x1b\[16t'),          b'\x1b[6;%d;%dt' % (YPIX // ROWS, XPIX // COLS)),
    (re.compile(rb'\x1b\[18t'),          b'\x1b[8;%d;%dt' % (ROWS, COLS)),
    (re.compile(rb'\x1b\]10;\?(?:\x1b\\|\x07)'), b'\x1b]10;rgb:c8c8/c8c8/dcdc\x1b\\'),
    (re.compile(rb'\x1b\]11;\?(?:\x1b\\|\x07)'), b'\x1b]11;rgb:0505/0505/1010\x1b\\'),
]

def parse_apc_control(control: bytes) -> dict:
    out = {}
    for part in control.split(b','):
        if b'=' in part:
            k, v = part.split(b'=', 1)
            out[k.decode()] = v.decode()
    return out

def main():
    pid, master = pty.fork()
    if pid == 0:
        err = os.open(ERRLOG, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        os.dup2(err, 2)
        os.environ['TERM'] = 'xterm-kitty'
        os.environ['FIREWORKS_MAXDIM'] = '1024'
        os.environ['FIREWORKS_FPS'] = '10'
        os.execv(BINARY, [BINARY])

    fcntl.ioctl(master, termios.TIOCSWINSZ,
                struct.pack('HHHH', ROWS, COLS, XPIX, YPIX))
    os.set_blocking(master, False)

    buf = b""           # unparsed tail (possible split APC)
    cell_stream = b""   # stream minus APC sequences
    answered = set()    # match offsets already responded to

    frame_meta = None
    frame_chunks = []
    frames = []
    exit_reason = "time"
    inputs = list(INPUTS)
    quit_sent = False

    t0 = time.time()

    def respond(data: bytes):
        try:
            os.write(master, data)
        except OSError:
            pass

    def handle_apc(seq: bytes):
        nonlocal frame_meta, frame_chunks
        body = seq[2:-2]
        if not body.startswith(b'G'):
            return
        control, _, payload = body[1:].partition(b';')
        meta = parse_apc_control(control)
        if meta.get('a') == 'q':
            ident = meta.get('i', '31')
            respond(b'\x1b_Gi=%s;OK\x1b\\' % ident.encode())
            return
        if meta.get('a') == 'T':
            frame_meta = meta
            frame_chunks = [payload]
        elif frame_meta is not None and 'm' in meta:
            frame_chunks.append(payload)
        else:
            return
        if meta.get('m', '0') == '0' and frame_meta is not None:
            try:
                raw = base64.b64decode(b''.join(frame_chunks))
            except Exception:
                raw = b''
            w, h = int(frame_meta['s']), int(frame_meta['v'])
            if len(raw) == w * h * 4:
                frames.append((time.time() - t0, dict(frame_meta), raw, len(cell_stream)))
            frame_meta = None
            frame_chunks = []

    while True:
        now = time.time() - t0
        while inputs and inputs[0][0] <= len(frames):
            _, keys = inputs.pop(0)
            respond(keys)
        if (len(frames) >= QUIT_FRAME or now > RUN_SECONDS) and not quit_sent:
            respond(b'q')
            quit_sent = True
        if now > RUN_SECONDS + 3:
            break

        r, _, _ = select.select([master], [], [], 0.05)
        if master in r:
            # drain greedily: the child blocks on its multi-MB frame writes
            # until we empty the pty, so slow reads throttle the simulation
            chunks = []
            eof = False
            while True:
                try:
                    data = os.read(master, 1 << 20)
                except BlockingIOError:
                    break
                except OSError:
                    eof = True
                    break
                if not data:
                    eof = True
                    break
                chunks.append(data)
            if eof and not chunks:
                exit_reason = f"EOF at t={now:.2f}"
                break
            buf += b''.join(chunks)

            pos = 0
            while pos < len(buf):
                i = buf.find(b'\x1b_', pos)
                if i < 0:
                    # flush all but a possible trailing lone ESC
                    keep = len(buf) - 1 if buf.endswith(b'\x1b') else len(buf)
                    cell_stream += buf[pos:keep]
                    buf = buf[keep:]
                    break
                cell_stream += buf[pos:i]
                end = buf.find(b'\x1b\\', i + 2)
                if end < 0:
                    buf = buf[i:]
                    break
                handle_apc(buf[i:end + 2])
                pos = end + 2
            else:
                buf = b''

            matches = []
            for rx, resp in QUERY_RESPONSES:
                for m in rx.finditer(cell_stream):
                    if m.start() not in answered:
                        matches.append((m.start(), resp))
            matches.sort()
            for start, resp in matches:
                answered.add(start)
                respond(resp)

    elapsed = time.time() - t0
    try:
        os.kill(pid, 9)
    except ProcessLookupError:
        pass
    _, status = os.waitpid(pid, 0)
    os.close(master)

    print(f"exit_reason={exit_reason} elapsed={elapsed:.1f}s status={status:#06x}")
    print(f"captured {len(frames)} frames")

    with open('/tmp/gfx_cell_stream.bin', 'wb') as f:
        f.write(cell_stream)

    if os.path.exists(ERRLOG) and os.path.getsize(ERRLOG):
        with open(ERRLOG) as f:
            print("child stderr:", f.read()[:500])

    if not frames:
        print("no frames! first 300 cell bytes:", repr(cell_stream[:300]))
        sys.exit(1)

    os.makedirs(FRAME_DIR, exist_ok=True)
    for old in os.listdir(FRAME_DIR):
        if old.startswith('frame_'):
            os.unlink(os.path.join(FRAME_DIR, old))
    index = []
    for n, (t, meta, raw, cell_len) in enumerate(frames):
        w, h = int(meta['s']), int(meta['v'])
        Image.frombytes('RGBA', (w, h), raw).save(f'{FRAME_DIR}/frame_{n:03d}.png')
        index.append(f"{n} t={t:.2f} {w}x{h} cell_len={cell_len}")
    with open(f'{FRAME_DIR}/index.txt', 'w') as f:
        f.write('\n'.join(index) + '\n')
    print(f"wrote {len(frames)} frames to {FRAME_DIR}/")

if __name__ == '__main__':
    main()

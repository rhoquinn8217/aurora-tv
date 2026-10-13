#!/usr/bin/env python3
"""Set the DualSense badge on the webOS app icons.

    python3 scripts/gen_icon_badge.py            writes deploy/webos/icon*.png
    python3 scripts/gen_icon_badge.py --check    writes nothing, exit 1 if their pixels differ

⭐ WHY THIS EXISTS. deploy/webos/icon.png and icon_large.png are GENERATED, and
for a day they had no generator: they were built by a script that lived in a
temp folder. A generated file with nothing that can regenerate it is a file
nobody can adjust, so the script and its sources live here, and --check proves
the committed icons show exactly the pixels this produces.
⚠️ The PIXELS, not the file's bytes: those are zlib's output, and a Python
built with another zlib may compress the same pixels differently, which would
read as stale icons that are not.

⛔⛔ DO NOT RUN gen_aurora_logo.py AND EXPECT THESE ICONS BACK. That script
writes icon.png at 130 px and icon_large.png at 512 px, where the files that
ship are 80 px and 130 px, and it knows nothing about the badge. Run on its
own it changes both sizes and removes the badge. If the Aurora art itself ever
changes, replace the two base icons in scripts/bridge_badge/ at their shipped
sizes and run THIS.

THE SOURCES, in scripts/bridge_badge/:
    icon_base.png        80 px   the Aurora icon with no badge on it
    icon_large_base.png  130 px  the same, large
    ds5_badge.png        512 px  the badge: a DualSense on a flat-panel TV

ⓘ The base icons are the app icons as they stood before a badge was first
added. An existing badge is baked into the pixels and cannot be lifted back
out, so the badge is always set on the clean art, never painted over an old one.

⚠️ THE FOLDER IS STILL CALLED bridge_badge AND bridge_master.png IS STILL IN
IT. Nothing reads that file any more: the badge used to be Ciprian's bridge,
recoloured black with a white glow so it would show on near-black art, and it
is now the DualSense mark instead. The master is kept because it is his
artwork and the decision is reversible; the folder keeps its name because
renaming it would churn paths for no gain. Neither is a source of these icons.

⭐ THE BADGE IS NOT DRAWN HERE, it is a committed master, the same arrangement
bridge_master.png had. ds5_badge.png comes from the DS5-USBIP listener, where
installer/trace-silhouette.py traces the controller outline and
installer/build-icon.ps1 composes it on the television; the 512 px master is
that composition rendered once and dropped in here. Regenerating it needs that
repo, which is why it is committed rather than built.

⭐ THE AURORA ART IS NOT TOUCHED. The badge is composited into a square at the
bottom right and nothing outside that square changes -- not scaled, not moved,
not recoloured. --check would catch it if it were.

Standard library only, on purpose: it runs on a machine with nothing installed.
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "scripts" / "bridge_badge"
WEBOS = ROOT / "deploy" / "webos"

# (base icon, output icon). The sizes come from the base icons themselves, so
# nothing here can disagree with what webOS is given.
TARGETS = (
    (SOURCES / "icon_base.png", WEBOS / "icon.png"),
    (SOURCES / "icon_large_base.png", WEBOS / "icon_large.png"),
)
BADGE_MASTER = SOURCES / "ds5_badge.png"

# ⭐ THE LOOK, as chosen by eye against the shipped sizes. Change these and
# rerun; nothing else in the file is a matter of taste.
#
# 0.19 is a floor rather than a preference. The controller sits at 0.78 of the
# badge's width, so at this scale it is 19 x 12 px on the 130 icon and 15 x 9
# on the 80. Go smaller and the grips and the notch between them stop
# resolving, and the badge reads as a white tile with a smudge in it.
BADGE_SCALE = 0.19        # badge width as a share of the icon's width
INSET = 0.03              # inset from the bottom-right corner, same share


# ---------------------------------------------------------------- PNG, by hand

def _chunks(data: bytes):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    i = 8
    while i < len(data):
        (ln,) = struct.unpack(">I", data[i:i + 4])
        yield data[i + 4:i + 8], data[i + 8:i + 8 + ln]
        i += 12 + ln


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _unfilter(raw: bytes, w: int, h: int, bpp: int) -> bytearray:
    stride = w * bpp
    out = bytearray()
    prev = bytearray(stride)
    pos = 0
    for _ in range(h):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if ft == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 0xFF
        elif ft == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif ft == 3:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif ft == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                c = prev[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + _paeth(a, prev[x], c)) & 0xFF
        elif ft != 0:
            raise ValueError(f"bad PNG filter {ft}")
        out += line
        prev = line
    return out


def load(path: Path):
    """8-bit grey, RGB or RGBA. Returns (w, h, bytes per pixel, pixels)."""
    idat = b""
    w = h = bd = ct = None
    for typ, body in _chunks(path.read_bytes()):
        if typ == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
    if bd != 8 or ct not in (0, 2, 6):
        raise ValueError(f"{path.name}: need 8-bit grey, RGB or RGBA")
    bpp = {0: 1, 2: 3, 6: 4}[ct]
    return w, h, bpp, _unfilter(zlib.decompress(idat), w, h, bpp)


def encode_rgba(w: int, h: int, pix: bytearray) -> bytes:
    """Filter type 0 on every line. Larger than an optimal choice and perfectly
    valid, and with the same zlib the output is byte-for-byte reproducible."""
    stride = w * 4
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pix[y * stride:(y + 1) * stride]
    out = b"\x89PNG\r\n\x1a\n"
    for typ, body in (
        (b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)),
        (b"IDAT", zlib.compress(bytes(raw), 9)),
        (b"IEND", b""),
    ):
        out += struct.pack(">I", len(body)) + typ + body
        out += struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF)
    return out


def to_rgba(w: int, h: int, bpp: int, pix: bytearray) -> bytearray:
    if bpp == 4:
        return pix
    out = bytearray(w * h * 4)
    for i in range(w * h):
        if bpp == 3:
            out[i * 4:i * 4 + 3] = pix[i * 3:i * 3 + 3]
        else:
            out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = pix[i]
        out[i * 4 + 3] = 255
    return out


# ------------------------------------------------------------------- the image

def rgba_down(pix: bytearray, w: int, h: int, nw: int, nh: int) -> bytearray:
    """Area average. ⚠️ Colour is weighted by alpha: a plain average drags the
    colour of fully transparent pixels into every edge."""
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        y0, y1 = y * h // nh, max(y * h // nh + 1, (y + 1) * h // nh)
        for x in range(nw):
            x0, x1 = x * w // nw, max(x * w // nw + 1, (x + 1) * w // nw)
            r = g = b = a = n = 0
            for yy in range(y0, y1):
                base = yy * w
                for xx in range(x0, x1):
                    i = (base + xx) * 4
                    al = pix[i + 3]
                    r += pix[i] * al
                    g += pix[i + 1] * al
                    b += pix[i + 2] * al
                    a += al
                    n += 1
            o = (y * nw + x) * 4
            if a > 0:
                out[o] = min(255, r // a)
                out[o + 1] = min(255, g // a)
                out[o + 2] = min(255, b // a)
            out[o + 3] = a // n if n else 0
    return out


def badge_at(size: int) -> bytearray:
    """The badge master, area-averaged down to `size` square."""
    w, h, bpp, pix = load(BADGE_MASTER)
    if w != h:
        raise ValueError(f"{BADGE_MASTER.name}: expected a square master")
    return rgba_down(to_rgba(w, h, bpp, pix), w, h, size, size)


def badged_icon(base_path: Path) -> tuple[int, int, bytearray]:
    """The base icon with the badge set near its bottom-right corner, as RGBA
    pixels. Everything outside the badge's square is the base, untouched."""
    w, h, bpp, pix = load(base_path)
    pix = to_rgba(w, h, bpp, pix)
    size = int(w * BADGE_SCALE)
    inset = int(round(w * INSET))
    badge = badge_at(size)
    x0 = w - size - inset
    y0 = h - size - inset
    for y in range(size):
        for x in range(size):
            bi = (y * size + x) * 4
            a = badge[bi + 3] / 255.0
            if a <= 0:
                continue
            di = ((y0 + y) * w + (x0 + x)) * 4
            for c in range(3):
                pix[di + c] = int(badge[bi + c] * a + pix[di + c] * (1 - a) + 0.5)
            pix[di + 3] = max(pix[di + 3], badge[bi + 3])
    return w, h, pix


def shows_the_same(target: Path, w: int, h: int, pix: bytearray) -> bool:
    """Does the committed icon show exactly these pixels? Decoded and compared,
    so how the file happens to be compressed does not matter."""
    if not target.is_file():
        return False
    try:
        tw, th, tbpp, tpix = load(target)
    except (ValueError, zlib.error, struct.error):
        return False
    return (tw, th) == (w, h) and to_rgba(tw, th, tbpp, tpix) == pix


def main() -> int:
    check = "--check" in sys.argv[1:]
    missing = [p for p in (BADGE_MASTER, *(b for b, _ in TARGETS)) if not p.is_file()]
    if missing:
        for p in missing:
            print(f"missing source: {p}", file=sys.stderr)
        return 2

    stale = 0
    for base, target in TARGETS:
        w, h, pix = badged_icon(base)
        size = int(w * BADGE_SCALE)
        if check:
            same = shows_the_same(target, w, h, pix)
            print(f"{'ok   ' if same else 'STALE'} {target.relative_to(ROOT)}  "
                  f"{w}x{h}  badge {size}px")
            stale += 0 if same else 1
        else:
            data = encode_rgba(w, h, pix)
            target.write_bytes(data)
            print(f"Wrote {target.relative_to(ROOT)}  {w}x{h}  badge {size}px  "
                  f"{len(data)} bytes")
    return 1 if stale else 0


if __name__ == "__main__":
    raise SystemExit(main())

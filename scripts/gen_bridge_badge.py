#!/usr/bin/env python3
"""Set the bridge badge on the webOS app icons: a black bridge with a white glow.

    python3 scripts/gen_bridge_badge.py            writes deploy/webos/icon*.png
    python3 scripts/gen_bridge_badge.py --check    writes nothing, exit 1 if they differ

⭐ WHY THIS EXISTS. deploy/webos/icon.png and icon_large.png are GENERATED, and
for a day they had no generator: they were built by a script that lived in a
temp folder. A generated file with nothing that can regenerate it is a file
nobody can adjust, so the script and its three sources live here now, and
--check proves the committed icons are exactly what this produces.

⛔⛔ DO NOT RUN gen_aurora_logo.py AND EXPECT THESE ICONS BACK. That script
writes icon.png at 130 px and icon_large.png at 512 px, where the files that
ship are 80 px and 130 px, and it knows nothing about the badge. Run on its
own it changes both sizes and removes the bridge. If the Aurora art itself
ever changes, replace the two base icons in scripts/bridge_badge/ at their
shipped sizes and run THIS.

THE SOURCES, in scripts/bridge_badge/:
    icon_base.png        80 px   the Aurora icon with no badge on it
    icon_large_base.png  130 px  the same, large
    bridge_master.png    512 px  the bridge, as luminance only

ⓘ The base icons are the app icons as they stood before a badge was first
added. An existing badge is baked into the pixels and cannot be lifted back
out, so the badge is always set on the clean art, never painted over an old one.

WHY IT IS BUILT THIS WAY. The icon is almost black, so a black bridge alone
would be invisible on it. The glow is not decoration: it is the only thing that
makes the silhouette readable. Two orderings were tried first and both failed,
and the order below is what is left:

  1. THICKEN AT HIGH RESOLUTION. Thickening closes any gap narrower than twice
     its radius. At badge size the gaps between the cables are a pixel or two,
     so thickening there welds the cables into a solid blob. At 512 px the same
     gaps are tens of pixels wide and survive.
  2. GLOW AT LOW RESOLUTION. Build the black bridge and the white glow large,
     shrink the result, and every black line is averaged with the white around
     it: the bridge comes out grey. So the mask is shrunk FIRST, to twice the
     badge size, and the glow is drawn around a bridge that is still solid.
  3. One last 2:1 shrink, which only antialiases.

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
BRIDGE_MASTER = SOURCES / "bridge_master.png"

# ⭐ THE LOOK, as chosen by eye on the television's launcher size. Change these
# and rerun; nothing else in the file is a matter of taste.
BADGE_SCALE = 0.45        # badge width as a share of the icon's width
THICKEN = 6               # pixels of thickening, at the 512 px master
GLOW_RADIUS = 0.028       # glow blur radius as a share of the working size
GLOW_LIFT = 2.4           # three box passes spread the glow thin; lift it back
MASK_LO, MASK_HI = 0.42, 0.62   # where luminance stops being glow and is bridge
INSET = 0                 # pixels in from the bottom-right corner


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
    valid, and it keeps the output byte-for-byte reproducible."""
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


def grey_down(m: bytearray, w: int, h: int, nw: int, nh: int) -> bytearray:
    """Area average of a mask. Its midtones ARE antialiasing, so a plain
    average is exactly right here, unlike colour."""
    out = bytearray(nw * nh)
    for y in range(nh):
        y0, y1 = y * h // nh, max(y * h // nh + 1, (y + 1) * h // nh)
        for x in range(nw):
            x0, x1 = x * w // nw, max(x * w // nw + 1, (x + 1) * w // nw)
            acc = n = 0
            for yy in range(y0, y1):
                base = yy * w
                for xx in range(x0, x1):
                    acc += m[base + xx]
                    n += 1
            out[y * nw + x] = acc // n if n else 0
    return out


def dilate(mask: bytearray, w: int, h: int, radius: int) -> bytearray:
    """Grow the bridge outward. A MAX filter, not a blur: a blur would make
    the edges softer and the middle lighter, which is the opposite of thicker."""
    if radius <= 0:
        return mask
    tmp = bytearray(len(mask))
    for y in range(h):
        row = y * w
        for x in range(w):
            m = 0
            for i in range(max(0, x - radius), min(w, x + radius + 1)):
                v = mask[row + i]
                if v > m:
                    m = v
                    if m == 255:
                        break
            tmp[row + x] = m
    out = bytearray(len(mask))
    for x in range(w):
        for y in range(h):
            m = 0
            for i in range(max(0, y - radius), min(h, y + radius + 1)):
                v = tmp[i * w + x]
                if v > m:
                    m = v
                    if m == 255:
                        break
            out[y * w + x] = m
    return out


def _blur_axis(src: bytearray, w: int, h: int, radius: int, horizontal: bool) -> bytearray:
    """Running-sum box blur along one axis."""
    dst = bytearray(len(src))
    span = 2 * radius + 1
    if horizontal:
        for y in range(h):
            row = y * w
            acc = 0
            for i in range(-radius, radius + 1):
                acc += src[row + min(max(i, 0), w - 1)]
            for x in range(w):
                dst[row + x] = acc // span
                acc -= src[row + min(max(x - radius, 0), w - 1)]
                acc += src[row + min(max(x + radius + 1, 0), w - 1)]
    else:
        for x in range(w):
            acc = 0
            for i in range(-radius, radius + 1):
                acc += src[min(max(i, 0), h - 1) * w + x]
            for y in range(h):
                dst[y * w + x] = acc // span
                acc -= src[min(max(y - radius, 0), h - 1) * w + x]
                acc += src[min(max(y + radius + 1, 0), h - 1) * w + x]
    return dst


def blur(mask: bytearray, w: int, h: int, radius: int, passes: int = 3) -> bytearray:
    """Three box passes are close enough to a Gaussian for a glow. One pass
    leaves visibly square edges."""
    for _ in range(passes):
        mask = _blur_axis(mask, w, h, radius, True)
        mask = _blur_axis(mask, w, h, radius, False)
    return mask


def smoothstep(lo: float, hi: float, x: float) -> float:
    if hi <= lo:
        return 0.0 if x < lo else 1.0
    t = (x - lo) / (hi - lo)
    if t <= 0:
        return 0.0
    if t >= 1:
        return 1.0
    return t * t * (3.0 - 2.0 * t)


def make_badge(size: int) -> bytearray:
    """The badge alone, RGBA, `size` pixels square."""
    w, h, bpp, lums = load(BRIDGE_MASTER)
    if bpp != 1:
        raise ValueError("bridge_master.png must be 8-bit grey")

    # A low percentile, not the minimum: one stray dark pixel would set the
    # floor and shift every threshold with it.
    ordered = sorted(lums)
    floor = ordered[int(len(ordered) * 0.02)]
    span = max(1, 255 - floor)

    mask = bytearray(w * h)
    for i in range(w * h):
        mask[i] = int(255 * smoothstep(MASK_LO, MASK_HI, (lums[i] - floor) / span) + 0.5)

    mask = dilate(mask, w, h, THICKEN)              # 1. thicken while gaps are wide

    work = size * 2
    mask = grey_down(mask, w, h, work, work)        # 2. shrink the MASK, then glow
    glow = blur(mask, work, work, max(1, int(work * GLOW_RADIUS)))

    out = bytearray(work * work * 4)
    for i in range(work * work):
        m = mask[i] / 255.0
        gl = min(1.0, (glow[i] / 255.0) * GLOW_LIFT)
        a = m + gl * (1.0 - m)
        if a <= 0.0:
            continue
        # a black bridge over a white glow, resolved to straight colour
        c = (255.0 * gl * (1.0 - m)) / a
        o = i * 4
        out[o] = out[o + 1] = out[o + 2] = int(max(0.0, min(255.0, c)) + 0.5)
        out[o + 3] = int(max(0.0, min(255.0, a * 255.0)) + 0.5)

    return rgba_down(out, work, work, size, size)   # 3. antialias only


def badge_icon(base_path: Path) -> tuple[int, int, bytes]:
    """The base icon with the badge set flush in its bottom-right corner."""
    w, h, bpp, pix = load(base_path)
    pix = to_rgba(w, h, bpp, pix)
    size = int(w * BADGE_SCALE)
    badge = make_badge(size)
    x0 = w - size - INSET
    y0 = h - size - INSET
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
    return w, h, encode_rgba(w, h, pix)


def main() -> int:
    check = "--check" in sys.argv[1:]
    missing = [p for p in (BRIDGE_MASTER, *(b for b, _ in TARGETS)) if not p.is_file()]
    if missing:
        for p in missing:
            print(f"missing source: {p}", file=sys.stderr)
        return 2

    stale = 0
    for base, target in TARGETS:
        w, h, data = badge_icon(base)
        if check:
            same = target.is_file() and target.read_bytes() == data
            print(f"{'ok   ' if same else 'STALE'} {target.relative_to(ROOT)}  {w}x{h}")
            stale += 0 if same else 1
        else:
            target.write_bytes(data)
            print(f"Wrote {target.relative_to(ROOT)}  {w}x{h}  {len(data)} bytes")
    return 1 if stale else 0


if __name__ == "__main__":
    raise SystemExit(main())

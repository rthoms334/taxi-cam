# Usage: python tools/icon/make_icon.py src/app/taxi-cam.ico [preview.png]
#
# Draws the Taxi Cam icon at each shell size. Every size is fitted to whole
# pixels by itself rather than scaled from one drawing: a dark tile in a yellow
# border, and inside it, clear of the border, a hold-short marking.
#
# - Up to 32 px: a stop bar (a dashed line over a solid one) and the taxiway
#   centre line running up the tile to meet it. No lettering.
# - 40 and 48 px: the stop bar over TAXI.
# - 64 px and up: the full marking (two dashed lines over two solid ones) over TAXI.
#
# Straight edges fall on pixel boundaries; only the corners and the diagonals
# of A and X are antialiased (8x supersampling, area coverage).
import sys
from PIL import Image, ImageDraw

TILE = (18, 20, 23, 255)
YELLOW = (242, 194, 48, 255)
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
K = 8  # Supersampling.

# Per size: border, padding inside it, line thickness, gap between lines,
# rows from the padding to the marking, corner radius.
FIT = {
    16: dict(border=1, pad=1, t=2, gap=1, top=1, radius=4),
    20: dict(border=1, pad=2, t=2, gap=2, top=1, radius=4),
    24: dict(border=2, pad=2, t=2, gap=2, top=1, radius=5),
    32: dict(border=2, pad=3, t=3, gap=3, top=1, radius=7),
    40: dict(border=2, pad=4, t=3, gap=3, top=1, radius=9),
    48: dict(border=3, pad=4, t=4, gap=3, top=1, radius=11),
    64: dict(border=4, pad=5, t=3, gap=3, top=1, radius=14),
    128: dict(border=7, pad=11, t=6, gap=6, top=2, radius=28),
    256: dict(border=14, pad=22, t=12, gap=12, top=4, radius=56),
}


def dashes(width, count):
    """Whole dashes and gaps across `width`: (dash, gap, offset). They fill it
    exactly where whole pixels allow, and are centred where they cannot."""
    best = None
    for gap in range(1, width):
        for dash in range(gap, width):
            total = count * dash + (count - 1) * gap
            if total > width:
                break
            left = width - total
            score = (left % 2, left, abs(dash - 2 * gap))
            if best is None or score < best[0]:
                best = (score, dash, gap, left // 2)
    return best[1], best[2], best[3]


def draw(size):
    f = FIT[size]
    n = size * K
    mask = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(mask)

    def rect(x0, y0, x1, y1):  # Whole pixels, right and bottom exclusive.
        if x1 > x0 and y1 > y0:
            d.rectangle((x0 * K, y0 * K, x1 * K - 1, y1 * K - 1), fill=255)

    def poly(points):
        d.polygon([(x * K, y * K) for x, y in points], fill=255)

    b, p = f["border"], f["pad"]
    x0, x1 = b + p, size - b - p  # The content box.
    width = x1 - x0
    t, gap = f["t"], f["gap"]
    glyph = size <= 32
    full = size >= 64

    # The marking: dashed lines over solid ones.
    y = x0 + f["top"]
    count = 3 if size <= 40 else 5 if size >= 64 else 4
    dash, space, offset = dashes(width, count)
    for dashed in ([True, True, False, False] if full else [True, False]):
        if dashed:
            for i in range(count):
                rect(x0 + offset + i * (dash + space), y, x0 + offset + i * (dash + space) + dash, y + t)
        else:
            rect(x0, y, x1, y + t)
        y += t + gap
    marking_bottom = y - gap

    if glyph:
        # The taxiway's centre line, running up the tile and meeting the stop bar.
        line = 2 if size <= 24 else 4
        rect(size // 2 - line // 2, marking_bottom, size // 2 + line // 2, size - b)
    else:
        # TAXI, as tall as fits under the marking and across the content.
        room = x1 - (marking_bottom + gap)
        h = room - max(1, round(size * 0.02))
        while True:
            stroke = max(2, round(h * 0.2))
            letter = round(h * 0.62)
            if (letter - stroke) % 2:
                letter += 1  # A centred stem on whole pixels.
            between = max(1, round(h * 0.15))
            total = 3 * letter + stroke + 3 * between
            if total <= width:
                break
            h -= 1
        top = x1 - h
        x = x0 + (width - total) // 2
        s = stroke
        # T
        rect(x, top, x + letter, top + s)
        rect(x + (letter - s) // 2, top, x + (letter + s) // 2, top + h)
        x += letter + between
        # A: two legs from a flat apex, and a bar.
        mid = x + letter / 2
        lean = s * 1.08  # A leg's width along the baseline.
        poly([(mid - s * 0.5, top), (mid + s * 0.5, top), (x + letter, top + h), (x + letter - lean, top + h),
              (mid, top + s * 1.15), (x + lean, top + h), (x, top + h)])
        bar = top + h - round(h * 0.36)
        inset = (h - (bar - top)) / h * (letter / 2 - s * 0.5)
        poly([(x + inset, bar), (x + letter - inset, bar), (x + letter - inset, bar + max(1, s - 1)), (x + inset, bar + max(1, s - 1))])
        x += letter + between
        # X
        poly([(x, top), (x + lean, top), (x + letter, top + h), (x + letter - lean, top + h)])
        poly([(x + letter, top), (x + letter - lean, top), (x, top + h), (x + lean, top + h)])
        x += letter + between
        # I
        rect(x, top, x + s, top + h)

    # The border: the tile's outline, the same width all round.
    radius = f["radius"]
    outer = Image.new("L", (n, n), 0)
    ImageDraw.Draw(outer).rounded_rectangle((0, 0, n - 1, n - 1), radius * K, fill=255)
    inner = Image.new("L", (n, n), 0)
    ImageDraw.Draw(inner).rounded_rectangle((b * K, b * K, n - 1 - b * K, n - 1 - b * K), max(0, radius - b) * K, fill=255)

    small = lambda m: m.resize((size, size), Image.BOX)
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    image.paste(Image.new("RGBA", (size, size), YELLOW), (0, 0), small(outer))
    image.paste(Image.new("RGBA", (size, size), TILE), (0, 0), small(inner))
    image.paste(Image.new("RGBA", (size, size), YELLOW), (0, 0), small(mask))
    return image


if __name__ == "__main__":
    out = sys.argv[1]
    images = [draw(s) for s in SIZES]
    images[-1].save(out, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1], bitmap_format="bmp")
    if len(sys.argv) > 2:
        # Every size as drawn, on light and dark, then enlarged without smoothing.
        sheet = Image.new("RGB", (1280, 620), (238, 238, 238))
        ImageDraw.Draw(sheet).rectangle((0, 310, 1280, 620), fill=(32, 32, 32))
        for row in (0, 310):
            x = 24
            for s in SIZES:
                im = draw(s)
                sheet.paste(im, (x, row + 155 - s // 2), im)
                x += s + 24
            x += 16
            for s in (16, 24, 32):
                im = draw(s).resize((s * 4, s * 4), Image.NEAREST)
                sheet.paste(im, (x, row + 155 - s * 2), im)
                x += s * 4 + 24
        sheet.save(sys.argv[2])

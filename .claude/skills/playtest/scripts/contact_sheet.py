#!/usr/bin/env python3
"""Tile playtest screenshots into one captioned image (for a report).

  contact_sheet.py OUT.png SHOT1.png "caption 1" SHOT2.png "caption 2" ... [--cols N] [--width W]

Each shot is scaled to the column width (default 640 px, 2 columns) with
its caption in a bar underneath. Needs Pillow.
"""

import sys

from PIL import Image, ImageDraw, ImageFont


def main():
    args = sys.argv[1:]
    cols, width = 2, 640
    for flag in ("--cols", "--width"):
        if flag in args:
            i = args.index(flag)
            val = int(args[i + 1])
            del args[i:i + 2]
            if flag == "--cols":
                cols = val
            else:
                width = val
    if len(args) < 3 or len(args) % 2 != 1:
        sys.exit(__doc__)
    out, pairs = args[0], list(zip(args[1::2], args[2::2]))
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 16)
    except OSError:
        font = ImageFont.load_default()
    cap = 30
    tiles = []
    for path, caption in pairs:
        im = Image.open(path).convert("RGB")
        h = round(im.height * width / im.width)
        im = im.resize((width, h), Image.LANCZOS)
        t = Image.new("RGB", (width, h + cap), (18, 20, 26))
        t.paste(im, (0, 0))
        ImageDraw.Draw(t).text((8, h + 6), caption, fill=(235, 235, 235), font=font)
        tiles.append(t)
    rows = (len(tiles) + cols - 1) // cols
    th = max(t.height for t in tiles)
    sheet = Image.new("RGB", (cols * width + (cols - 1) * 6, rows * th + (rows - 1) * 6), (8, 9, 12))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * (width + 6), (i // cols) * (th + 6)))
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()

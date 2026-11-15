#!/usr/bin/env python3
"""Screenshot the Xvfb root window (optionally cropped): shot.py out.png [x y w h]"""
import sys
from Xlib import display, X
from PIL import Image
d = display.Display(); r = d.screen().root; g = r.get_geometry()
raw = r.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff)
im = Image.frombytes('RGB', (g.width, g.height), raw.data, 'raw', 'BGRX')
if len(sys.argv) >= 6:
    x, y, w, h = map(int, sys.argv[2:6]); im = im.crop((x, y, x + w, y + h))
im.save(sys.argv[1])

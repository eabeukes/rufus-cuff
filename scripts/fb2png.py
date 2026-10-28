"""Convert a gzipped dump of the watch's /dev/fb0 to a PNG in the wearer's view.
    python3 fb2png.py fb.gz out.png
Panel is portrait 480x640 XRGB8888 (stride 1920); worn landscape with the buttons on the right,
so canvas (x, y) lives at framebuffer row x, column 479-y: a 90-degree counter-clockwise turn.
"""
import gzip
import sys

from PIL import Image

raw = gzip.open(sys.argv[1]).read()
im = Image.frombuffer('RGB', (480, 640), raw, 'raw', 'BGRX', 1920, 1)
im.transpose(Image.Transpose.ROTATE_90).save(sys.argv[2])

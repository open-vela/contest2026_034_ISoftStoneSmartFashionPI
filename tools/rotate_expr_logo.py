#!/usr/bin/env python3
"""Rotate expr_logo.gif 90 degrees clockwise, keeping frame timing/loops."""
import os
from PIL import Image

SRC = '/home/kecen/vela_agent0706/contest2026_034_ISoftStoneSmartFashionPI/app/watch/resource/power_png/expr_logo.gif'
DST = SRC  # overwrite after rotating
BAK = '/tmp/expr_logo_orig.gif'

if not os.path.exists(BAK):
    with open(SRC, 'rb') as f:
        with open(BAK, 'wb') as g:
            g.write(f.read())
    print('backup ->', BAK)

im = Image.open(SRC)
frames = []
durations = []
for i in range(im.n_frames):
    im.seek(i)
    # Composite onto solid black (logo GIF has black bg; keeps rotation clean)
    bg = Image.new('RGBA', im.size, (0, 0, 0, 255))
    bg.alpha_composite(im.convert('RGBA'))
    # PIL rotate is counter-clockwise positive: +90 == 90 deg counter-clockwise
    rot = bg.rotate(90, expand=True).convert('RGB')
    frames.append(rot.quantize(colors=256, method=Image.MEDIANCUT))
    durations.append(im.info.get('duration', 30))

frames[0].save(DST, save_all=True, append_images=frames[1:],
               duration=durations, loop=0, disposal=2)
print('rotated:', Image.open(DST).size, 'frames:', Image.open(DST).n_frames)

# Preview of first/middle frame for direction check
im2 = Image.open(DST)
im2.seek(0)
im2.convert('RGBA').save('/tmp/pv_expr_rot0.png')
im2.seek(45)
im2.convert('RGBA').save('/tmp/pv_expr_rot45.png')
print('previews saved')

"""Classifies dumped bottom-screen readbacks the way src/msr/bottom_classifier.h does.

  python tools/msr/dev/bottom_classify.py "<dir>/seq-*.ppm" [every]

Prints each frame where the state changes (every N frames too, if given) with the measures the
classifier uses: the HUD rules at rows 40 and 215, the title rule (rows 18-31), the longest cyan
rule, the dark fraction, the peak and the boxes of bright (>140) and any (>4) content. Dumps come
from the test copy with MSR_DEV=1: user/msr/bottom.trigger (one frame) or bottomseq.trigger
(holding a frame count). Keep this in step with the C++ when thresholds change.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

W, H = 320, 240


def load(path):
    return np.asarray(Image.open(path).convert('RGB')).astype(int)


def find_rule(im, y, x0, x1):
    """Coverage and level of a HUD rule at row y: blue standing out from 7 rows above/below."""
    hits = 0
    level = 0.0
    for x in range(x0, x1):
        line = max(im[min(max(y + dy, 0), H - 1), x, 2] for dy in range(-2, 3))
        above = im[min(max(y - 7, 0), H - 1), x, 2]
        below = im[min(max(y + 7, 0), H - 1), x, 2]
        hits += (line - min(above, below)) > 35
        level += line
    n = max(1, x1 - x0)
    return hits / n, level / n


def cyan(im):
    r, g, b = im[..., 0], im[..., 1], im[..., 2]
    return (b >= 150) & (b > r + 30) & (g > r)


def longest_rule(im, y0=0, y1=H):
    """(length, y, x0, x1) of the longest horizontal cyan run in rows y0..y1."""
    c = cyan(im)
    best = (0, 0, 0, 0)
    for y in range(y0, y1):
        run = 0
        for x in range(W):
            run = run + 1 if c[y, x] else 0
            if run > best[0]:
                best = (run, y, x + 1 - run, x + 1)
    return best


def longest_column(im, x0, x1, y0=0, y1=H):
    c = cyan(im)
    best = 0
    for x in range(x0, x1):
        run = 0
        for y in range(y0, y1):
            run = run + 1 if c[y, x] else 0
            best = max(best, run)
    return best


def dark_fraction(im, threshold):
    return (im[::2, ::2].max(axis=2) < threshold).mean()


def box(m, threshold):
    ys, xs = np.nonzero(m > threshold)
    return (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1) if len(xs) else None


def title_menu_page(im):
    if longest_column(im, 30, 42, 50, 190) >= 100 and longest_column(im, 278, 290, 50, 190) >= 100:
        return 'Gallery'
    top = longest_rule(im, 28, 39)
    if top[0] >= 240 and top[2] <= 55:
        bottom = longest_rule(im, 80, 90)
        return 'FileList' if bottom[0] >= 240 and bottom[2] <= 45 else 'FileOpen'
    return 'Other'


def classify(im):
    top = find_rule(im, 40, 85, 220)
    bottom = find_rule(im, 215, 65, 230)
    title = longest_rule(im, 18, 31)
    if top[0] > .8 and bottom[0] > .8 and title[0] < 100:
        return 'Hud' if top[1] > 135 and bottom[1] > 135 else 'Logo'
    if longest_rule(im)[0] >= 100:
        if dark_fraction(im, 6) > .3:
            return 'Menu/' + title_menu_page(im)
        page = 'Other'
        if title[0] >= 60:
            if title[3] >= 310 and title[2] < 90:
                page = 'PauseSamus'
            elif title[2] < 80 and title[3] < 280 and bottom[0] > .8:
                page = 'PauseMap'
            elif title[2] >= 90 and title[3] <= 235 and bottom[0] > .8:
                page = 'MapPins'
        return 'Menu/' + page
    if (im[::2, ::2].max(axis=2) > 140).mean() > .001:
        return 'Prompt'
    return 'Logo'


def measures(im):
    top = find_rule(im, 40, 85, 220)
    bottom = find_rule(im, 215, 65, 230)
    m = im.max(axis=2)
    return ('top %.2f/%3d bot %.2f/%3d title %3d long %3d dark6 %.2f peak %3d bright %s any %s' % (
        top[0], top[1], bottom[0], bottom[1], longest_rule(im, 18, 31)[0], longest_rule(im)[0],
        dark_fraction(im, 6), m.max(), box(m, 140), box(m, 4)))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    every = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    last = None
    for i, path in enumerate(sorted(glob.glob(sys.argv[1]))):
        im = load(path)
        state = classify(im)
        if state != last or (every and i % every == 0):
            print(os.path.basename(path), state.ljust(14), measures(im))
        last = state


if __name__ == '__main__':
    main()

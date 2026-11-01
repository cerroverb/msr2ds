"""flicker_scan.py <dir> [out_dir] [min_pixels]: finds flicker in a topseq.trigger capture (the
top-*-f<frame>-t<ms>.ppm files in dir): frames that differ from both neighbours while the
neighbours agree. Prints each suspect with the area that changed and, with out_dir, saves the
worst ones as before / frame / after strips. A suspect's draws are under the previous frame's
header in the draw log."""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    min_pixels = int(sys.argv[3]) if len(sys.argv) > 3 else 1500

    def frame_number(path):
        return int(re.search(r'top-\d+-f(\d+)', os.path.basename(path)).group(1))

    paths = sorted(glob.glob(os.path.join(folder, 'top-*.ppm')), key=frame_number)
    if len(paths) < 3:
        sys.exit('need at least 3 frames')
    numbers = [frame_number(p) for p in paths]
    frames = [np.asarray(Image.open(p).convert('RGB')).astype(np.int16) for p in paths]

    suspects = []
    for i in range(1, len(frames) - 1):
        before, frame, after = frames[i - 1], frames[i], frames[i + 1]
        changed = ((np.abs(frame - before).max(axis=2) > 40) &
                   (np.abs(frame - after).max(axis=2) > 40) &
                   (np.abs(after - before).max(axis=2) < 12))
        count = int(changed.sum())
        if count >= min_pixels:
            ys, xs = np.nonzero(changed)
            suspects.append((count, i))
            print(f'frame {numbers[i]}: {count} pixels, x {xs.min()}-{xs.max()}, '
                  f'y {ys.min()}-{ys.max()}')
    print(f'{len(frames)} frames, {len(suspects)} suspects')

    if out:
        os.makedirs(out, exist_ok=True)
        for count, i in sorted(suspects, reverse=True)[:12]:
            strip = np.concatenate(frames[i - 1:i + 2], axis=1).astype(np.uint8)
            Image.fromarray(strip).save(os.path.join(out, f'flicker-{numbers[i]}.png'))


if __name__ == '__main__':
    main()

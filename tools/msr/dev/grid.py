"""grid.py <out.png> <columns> <width> <image>...: a contact sheet of the images, each scaled to
width, for comparing captures (drive.ps1 Shot) side by side."""
import sys

from PIL import Image


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    out, columns, width = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    images = [Image.open(p) for p in sys.argv[4:]]
    images = [im.resize((width, int(im.height * width / im.width))) for im in images]
    height = max(im.height for im in images)
    rows = (len(images) + columns - 1) // columns
    sheet = Image.new('RGB', (columns * width, rows * height))
    for i, im in enumerate(images):
        sheet.paste(im, ((i % columns) * width, (i // columns) * height))
    sheet.save(out)


if __name__ == '__main__':
    main()

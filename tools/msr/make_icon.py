"""Generates dist/msr/samus.ico, the executable icon (requires Pillow).

An original, simple emblem: an orange helmet with a green visor on a navy disc ringed in the
cyan of the game's interface. Run from the repository root: python tools/msr/make_icon.py
"""
import os

from PIL import Image, ImageDraw

SIZE = 1024  # drawn large, then downsampled for clean edges


def draw():
    image = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(image)
    c = SIZE // 2
    # Navy disc with a cyan ring
    d.ellipse([24, 24, SIZE - 24, SIZE - 24], fill=(10, 26, 40, 255))
    d.ellipse([24, 24, SIZE - 24, SIZE - 24], outline=(63, 169, 219, 255), width=44)
    # Helmet: orange dome with a darker lower jaw
    d.ellipse([c - 300, c - 330, c + 300, c + 290], fill=(232, 115, 28, 255))
    d.rectangle([c - 300, c - 20, c + 300, c + 300], fill=(232, 115, 28, 255))
    d.polygon([(c - 300, c + 160), (c - 170, c + 330), (c + 170, c + 330), (c + 300, c + 160)],
              fill=(176, 72, 18, 255))
    # Red crown stripe
    d.rounded_rectangle([c - 70, c - 330, c + 70, c - 120], radius=60, fill=(200, 40, 34, 255))
    # Green visor
    d.polygon([(c - 250, c - 70), (c + 250, c - 70), (c + 210, c + 130), (c + 60, c + 190),
               (c - 60, c + 190), (c - 210, c + 130)], fill=(82, 209, 58, 255))
    # Visor highlight
    d.polygon([(c - 200, c - 40), (c - 40, c - 40), (c - 120, c + 70), (c - 175, c + 70)],
              fill=(178, 245, 150, 200))
    return image


def main():
    out = os.path.join('dist', 'msr', 'samus.ico')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    image = draw()
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    image.resize((256, 256), Image.LANCZOS).save(out, sizes=sizes)
    image.resize((512, 512), Image.LANCZOS).save(os.path.join('dist', 'msr', 'samus.png'))
    print('wrote', out)


if __name__ == '__main__':
    main()

"""Draw the Binary Stars scene from its two emission fields and a seeded star catalog."""
from math import atan2, cos, exp, hypot, sin
from pathlib import Path
from random import Random

from PIL import Image, ImageChops, ImageDraw, ImageFilter


SIZE = (1280, 720)
OUTPUT = Path(__file__).resolve().parents[1] / "examples/binary-stars/sky.png"


def emission(x, y, cx, cy, eclipse):
    dx, dy = x - cx, y - cy
    r = hypot(dx, dy)
    angle = atan2(dy, dx)
    if r > 260:
        return 0
    if eclipse:
        disk = hypot(dx / 1.55, dy * 1.55)
        ring = exp(-abs(r - 28) / 3) + .44 * exp(-abs(r - 37) / 4)
        filaments = (.58 + .42 * sin(3 * angle - disk * .14)) * exp(-abs(disk - 50) / 15)
        ray = .9 * exp(-abs(dy + .06 * dx) / 2.6) * exp(-abs(dx) / 87)
        return (ring + .46 * filaments + ray) * min(1, max(0, (r - 17) / 8))
    arm = 3 * angle + r * .072 + .5 * sin(5 * angle - .05 * r)
    field = exp(-r / 55) * (.15 + .82 * (.5 + .5 * cos(arm)) ** 5)
    return field + .4 * exp(-abs(r - 37 - 3 * sin(angle * 4)) / 3) + .9 * exp(-r / 13)


def main():
    width, height = SIZE
    raster = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            blue = emission(x, y, 275, 299, False)
            gold = emission(x, y, 920, 320, True)
            dust = exp(-abs(y - 270 - 74 * sin(x * .0043)) / 70)
            noise = sin(.026 * x + .031 * y + 1.3 * sin(.034 * y - .016 * x))
            haze = dust * (9 + 5 * noise)
            for i, (base, a, b, k) in enumerate(((5, 107, 255, .64), (10, 227, 146, .79), (19, 250, 60, 1))):
                raster[(y * width + x) * 3 + i] = min(255, round(base + haze * k + blue * a + gold * b))
    sky = Image.frombytes("RGB", SIZE, bytes(raster))
    stars = Image.new("RGB", SIZE)
    pen, rng = ImageDraw.Draw(stars), Random(1851)
    for _ in range(1450):
        x, y = rng.randrange(width), rng.randrange(height)
        brightness = int(25 + 185 * rng.random() ** 3)
        pen.point((x, y), (brightness, brightness, min(255, brightness + 12)))
        if brightness > 193:
            pen.line((x - 3, y, x + 3, y), fill=(75, 87, 103))
            pen.line((x, y - 3, x, y + 3), fill=(75, 87, 103))
            pen.point((x, y), (216, 221, 229))
    sky = ImageChops.add(sky, ImageChops.add(stars, stars.filter(ImageFilter.GaussianBlur(1.6))))
    sky.convert("RGBA").save(OUTPUT, optimize=True)


if __name__ == "__main__":
    main()

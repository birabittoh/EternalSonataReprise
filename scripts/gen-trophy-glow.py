#!/usr/bin/env python3
"""Builds src/images/icon_trophies_glow.png from src/images/icon_trophies.png.

The option strip's hover glow is per option art: a 128x128 chunk holding a
blurred silhouette of that option's own 64x64 icon, drawn 32px up and left of
it. Replacing the icon therefore leaves the glow behind unless this is rerun,
and the result is checked in (the game reads the PNG through gen-images.py).

Colours are taken from the shipped glows: a near white core, a salmon rim, and
a paler salmon in the fade. Everything else is a knob; run with --preview to
write a side by side against a decoded original before committing.
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
IMAGES_DIR = os.path.join(ROOT, "src", "images")

CORE_RGB = (247, 239, 239)
RIM_RGB = (255, 160, 148)
FADE_RGB = (255, 203, 189)


def blur(a, radius):
    image = Image.fromarray((a * 255).clip(0, 255).astype(np.uint8))
    return np.asarray(image.filter(ImageFilter.GaussianBlur(radius))).astype(float) / 255.0


def erode(a, pixels):
    if pixels <= 0:
        return a
    image = Image.fromarray((a * 255).clip(0, 255).astype(np.uint8))
    return np.asarray(image.filter(ImageFilter.MinFilter(2 * pixels + 1))).astype(float) / 255.0


def build(icon, size, core_radius, core_gain, core_inset, halo_radius, halo_gain,
          halo_peak, opacity):
    icon = icon.convert("RGBA")
    inset = size // 4  # the icon is half the glow's width, centred
    if icon.size != (size // 2, size // 2):
        icon = icon.resize((size // 2, size // 2), Image.LANCZOS)
    canvas = Image.new("L", (size, size), 0)
    canvas.paste(icon.split()[3], (inset, inset))

    solid = (np.asarray(canvas).astype(float) / 255.0 > 0.5).astype(float)
    # Pulled inside the outline: the icon's own edge is antialiased, so a core
    # that reaches the outline shows as a white rim around it.
    core = np.clip(blur(erode(solid, core_inset), core_radius) * core_gain, 0.0, 1.0)
    # The ceiling only bites where the halo is strongest, which is the band
    # hugging the icon, so it cools that edge and leaves the falloff alone.
    halo = np.clip(blur(solid, halo_radius) * halo_gain, 0.0, halo_peak)

    alpha = np.maximum(core, halo) * opacity
    rim = np.asarray(RIM_RGB) + (np.asarray(FADE_RGB) - np.asarray(RIM_RGB)) * (1.0 - halo)[..., None]
    rgb = np.asarray(CORE_RGB) * core[..., None] + rim * (1.0 - core)[..., None]
    return Image.fromarray(
        np.dstack([rgb, alpha * 255.0]).clip(0, 255).astype(np.uint8), "RGBA")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--icon", default=os.path.join(IMAGES_DIR, "icon_trophies.png"))
    p.add_argument("--out", default=os.path.join(IMAGES_DIR, "icon_trophies_glow.png"))
    p.add_argument("--size", type=int, default=128)
    p.add_argument("--core-radius", type=float, default=1.5,
                   help="how soft the bright centre's edge is")
    p.add_argument("--core-gain", type=float, default=1.0,
                   help="above 1 spreads the centre past the icon's outline, where the "
                        "icon stops hiding it and it reads as white spill")
    p.add_argument("--core-inset", type=int, default=3,
                   help="pixels the white core is held back from the outline")
    p.add_argument("--halo-radius", type=float, default=13.0, help="how far the glow reaches")
    p.add_argument("--halo-gain", type=float, default=2.6, help="how solid the glow is")
    p.add_argument("--halo-peak", type=float, default=0.65,
                   help="ceiling on the glow, which is how hot the band against the "
                        "icon's border gets")
    p.add_argument("--opacity", type=float, default=0.90, help="overall brightness")
    p.add_argument("--preview", metavar="PATH",
                   help="also write the glow as drawn, icon on top, at 3x")
    args = p.parse_args()

    icon = Image.open(args.icon)
    glow = build(icon, args.size, args.core_radius, args.core_gain, args.core_inset,
                 args.halo_radius, args.halo_gain, args.halo_peak, args.opacity)
    glow.save(args.out)
    print(f"+ wrote {args.out}")

    if args.preview:
        # The icon covers the middle of the glow, so only what escapes its
        # outline is ever visible: compose them the way the strip does.
        inset = args.size // 4
        shot = Image.new("RGBA", (args.size, args.size), (60, 50, 45, 255))
        shot.alpha_composite(glow, (0, 0))
        shot.alpha_composite(icon.convert("RGBA"), (inset, inset))
        side = args.size * 3
        sheet = Image.new("RGBA", (side * 2, side), (255, 255, 255, 255))
        sheet.paste(shot.resize((side, side), Image.NEAREST), (0, 0))
        sheet.paste(glow.resize((side, side), Image.NEAREST), (side, 0))
        sheet.save(args.preview)
        print(f"+ wrote {args.preview}")


if __name__ == "__main__":
    main()

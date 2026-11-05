#!/usr/bin/env python3
"""Build a user mod for native touch: a finger on the cards instead of a mouse.

    tools/native_touch_ui_mod.py --base-game ~/Heroes\\ V/game --out build/touch/NativeTouchUI.h5u
    tools/native_touch_ui_mod.py --game hof --base-game ~/Heroes\\ V/game --out build/touch/NativeTouchUI-hof.pak
    tools/native_touch_ui_mod.py --game toe --base-game ~/Heroes\\ V/game-toe --out build/touch/NativeTouchUI-toe.h5u

A card of the map's objects, a hero's, a creature's (NUI::CClickingTooltip-
Creator: a short card, then the full one) carries a small mouse with its
right button lit in a corner (UI/Tooltips/CommonAdvObjTooltip/MoreIcon, the
game's TOOLTIP_MORE_WND_ID; the battle tower's card shows the same picture
as its NextIcon): "hold the right button for more". With native touch a
finger's rest shows the card and a tap on it the full one (native/
touch_native.c), so the picture is a hand whose raised finger is lit the same
blue. The picture is the port's own, drawn here (15x20, as the mouse); the
icon's background (MoreIcon.(BackgroundSimpleTexture)) names a texture of
that size under a new name (TouchMoreIcon), made from the mouse's
description with its picture, size and record changed.

The three games carry the same files. The recipe (tools/ui_recipe.py) carries
the picture in base64; its out path starts with touch/: the app makes the mod
only while native touch is on (kit/host/ui_mods.cpp) and takes it out when it
is off."""

import argparse
import re
import struct
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ui_recipe  # noqa: E402
from touch_ui_mod import ENTRY_DATE, paks_for, read, replace_once, set_record  # noqa: E402

BACK = "UI/Tooltips/CommonAdvObjTooltip/MoreIcon.(BackgroundSimpleTexture).xdb"
TEX_DIR = "Textures/Interface/Tooltips/Hero/"
TEXTURE = TEX_DIR + "MoreIcon.xdb"
NEW_TEXTURE = TEX_DIR + "TouchMoreIcon.xdb"
NEW_PICTURE = TEX_DIR + "TouchMoreIcon.dds"
RECIPE_OUT = {"base": "touch/UserMODs/NativeTouchUI.h5u", "hof": "touch/data/zz-native-touch-ui.pak",
              "toe": "touch/UserMODs/NativeTouchUI.h5u"}
RECORD_ID = 7731005  # away from the game's own and touch_ui_mod.py's
SIZE = (15, 20)      # the mouse's: the icon's window (19x24) is left as it is


def draw_finger():
    """The hand, RGBA rows top down, drawn 16 times larger and scaled down:
    a fist with its index finger raised, beige with the dark brown outline of
    the game's mouse, the finger's tip the blue of its right button."""
    from PIL import Image, ImageDraw, ImageFilter

    s, (w, h) = 16, SIZE

    def u(v):
        return round(v * s)

    def shape(draw):
        draw.rounded_rectangle([u(4.8), u(0.4), u(9.8), u(12)], radius=u(2.1), fill=255)   # the finger
        draw.rounded_rectangle([u(4.2), u(7.6), u(13.6), u(18.8)], radius=u(2.6), fill=255)  # the fist
        for cx in (10.6, 12.4):                                                              # its knuckles
            draw.ellipse([u(cx - 1.6), u(7.0), u(cx + 1.4), u(10.6)], fill=255)
        draw.rounded_rectangle([u(1.4), u(9.4), u(7.0), u(14.2)], radius=u(2.3), fill=255)   # the thumb

    size = (u(w), u(h))
    mask = Image.new("L", size, 0)
    shape(ImageDraw.Draw(mask))
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    img.paste(Image.new("RGBA", size, (0x4c, 0x32, 0x1e, 255)), (0, 0), mask.filter(ImageFilter.MaxFilter(2 * s - 1)))
    inner = mask.filter(ImageFilter.MinFilter(s // 2 * 2 + 1))
    body = Image.new("RGBA", size)
    bd = ImageDraw.Draw(body)
    for x in range(size[0]):  # lit in the middle, darker to the sides, as the mouse
        t = (abs(x / size[0] - 0.52) * 2) ** 2
        bd.line([(x, 0), (x, size[1])], fill=(round(0xcf - 0x45 * t), round(0xb9 - 0x43 * t), round(0xa0 - 0x44 * t), 255))
    img.paste(body, (0, 0), inner)
    tip = Image.new("L", size, 0)
    ImageDraw.Draw(tip).rounded_rectangle([u(4.8), u(0.4), u(9.8), u(5.6)], radius=u(2.1), fill=255)
    tip = Image.composite(tip, Image.new("L", size, 0), inner)
    blue = Image.new("RGBA", size)
    bl = ImageDraw.Draw(blue)
    for y in range(size[1]):
        t = min(1.0, y / u(5.4))
        bl.line([(0, y), (size[0], y)], fill=(round(0x2a + 0x10 * t), round(0xa4 - 0x40 * t), 0xff, 255))
    img.paste(blue, (0, 0), tip)
    d = ImageDraw.Draw(img)
    lw = s // 2
    d.line([(u(5.0), u(5.6)), (u(9.6), u(5.6))], fill=(0x35, 0x28, 0x1e, 255), width=lw)  # the tip's edge
    for x in (9.6, 11.5):                                                                  # the folded fingers
        d.line([(u(x), u(8.9)), (u(x), u(12.2))], fill=(0x5c, 0x46, 0x33, 255), width=lw)
    d.arc([u(1.6), u(9.6), u(7.0), u(14.0)], 300, 60, fill=(0x5c, 0x46, 0x33, 255), width=lw)
    # Scaled with the colours weighted by their cover, so the outline's edge
    # fades to the dark of the outline rather than to the colours beside it;
    # the faintest edge pixels go (the game draws them as they are).
    small = img.convert("RGBa").resize((w, h), Image.LANCZOS).convert("RGBA")
    small.putdata([p if p[3] >= 12 else (0, 0, 0, 0) for p in small.get_flattened_data()])
    return small


def dds(img):
    """An uncompressed 32-bit DDS without mips (BGRA in memory), as the game's
    own MoreIcon.dds."""
    w, h = img.size
    head = bytearray(128)
    head[0:4] = b"DDS "
    struct.pack_into("<7I", head, 4, 124, 0x1007, h, w, 0, 0, 1)            # size, flags, height, width, pitch, depth, mips
    struct.pack_into("<8I", head, 76, 32, 0x41, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000)
    struct.pack_into("<I", head, 108, 0x1000)                               # caps: a texture
    rgba = img.tobytes()
    px = bytearray(rgba)
    px[0::4], px[2::4] = rgba[2::4], rgba[0::4]
    return bytes(head) + bytes(px)


def build(files, picture):
    """name -> bytes of the mod's files."""
    tex = files[TEXTURE]
    tex = replace_once(tex, b'<DestName href="MoreIcon.dds"/>', b'<DestName href="TouchMoreIcon.dds"/>')
    tex = re.sub(rb'<SrcName href="[^"]*"/>', b'<SrcName href="/UI/Misc/TouchMoreIcon.tga"/>', tex, count=1)
    tex = re.sub(rb"<Width>\d+</Width>", b"<Width>%d</Width>" % SIZE[0], tex, count=1)
    tex = re.sub(rb"<Height>\d+</Height>", b"<Height>%d</Height>" % SIZE[1], tex, count=1)
    back = replace_once(files[BACK], b'href="/Textures/Interface/Tooltips/Hero/MoreIcon.xdb#',
                        b'href="/Textures/Interface/Tooltips/Hero/TouchMoreIcon.xdb#')
    return {NEW_TEXTURE: set_record(tex, RECORD_ID), NEW_PICTURE: picture, BACK: back}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--base-game", type=Path, required=True,
                   help="the game's install folder (the base game's for hof, Tribes of the East's for toe)")
    p.add_argument("--game", choices=("base", "hof", "toe"), default="base")
    p.add_argument("--out", type=Path, required=True, help="the .h5u (or for hof the .pak) to write")
    p.add_argument("--recipe", type=Path, help="also write the recipe (tools/ui_recipe.py) the app makes it from")
    p.add_argument("--png", type=Path, help="also write the picture as a PNG, for a look")
    args = p.parse_args()

    paks = [args.base_game / n for n in paks_for(args.game)]
    files = read(paks, [BACK, TEXTURE])
    missing = [n for n in (BACK, TEXTURE) if n not in files]
    if missing:
        sys.exit("Not in the game's data: " + ", ".join(missing))
    finger = draw_finger()
    if args.png:
        finger.save(args.png)
    out = build(files, dds(finger))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(out):
            info = zipfile.ZipInfo(name, date_time=ENTRY_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, out[name])
    if args.recipe:
        ui_recipe.write(args.recipe, name="NativeTouchUI" + ("" if args.game == "base" else "-" + args.game),
                        out=RECIPE_OUT[args.game], date="%04d-%02d-%02d %02d:%02d:%02d" % ENTRY_DATE,
                        layers=paks_for(args.game), files=out,
                        sources={NEW_TEXTURE: (TEXTURE, files[TEXTURE]), BACK: (BACK, files[BACK])},
                        drawn=(NEW_PICTURE,),
                        comment="Made by tools/native_touch_ui_mod.py --game %s; applied by kit/host/ui_mods.cpp."
                                % args.game)
    print("%s: %d files" % (args.out, len(out)))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Turn the Windows executable's icon into the app's icon files.

    tools/extract_icon.py --exe original/gog/D3DPopTB.exe --dest App.app
    tools/extract_icon.py --image icon.png --dest App.app
    tools/extract_icon.py --image icon.png --icns App.app/Contents/Resources/AppIcon.icns
    tools/extract_icon.py --image icon.png --ico build/app.ico

Reads the first RT_GROUP_ICON resource and its RT_ICON frames, picks the
largest frame, and writes the PNG sizes iPadOS reads from CFBundleIconFiles
(Icon-<points>@2x.png). The game's own art is the icon, scaled up; no icon
is invented. A game repository may ship its own square artwork (icon.png at
its root) instead; that image then also becomes the macOS AppIcon.icns and
the Windows executable's icon (an .ico the app's resources take in)."""

import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

import pefile
from PIL import Image
import io

# (base name, pixels): the @2x files iPadOS looks up for the names in the plist.
IOS_ICONS = (("Icon-29", 58), ("Icon-40", 80), ("Icon-60", 120), ("Icon-76", 152), ("Icon-83.5", 167))
# (iconset name, pixels): what iconutil packs into a macOS .icns.
MAC_ICONS = tuple(("icon_%dx%d%s" % (pt, pt, suffix), pt * scale)
                  for pt in (16, 32, 128, 256, 512) for suffix, scale in (("", 1), ("@2x", 2)))
# The frames of a Windows .ico: the shell picks one per view and DPI.
WINDOWS_ICONS = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
# macOS app icon grid (1024 canvas): an 824 px rounded square, 100 px margin.
MAC_CANVAS, MAC_TILE = 1024, 824


class MissingIconError(ValueError):
    """The PE is valid but has no icon resource."""


def ico_bytes(exe_path):
    """The first icon group of a PE, rebuilt as a .ico file's bytes."""
    pe = pefile.PE(str(exe_path), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    icons, group = {}, None
    resource = getattr(pe, "DIRECTORY_ENTRY_RESOURCE", None)
    for entry in resource.entries if resource else []:
        kind = pefile.RESOURCE_TYPE.get(entry.id)
        if kind not in ("RT_ICON", "RT_GROUP_ICON"):
            continue
        for e in entry.directory.entries:
            lang = e.directory.entries[0].data.struct
            data = pe.get_data(lang.OffsetToData, lang.Size)
            if kind == "RT_ICON":
                icons[e.id] = data
            elif group is None:
                group = data
    if group is None or not icons:
        raise MissingIconError("%s has no icon group" % exe_path)
    count = struct.unpack_from("<H", group, 4)[0]
    header = struct.pack("<HHH", 0, 1, count)
    entries, blobs = b"", b""
    offset = 6 + 16 * count
    for i in range(count):
        w, h, colors, reserved, planes, bpp, size, ordinal = struct.unpack_from("<BBBBHHIH", group, 6 + 14 * i)
        blob = icons[ordinal]
        entries += struct.pack("<BBBBHHII", w, h, colors, reserved, planes, bpp, len(blob), offset)
        blobs += blob
        offset += len(blob)
    return header + entries + blobs


def largest_frame(ico):
    """The biggest image in the .ico, as RGBA."""
    image = Image.open(io.BytesIO(ico))
    best = max(image.info.get("sizes", {image.size}), key=lambda s: s[0] * s[1])
    image.size = best
    image.load()
    return image.convert("RGBA")


def square_artwork(image_path):
    """A repository-supplied icon image, centre-cropped to a square, as RGBA."""
    with Image.open(image_path) as image:
        art = image.convert("RGBA")
    side = min(art.size)
    left, top = (art.width - side) // 2, (art.height - side) // 2
    return art.crop((left, top, left + side, top + side))


def write_icons(exe_path, dest, sizes=IOS_ICONS, image=None):
    """Write every icon file into `dest`; returns the paths written."""
    if image is not None:
        # Painted artwork, not pixel art: one smooth downscale per size.
        art = square_artwork(image)
        dest = Path(dest)
        dest.mkdir(parents=True, exist_ok=True)
        written = []
        for name, px in sizes:
            path = dest / ("%s@2x.png" % name)
            art.resize((px, px), Image.LANCZOS).convert("RGB").save(path, "PNG")
            written.append(path)
        return written
    try:
        icon = ico_bytes(exe_path)
    except MissingIconError:
        # Some installers keep the executable's icon beside it instead of
        # embedding a resource. Windows filenames are case insensitive.
        exe_path = Path(exe_path)
        name = exe_path.with_suffix(".ico").name.casefold()
        matches = [p for p in exe_path.parent.iterdir() if p.name.casefold() == name and p.is_file()]
        if len(matches) != 1:
            raise
        icon = matches[0].read_bytes()
    frame = largest_frame(icon)
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    written = []
    for name, px in sizes:
        # Pixel art from 1998: scale by whole multiples first so the pixels
        # stay square, then fit the exact size.
        factor = max(1, px // frame.width)
        scaled = frame.resize((frame.width * factor, frame.height * factor), Image.NEAREST)
        scaled = scaled.resize((px, px), Image.LANCZOS)
        # iOS icons have no alpha: composite onto black, as the Windows shell would on a dark tile.
        flat = Image.new("RGB", (px, px), (0, 0, 0))
        flat.paste(scaled, mask=scaled.split()[3])
        path = dest / ("%s@2x.png" % name)
        flat.save(path, "PNG")
        written.append(path)
    return written


def mac_icon(image_path):
    """The artwork on the macOS icon grid: a rounded tile with a soft shadow."""
    from PIL import ImageDraw, ImageFilter
    art = square_artwork(image_path).resize((MAC_TILE, MAC_TILE), Image.LANCZOS)
    # Draw the mask 4x larger and shrink it, for smooth corners.
    big = MAC_TILE * 4
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, big - 1, big - 1), radius=round(big * 0.225), fill=255)
    mask = mask.resize((MAC_TILE, MAC_TILE), Image.LANCZOS)
    margin = (MAC_CANVAS - MAC_TILE) // 2
    canvas = Image.new("RGBA", (MAC_CANVAS, MAC_CANVAS), (0, 0, 0, 0))
    shadow = Image.new("L", (MAC_CANVAS, MAC_CANVAS), 0)
    shadow.paste(mask.point(lambda a: a * 0.5), (margin, margin + 12))
    canvas.putalpha(shadow.filter(ImageFilter.GaussianBlur(14)))
    tile = Image.new("RGBA", (MAC_TILE, MAC_TILE), (0, 0, 0, 0))
    tile.paste(art, mask=mask)
    canvas.alpha_composite(tile, (margin, margin))
    return canvas


def write_icns(image_path, icns_path):
    """Pack the artwork into a macOS .icns with iconutil."""
    icon = mac_icon(image_path)
    icns_path = Path(icns_path)
    icns_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / "AppIcon.iconset"
        iconset.mkdir()
        for name, px in MAC_ICONS:
            icon.resize((px, px), Image.LANCZOS).save(iconset / ("%s.png" % name), "PNG")
        subprocess.run([shutil.which("iconutil") or "iconutil", "-c", "icns", "-o", str(icns_path), str(iconset)],
                       check=True)
    return icns_path


def write_ico(image_path, ico_path):
    """The artwork as a Windows .ico, every size in WINDOWS_ICONS, alpha kept."""
    art = square_artwork(image_path)
    ico_path = Path(ico_path)
    ico_path.parent.mkdir(parents=True, exist_ok=True)
    art.resize((256, 256), Image.LANCZOS).save(ico_path, "ICO", sizes=[(px, px) for px in WINDOWS_ICONS])
    return ico_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, help="Windows executable whose icon group is used")
    parser.add_argument("--image", type=Path, help="Square artwork used instead of the executable's icon")
    parser.add_argument("--dest", type=Path, help="Directory for the iPadOS Icon-*@2x.png files")
    parser.add_argument("--icns", type=Path, help="Write a macOS .icns here (needs --image)")
    parser.add_argument("--ico", type=Path, help="Write a Windows .ico here (needs --image)")
    args = parser.parse_args()
    if args.image is not None and not args.image.is_file():
        # The game repository has no artwork of its own: keep the executable's icon.
        args.image = None
    if args.dest is not None:
        if args.image is None and args.exe is None:
            parser.error("--dest needs --exe or an existing --image")
        paths = write_icons(args.exe, args.dest, image=args.image)
        print("icon: %d files from %s into %s" % (len(paths), args.image or args.exe, args.dest))
    if args.icns is not None:
        if args.image is None:
            print("icon: no artwork for %s; the app keeps the generic icon" % args.icns)
        else:
            print("icon: %s from %s" % (write_icns(args.image, args.icns), args.image))
    if args.ico is not None:
        if args.image is None:
            parser.error("--ico needs an existing --image")
        print("icon: %s from %s" % (write_ico(args.image, args.ico), args.image))


if __name__ == "__main__":
    sys.exit(main())

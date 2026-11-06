#!/usr/bin/env python3
"""Build a user mod that lays the base game's interface out for wide screens.

    tools/widescreen_mod.py --base-game ~/Heroes\\ V/game \\
        --toe-data ~/Heroes\\ V/game-toe/data --out build/widescreen/Widescreen.h5u

Tribes of the East's patch 3.1 (a2p1-data.pak) re-anchored the interface for
wide screens: root screens got <HorAlign>/<VerAlign> (ALIGN_SCREEN,
ALIGN_CENTER) and their windows' <Placement> blocks moved to the right or
bottom edge (EPA_HIGH_END) or the centre (ERA_CENTER). The base game's exe
reads the same tags (design notes ultrawide-plan.md), but its data has none of them.

For every UI file a2p1 changes, this takes the base game's own copy (data.pak
with p0..p3c-data.pak over it) and carries over only how Tribes of the East
re-anchored it, from its data.pak to a2p1: each <Placement> block's anchors,
with the position recomputed so the base window keeps its own offset from the
edge or centre it is now anchored to, and the root screens' added
<HorAlign>/<VerAlign> lines. Textures, texts and windows the base game does not
have stay out. A block is left as it is when the base copy was anchored
differently from Tribes of the East's starting point.

The result is a zip the game loads from UserMODs/*.h5u over its data. It is
derived from the player's own game files and is never committed. --report
writes what was applied and what was left out, file by file. --recipe writes
what it changed without the files (tools/ui_recipe.py): the app makes the
same mod from the player's own install with it, so the mod never ships. The
few files the mod adds whole (the black curtains) and the types.xml lines
come from tools/ui_mod_parts.py, checked here against Tribes of the East's
copies."""

import argparse
import difflib
import json
import xml.etree.ElementTree as ET
from pathlib import Path
import re
import sys
import zipfile

import ui_mod_parts
import ui_recipe

# The base game's data in load order (a later pak's copy wins): data/ and then
# DataLS/p5.., which 006c4a60 mounts after it and which carry their own
# UI files and types.xml.
BASE_PAKS = ["data/data.pak", "data/p0-data.pak", "data/p1-data.pak", "data/p2-data.pak",
             "data/p3c-data.pak", "DataLS/p5-data.pak", "DataLS/p6-data.pak",
             "DataLS/p7-data.pak", "DataLS/p8-data.pak"]
# Hammers of Fate (bina1/H5_Game.exe) reads data/ and then dataa1/; it has no
# DataLS and no root-screen alignment, so its pak carries the child anchors,
# the black borders and the ALIGN_SCREEN roots stretched (stretch_root), but
# not the types.xml fields.
HOF_PAKS = ["data/data.pak", "data/p0-data.pak", "data/p1-data.pak", "data/p2-data.pak",
            "data/p3c-data.pak", "dataa1/a1-data.pak", "dataa1/a1p1-data.pak"]
# Hammers of Fate mounts these with a higher priority than data\*.pak, where
# the kit's overlay lists the mod, so a file they carry cannot be replaced
# under its own name (see unshadow).
HOF_TOP_PAKS = ["dataa1/a1-data.pak", "dataa1/a1p1-data.pak"]
# Tribes of the East (--base-game its install): its own wide-screen layout
# already; the tool only adds the wide dialogue scenes.
TOE_PAKS = ["data/data.pak", "data/a2p1-data.pak"]
BORDER_ITEMS = [b'\t\t<Item href="Common/BlackBorder/%s.(WindowSimple).xdb#xpointer(/WindowSimple)"/>' % side
                for side in (b"Left", b"Right", b"Top", b"Bottom")]
SCREEN_ALIGN = re.compile(rb"^\s*<(HorAlign|VerAlign)>[A-Z_]+</\1>\s*$")
# The black curtains a2p1 hangs beside letterboxed screens (dialogues,
# cutscenes, videos, the puzzle map): new windows of a plain colour.
BORDER_DIR = ui_mod_parts.BORDER_DIR
BORDER_ITEM = re.compile(rb'^\s*<Item href="[^"]*Common/BlackBorder/[A-Za-z]+\.\(WindowSimple\)\.xdb#xpointer\(/WindowSimple\)"/>\s*$')


def read_layers(paks):
    """name -> bytes of UI/*.xdb and types.xml, later paks over earlier ones."""
    files = {}
    for pak in paks:
        if not pak.is_file():
            continue
        with zipfile.ZipFile(pak) as z:
            for info in z.infolist():
                name = info.filename
                if (name.startswith("UI/") and name.endswith(".xdb")) or name == "types.xml":
                    files[name] = z.read(info)
    return files


def item_block(text, start):
    """The <Item>...</Item> block (its own lines) that encloses offset `start`."""
    begin = text.rfind(b"\n", 0, text.rfind(b"<Item>", 0, start)) + 1
    depth, pos = 0, begin
    for m in re.finditer(rb"<Item>|</Item>", text[begin:]):
        depth += 1 if m.group(0) == b"<Item>" else -1
        if depth == 0:
            pos = begin + m.end()
            break
    end = text.index(b"\n", pos) + 1
    return begin, end


def check_parts(toe):
    """Exit unless ui_mod_parts describes what Tribes of the East's patch has:
    the same curtains and the same types.xml declarations (but for the
    fields' wording)."""
    for name, data in ui_mod_parts.black_borders().items():
        if name in toe and toe[name] != data:
            sys.exit("%s: ui_mod_parts.py differs from Tribes of the East's" % name)
    t = toe["types.xml"].replace(b"\r\n", b"\n")
    eb, ee = item_block(t, t.index(b"<__ServerPtr>" + ui_mod_parts.ALIGN_TYPE + b"</__ServerPtr>"))
    hb, he = item_block(t, t.index(b"<Name>HorAlign</Name>"))
    vb, ve = item_block(t, t.index(b"<Name>VerAlign</Name>"))
    words = re.compile(rb"<Description>[^<]*</Description>")
    if (t[eb:ee] != ui_mod_parts.types_enum()
            or words.sub(b"", t[hb:he] + t[vb:ve]) != words.sub(b"", ui_mod_parts.types_fields())):
        sys.exit("types.xml: ui_mod_parts.py differs from Tribes of the East's declarations")


def patch_types(base):
    """The base game's types.xml with WindowScreen's HorAlign/VerAlign fields
    (chunks 5 and 6) and their Align enum, as Tribes of the East declares them.
    Without them the loader skips the tags: the 1.60 exe reads both chunks
    (00e575c0), only its data never named them."""
    nl = b"\r\n" if b"\r\n" in base else b"\n"
    field_type = ui_mod_parts.ALIGN_TYPE
    enum_item = ui_mod_parts.types_enum()
    fields = ui_mod_parts.types_fields()

    text = base
    cls = text.index(b"<TypeName>WindowScreen</TypeName>")
    nested = text.index(b"<NestedTypes/>", cls)
    indent = text[text.rfind(b"\n", 0, nested) + 1:nested]
    text = (text[:nested] + b"<NestedTypes>" + nl + indent + b"\t<Item>" + field_type + b"</Item>" + nl
            + indent + b"</NestedTypes>" + text[nested + len(b"<NestedTypes/>"):])
    tip = text.index(b"<Name>TooltipContext</Name>", cls)
    _, after = item_block(text, tip)
    text = text[:after] + fields.replace(b"\n", nl) + text[after:]
    shared_end = text.rindex(b"\t</SharedClasses>")
    text = text[:shared_end] + enum_item.replace(b"\n", nl) + text[shared_end:]
    return text


AXES = (("x", "HorAllign"), ("y", "VerAllign"))
PAIRS = ("Position", "Size", "LowerMargin", "UpperMargin")
PLACEMENT = re.compile(rb"<Placement>.*?</Placement>", re.S)


def read_placement(block):
    """A <Placement> block as {field: value}: (x, y) pairs and alignments,
    each with its <Second> flag (set in this file, else inherited)."""
    e = ET.fromstring(block)
    v = {}
    for name in PAIRS:
        f = e.find(name)
        v[name] = (int(f.findtext("First/x")), int(f.findtext("First/y")), f.findtext("Second"))
    for _, name in AXES:
        f = e.find(name)
        v[name] = (f.findtext("First"), f.findtext("Second"))
    return e, v


def merge_placement(b, o, n, sizes=None):
    """The base block re-anchored the way Tribes of the East re-anchored its
    own copy, keeping the base window's own offset and size; None when the
    base block does not start from the same anchoring. sizes: the base and
    old windows' sizes when the block inherits its size (Size/Second false)
    from the window's shared description, or None."""
    try:
        eb, vb = read_placement(b)
        _, vo = read_placement(o)
        _, vn = read_placement(n)
    except (AttributeError, ET.ParseError, TypeError, ValueError):
        return None  # a block without the usual fields
    if vo == vn:
        return b
    for name in PAIRS:
        if vb[name][2] != vo[name][2]:
            return None
    out = {name: list(vb[name][:2]) for name in PAIRS}
    flags = {name: vb[name][2] for name in PAIRS}
    align = {name: vb[name] for _, name in AXES}
    for i, (_, aname) in enumerate(AXES):
        changed = vo[aname] != vn[aname] or any(vo[p][i] != vn[p][i] for p in PAIRS)
        if not changed:
            continue
        if vb[aname] != vo[aname]:
            return None
        d_pos = vb["Position"][i] - vo["Position"][i]
        if sizes and vb["Size"][2] == "false" and vo["Size"][2] == "false":
            d_size = sizes[0][i] - sizes[1][i]
        else:
            d_size = vb["Size"][i] - vo["Size"][i]
        anchor = vn[aname][0]
        # Keep the base window's distance from what the new anchor holds:
        # the right or bottom edge, the centre, or the left or top edge.
        shift = {"EPA_HIGH_END": d_pos + d_size, "ERA_CENTER": d_pos + d_size // 2}.get(anchor, d_pos)
        out["Position"][i] = vn["Position"][i] + shift
        out["Size"][i] = vn["Size"][i] + d_size
        for m in ("LowerMargin", "UpperMargin"):
            out[m][i] = vn[m][i] + vb[m][i] - vo[m][i]
        align[aname] = vn[aname]
    for name in PAIRS:
        if vo[name][2] != vn[name][2]:
            flags[name] = vn[name][2]
    for name in PAIRS:
        f = eb.find(name)
        f.find("First/x").text, f.find("First/y").text = str(out[name][0]), str(out[name][1])
        f.find("Second").text = flags[name]
    for _, name in AXES:
        f = eb.find(name)
        f.find("First").text, f.find("Second").text = align[name]
    data = ET.tostring(eb, encoding="utf-8")
    if data.startswith(b"<?xml"):
        data = data.split(b"?>", 1)[1].lstrip(b"\r\n")
    return data


def screen_tags(old, new, base):
    """Insert the <HorAlign>/<VerAlign> lines a2p1 added to a root screen, and
    its black-border children, before the line that follows them there."""
    o, n, b = (x.splitlines(keepends=True) for x in (old, new, base))
    out = list(b)
    added = 0
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, o, n, autojunk=False).get_opcodes():
        if tag != "insert" or not all(SCREEN_ALIGN.match(n[j]) or BORDER_ITEM.match(n[j])
                                      for j in range(j1, j2)):
            continue
        anchor = o[i1] if i1 < len(o) else None
        if anchor is None or anchor not in out:
            continue
        at = out.index(anchor)
        out[at:at] = n[j1:j2]
        added += j2 - j1
    return b"".join(out), added


SHARED_HREF = re.compile(rb'<Shared href="([^"#]+)#')


def inherited_size(layer, name, data):
    """The size a window's description inherits: its shared description's
    first <Placement> Size, or None."""
    m = SHARED_HREF.search(data)
    if not m:
        return None
    href = m.group(1).decode()
    shared = href.lstrip("/") if href.startswith("/") else str(Path(name).parent / href)
    block = PLACEMENT.search(layer.get(shared, b""))
    if not block:
        return None
    try:
        return read_placement(block.group(0))[1]["Size"][:2]
    except (AttributeError, ET.ParseError, TypeError, ValueError):
        return None


def merge_file(base, old, new, sizes=None):
    """(merged bytes or None, blocks re-anchored, blocks left out, screen tags
    added). sizes: the inherited sizes of the file's own window in the base
    and old layers (inherited_size), for its first block."""
    pb, po, pn = (PLACEMENT.findall(x) for x in (base, old, new))
    if not (len(pb) == len(po) == len(pn)):
        return None, 0, len(pn), 0
    pieces, last, moved, left = [], 0, 0, 0
    for k, (m, o, n) in enumerate(zip(PLACEMENT.finditer(base), po, pn)):
        merged = merge_placement(m.group(0), o, n, sizes if k == 0 else None)
        if merged is None:
            left += 1
            merged = m.group(0)
        elif merged != m.group(0):
            moved += 1
        pieces += [base[last:m.start()], merged]
        last = m.end()
    pieces.append(base[last:])
    merged, tags = screen_tags(old, new, b"".join(pieces))
    return (merged if merged != base else None), moved, left, tags


def add_borders(data, items=None):
    """A root screen's shared description with the black borders (or the given
    border items) as its last children, for a screen a2p1 does not border
    itself (Hammers of Fate's loading screen, a 1024x768 picture centred on a
    wide screen)."""
    items = items or BORDER_ITEMS
    if b"Common/BlackBorder/" in data or b"Common/WideBorder/" in data:
        return data
    nl = b"\r\n" if b"\r\n" in data else b"\n"
    end = data.index(b"</Children>")
    start = data.rfind(nl, 0, end) + len(nl)
    return data[:start] + nl.join(items) + nl + data[start:]


def backdrop_borders(screen, background, black):
    """{name: bytes} of four borders like a2p1's black ones but painted with the
    given background (an absolute href such as
    /UI/pwlscreen/Back.(BackgroundSimpleScallingTexture).xdb#xpointer(/...)),
    so the sides of a centred screen show its backdrop the way Tribes of the
    East's full-width loading screen does; and the items that hang them."""
    stem = screen.rsplit("/", 1)[-1].split(".", 1)[0]
    folder = "UI/Common/WideBorder/%s/" % stem
    files, items = {}, []
    for name, data in sorted(black.items()):
        if not name.startswith(BORDER_DIR) or "(BackgroundSimpleTexture)" in name:
            continue
        leaf = name[len(BORDER_DIR):]
        if leaf.endswith("(WindowSimpleShared).xdb"):
            data = re.sub(rb'<Background href="[^"]*"/>',
                          b'<Background href="%s"/>' % background.encode(), data)
        files[folder + leaf] = data
        if leaf.endswith("(WindowSimple).xdb"):
            items.append(b'\t\t<Item href="/%s#xpointer(/WindowSimple)"/>' % (folder + leaf).encode())
    return files, items


def unshadow(written, base, top):
    """Rename each written screen description that a higher-priority pak also
    carries (names in top) to X-Wide.(WindowScreenShared).xdb, and point its
    screen, which only data/ carries, at the new name. The names of the
    shadowed files it cannot redirect."""
    stuck = []
    for name in [n for n in written if n in top]:
        screen = name.replace("(WindowScreenShared)", "(WindowScreen)")
        if (not name.endswith(".(WindowScreenShared).xdb") or screen in top
                or screen not in base):
            stuck.append(name)
            continue
        leaf = name.rsplit("/", 1)[-1]
        wide = leaf.replace(".(WindowScreenShared)", "-Wide.(WindowScreenShared)")
        src = written.get(screen, base[screen])
        old = b'href="%s#' % leaf.encode()
        if old not in src:
            sys.exit("%s does not name %s" % (screen, leaf))
        written[screen] = src.replace(old, b'href="%s#' % wide.encode())
        written[name[:-len(leaf)] + wide] = written.pop(name)
    return stuck


BACKDROP = "Backdrop-Wide"


def shared_name(files, screen):
    """The name of the shared description `screen` (a root's file) points at."""
    m = re.search(rb'<Shared href="([^"#]+)#', files.get(screen, b""))
    if not m:
        return None
    href = m.group(1).decode()
    return href[1:] if href.startswith("/") else screen.rsplit("/", 1)[0] + "/" + href


def move_backdrops(written, base, toe_new):
    """{name: bytes} of the backdrop windows added: for each root screen whose
    background Tribes of the East moved off the root onto a centred child
    (its root's <Background/> empty, a Back window among its children) while
    the base copy still paints it on the root, the root's background goes to
    a centred window of the port's own (ui_mod_parts.backdrop_window), so it
    stays under the panels a2p1 re-anchored to the centre. Edits the root's
    shared description in `written`."""
    added = {}
    for screen in sorted(toe_new):
        if not screen.endswith(".(WindowScreen).xdb") or screen not in base:
            continue
        toe_shared, own_shared = shared_name(toe_new, screen), shared_name(base, screen)
        if not toe_shared or not own_shared or toe_shared not in toe_new or own_shared not in base:
            continue
        t = toe_new[toe_shared]
        if not re.search(rb"<Background/>", t) or not re.search(rb'<Item href="[^"]*/Back\.\(WindowSimple\)\.xdb#', t):
            continue
        src = written.get(own_shared, base[own_shared])
        m = re.search(rb'<Background href="([^"#]+)(#[^"]*)"/>', src)
        if not m:
            continue
        folder = screen[:-len(".(WindowScreen).xdb")]  # UI/MPCreate
        texture = m.group(1).decode()
        if not texture.startswith("/"):
            texture = "/" + own_shared.rsplit("/", 1)[0] + "/" + texture
        window = "%s/%s.(WindowSimple).xdb" % (folder, BACKDROP)
        shared = "%s/%s.(WindowSimpleShared).xdb" % (folder, BACKDROP)
        added[window] = ui_mod_parts.backdrop_window("%s.(WindowSimpleShared).xdb" % BACKDROP)
        added[shared] = ui_mod_parts.backdrop_shared(texture + m.group(2).decode())
        # The root keeps no picture; the backdrop hangs last, as a2p1's Back does.
        src = src[:m.start()] + b"<Background/>" + src[m.end():]
        item = b'\t\t<Item href="%s#xpointer(/WindowSimple)"/>' % ("/" + window).encode()
        nl = b"\r\n" if b"\r\n" in src else b"\n"
        end = src.index(b"</Children>")
        start = src.rfind(nl, 0, end) + len(nl)
        written[own_shared] = src[:start] + item + nl + src[start:]
    return added


def stretch_root(data):
    """A root screen's description with its own placement stretched across
    its parent (EPA_MARGIN, no margins): Hammers of Fate has no root
    alignment, and native/ui_bounds.c gives such a root the whole screen
    where Tribes of the East's data says ALIGN_SCREEN."""
    m = PLACEMENT.search(data)
    if not m:
        return data
    block = m.group(0)
    block = re.sub(rb"(<HorAllign>\s*<First>)[A-Z_]+(</First>\s*<Second>)[a-z]+(</Second>)",
                   rb"\1EPA_MARGIN\2true\3", block, count=1)
    block = re.sub(rb"(<(LowerMargin|UpperMargin)>\s*<First>\s*<x>)[-0-9.]+(</x>.*?<Second>)[a-z]+(</Second>)",
                   rb"\g<1>0\3true\4", block, flags=re.S)
    return data[:m.start()] + block + data[m.end():]


# Later than any file the game shipped with, earlier than the phone mod's
# (tools/phone_ui_mod.py), which goes over this one.
ENTRY_DATE = (2030, 6, 1, 12, 0, 0)
# Where the app puts the mod in the player's profile (game.toml's
# profile_files had it there): Hammers of Fate reads no UserMODs, so its
# copy is a pak kit/host mounts over the game's data (native/hof_paks.c).
RECIPE_OUT = {"base": "UserMODs/Widescreen.h5u", "hof": "data/zz-widescreen.pak", "toe": "UserMODs/Widescreen.h5u"}


def write_recipe(args, written, base):
    """The recipe for `written`: each file's edits against the base layers'
    copy it was made from (the screen description renamed -Wide from its
    shadowed name), the curtains and the dialogue strips' tiled backgrounds
    whole, types.xml with ui_mod_parts' lines."""
    sources, authored = {}, set()
    for name in written:
        source = name.replace("-Wide.", ".") if "-Wide." in name else name
        if source in base:
            sources[name] = (source, base[source])
        elif name.startswith((BORDER_DIR, "UI/Common/WideBorder/")) or (
                name.startswith(DIALOGUE + "/") and name.endswith("-Wide.(BackgroundTiledTexture).xdb")) or (
                "/%s.(" % BACKDROP in name):
            authored.add(name)
    nl_parts = ui_mod_parts.types_fields() + ui_mod_parts.types_enum()
    allowed = set(ui_recipe.split_lines(nl_parts)) | set(ui_recipe.split_lines(nl_parts.replace(b"\n", b"\r\n")))
    # patch_types' <NestedTypes> naming the enum, at WindowScreen's indent.
    if "types.xml" in written:
        types = base["types.xml"]
        nested = types.index(b"<NestedTypes/>", types.index(b"<TypeName>WindowScreen</TypeName>"))
        indent = types[types.rfind(b"\n", 0, nested) + 1:nested]
        for nl in (b"\n", b"\r\n"):
            allowed |= {indent + b"<NestedTypes>" + nl, indent + b"</NestedTypes>" + nl,
                        indent + b"\t<Item>" + ui_mod_parts.ALIGN_TYPE + b"</Item>" + nl}
    flags = "".join(" " + a for a, on in (("--game " + args.game, args.game != "base"),
                                         ("--wide-dialogues", args.wide_dialogues)) if on)
    ui_recipe.write(args.recipe, name="Widescreen" + ("" if args.game == "base" else "-" + args.game),
                    out=RECIPE_OUT[args.game], date="%04d-%02d-%02d %02d:%02d:%02d" % ENTRY_DATE,
                    layers={"hof": HOF_PAKS, "toe": TOE_PAKS}.get(args.game, BASE_PAKS), files=written,
                    sources=sources, authored=authored, allowed_lines=allowed,
                    comment="Made by tools/widescreen_mod.py%s; applied by kit/host/ui_mods.cpp." % flags)



# Dialogue scenes (DialogueScreen: a 3D scene between two parchment strips, the
# hero's face and words, Next) across the whole screen instead of a 4:3 block
# between black curtains, as the player asked for: each parchment becomes a
# strip from the screen's left edge to its right one, its two rolled ends kept
# and its middle repeated (a BackgroundTiledTexture: rL, rF tiled, rR, cut
# from the game's own texture at the cuts below), so on a
# 4:3 screen it is the original picture; the rest stays in the middle.
DIALOGUE = "UI/DialogueScreen"
# The cuts: where the repeated middle meets itself and the right end best on
# the common screens (16:10, 16:9, 21:9, 3440x1440, a phone's 19.5:9, 2:1) -
# the last, partial repeat stops at a different column on each.
DIALOGUE_STRIPS = {  # window: (texture, height, fill from, fill to)
    "DialogueTop": ("/UI/DialogueScreen/DialogueTop.(Texture).xdb", 64, 200, 840),
    "DialoguePanel": ("/UI/DialogueScreen/DialoguePanel.(Texture).xdb", 256, 190, 908),
}
TILE_PARTS = ("rLT", "rRT", "rLB", "rRB", "rT", "rL", "rR", "rB", "rF")


def tiled_strip(texture, height, x1, x2, width=1024):
    """A BackgroundTiledTexture of one row: the texture's [0, x1) at the left
    edge, [x2, width) at the right one, [x1, x2) repeated between them."""
    rects = {"rL": (0, x1), "rF": (x1, x2), "rR": (x2, width)}
    parts = []
    for name in TILE_PARTS:
        a, b = rects.get(name, (0, 0))
        h = height if name in rects else 0
        parts.append("\t<%s>\n\t\t<Size>\n\t\t\t<x>%d</x>\n\t\t\t<y>%d</y>\n\t\t</Size>\n"
                     "\t\t<Maps>\n\t\t\t<x1>%d</x1>\n\t\t\t<y1>0</y1>\n\t\t\t<x2>%d</x2>\n\t\t\t<y2>%d</y2>\n"
                     "\t\t</Maps>\n\t\t<Rect>\n\t\t\t<x1>0</x1>\n\t\t\t<y1>0</y1>\n\t\t\t<x2>0</x2>\n"
                     "\t\t\t<y2>0</y2>\n\t\t</Rect>\n\t\t<Rotate>0</Rotate>\n\t</%s>\n"
                     % (name, b - a, h, a, b, h, name))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<BackgroundTiledTexture>\n'
            "\t<ClassTypeID>285694786</ClassTypeID>\n"
            '\t<Texture href="%s#xpointer(/Texture)"/>\n\t<Color>-1</Color>\n%s'
            "</BackgroundTiledTexture>\n" % (texture, "".join(parts))).encode()


def set_axis(data, anchor, x, margins=False):
    """A window description's first <Placement> anchored across by `anchor`
    at x (set here); with margins, both margins across set to 0 here."""
    m = PLACEMENT.search(data)
    block = m.group(0)
    block = re.sub(rb"(<HorAllign>\s*<First>)[A-Z_]+(</First>\s*<Second>)[a-z]+(</Second>)",
                   rb"\g<1>" + anchor.encode() + rb"\2true\3", block, count=1)
    block = re.sub(rb"(<Position>\s*<First>\s*<x>)[-0-9.]+(</x>.*?<Second>)[a-z]+(</Second>)",
                   rb"\g<1>" + str(x).encode() + rb"\2true\3", block, count=1, flags=re.S)
    if margins:
        block = re.sub(rb"(<(LowerMargin|UpperMargin)>\s*<First>\s*<x>)[-0-9.]+(</x>.*?<Second>)[a-z]+(</Second>)",
                       rb"\g<1>0\3true\4", block, flags=re.S)
    return data[:m.start()] + block + data[m.end():]


def wide_dialogues(layer, written, game):
    """{name: bytes} of the dialogue screen laid out across the whole screen
    (see DIALOGUE), from the files as written so far over the game's layers."""
    get = lambda n: written.get(n, layer.get(n))
    out = {}
    shared_name = DIALOGUE + ".(WindowScreenShared).xdb"
    shared = get(shared_name)
    # The side curtains go; those above and below stay (a screen taller than 4:3).
    shared = re.sub(rb'[ \t]*<Item href="[^"]*Common/BlackBorder/(Left|Right)\.\(WindowSimple\)\.xdb'
                    rb'#xpointer\(/WindowSimple\)"/>\r?\n', b"", shared)
    out[shared_name] = shared
    screen_name = DIALOGUE + ".(WindowScreen).xdb"
    screen = get(screen_name)
    if game == "hof":
        screen = stretch_root(screen)  # native/ui_bounds.c spans it across the screen
    elif b"<HorAlign>" not in screen:
        nl = b"\r\n" if b"\r\n" in screen else b"\n"
        end = screen.rindex(b"</WindowScreen>")
        screen = (screen[:end] + b"\t<HorAlign>ALIGN_SCREEN</HorAlign>" + nl +
                  b"\t<VerAlign>ALIGN_CENTER</VerAlign>" + nl + screen[end:])
    else:
        screen = re.sub(rb"<HorAlign>[A-Z_]+</HorAlign>", b"<HorAlign>ALIGN_SCREEN</HorAlign>", screen)
    out[screen_name] = screen
    for href in re.findall(rb'<Item href="(DialogueScreen/[^"#]+)#', shared):
        name = "UI/" + href.decode()
        data = get(name)
        if data is None:
            continue
        stem = name.rsplit("/", 1)[-1].split(".", 1)[0]
        if stem in DIALOGUE_STRIPS:
            texture, height, x1, x2 = DIALOGUE_STRIPS[stem]
            out[name] = set_axis(data, "EPA_MARGIN", 0, margins=True)
            own_shared = "%s/%s.(WindowSimpleShared).xdb" % (DIALOGUE, stem)
            tiled = "%s-Wide.(BackgroundTiledTexture).xdb" % stem
            out["%s/%s" % (DIALOGUE, tiled)] = tiled_strip(texture, height, x1, x2)
            out[own_shared] = re.sub(rb'<Background href="[^"]*"/>',
                                     b'<Background href="%s#xpointer(/BackgroundTiledTexture)"/>' %
                                     tiled.encode(), get(own_shared), count=1)
            continue
        # The rest keeps its place in the middle: its centre's distance from
        # the screen's (ERA_CENTER), the size its own or its shared one's.
        block = PLACEMENT.search(data)
        if not block:
            continue
        v = read_placement(block.group(0))[1]
        if v["HorAllign"][0] != "EPA_LOW_END":
            continue
        size = v["Size"][0] if v["Size"][2] == "true" and v["Size"][0] else None
        own = inherited_size(layer, name, data)
        if size is None and own:
            size = own[0]
        x = v["Position"][0]
        if v["Position"][2] != "true":
            shared_block = PLACEMENT.search(get(name.replace(").xdb", "Shared).xdb")) or b"")
            x = read_placement(shared_block.group(0))[1]["Position"][0] if shared_block else 0
        if size is None:
            continue
        out[name] = set_axis(data, "ERA_CENTER", x - (1024 - size) // 2)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--base-game", type=Path, required=True, help="the base game's install folder")
    p.add_argument("--toe-data", type=Path, required=True, help="Tribes of the East's data/ folder")
    p.add_argument("--out", type=Path, required=True, help="the .h5u to write")
    p.add_argument("--report", type=Path, help="a JSON report of the merge")
    p.add_argument("--recipe", type=Path, help="also write the recipe (tools/ui_recipe.py) the app makes it from")
    p.add_argument("--game", choices=("base", "hof", "toe"), default="base",
                   help="whose files to patch: the base game (default), Hammers of Fate, or "
                        "Tribes of the East (its own layout kept: only --wide-dialogues)")
    p.add_argument("--wide-dialogues", action="store_true",
                   help="lay the dialogue scenes out across the whole screen (DIALOGUE)")
    p.add_argument("--borders-only", action="store_true",
                   help="write only the black borders and the screens they hang on")
    p.add_argument("--border", action="append", default=[], metavar="UI/X.(WindowScreenShared).xdb[=HREF]",
                   help="also hang the black borders on this screen (repeatable); with =HREF the "
                        "borders are painted with that absolute background href instead")
    args = p.parse_args()

    paks = {"hof": HOF_PAKS, "toe": TOE_PAKS}.get(args.game, BASE_PAKS)
    base = read_layers([args.base_game / name for name in paks])
    toe_old = read_layers([args.toe_data / "data.pak"])
    toe_new = read_layers([args.toe_data / "a2p1-data.pak"])
    if not base or not toe_new:
        sys.exit("No UI files found: check --base-game and --toe-data")
    check_parts(toe_new)
    parts = ui_mod_parts.black_borders()

    report = {"applied": {}, "partial": {}, "skipped": {}}
    written = {}
    for name in sorted(toe_new) if args.game != "toe" else []:
        if name.startswith(BORDER_DIR) and name not in base:
            if name not in parts:
                sys.exit("%s: a curtain ui_mod_parts.py does not write" % name)
            written[name] = parts[name]
            report["applied"][name] = "black border, copied"
            continue
        if name not in base:
            report["skipped"][name] = "not in the base game"
            continue
        if name not in toe_old:
            report["skipped"][name] = "not in Tribes of the East's data.pak"
            continue
        # a2p1 carries only what it changed: the shared descriptions the
        # sizes come from are the base layer's and Tribes of the East's own.
        sizes = [inherited_size(layer, name, layer[name]) for layer in (base, toe_old)]
        merged, moved, left, tags = merge_file(base[name], toe_old[name], toe_new[name],
                                               sizes if all(sizes) else None)
        entry = {"placements": moved, "left_out": left, "screen_tags": tags}
        if merged is None:
            report["skipped"][name] = entry
            continue
        written[name] = merged
        report["partial" if left else "applied"][name] = entry

    if args.game in ("base", "hof"):
        for name, data in move_backdrops(written, base, toe_new).items():
            written[name] = data
            report["applied"][name] = "the root's background on a centred window"
    if args.game == "base":
        written["types.xml"] = patch_types(base["types.xml"])
    elif args.game == "hof":
        for name in sorted(toe_new):
            if (name.endswith(".(WindowScreen).xdb") and name in base
                    and re.search(rb"<HorAlign>ALIGN_SCREEN</HorAlign>", toe_new[name])):
                written[name] = stretch_root(written.get(name, base[name]))
                report["applied"].setdefault(name, {})
                if isinstance(report["applied"][name], dict):
                    report["applied"][name]["stretched_root"] = True
    for spec in args.border:
        name, _, background = spec.partition("=")
        src = written.get(name, base.get(name))
        if src is None:
            sys.exit("%s is not among the game's UI files" % name)
        if background:
            files, items = backdrop_borders(name, background, parts)
            written.update(files)
            # Like Tribes of the East's loading screen, the screen itself is
            # painted with the backdrop too, under whatever picture it shows.
            src = src.replace(b"<Background/>", b'<Background href="%s"/>' % background.encode(), 1)
            written[name] = add_borders(src, items)
            report["applied"][name] = "borders and background painted with " + background
        else:
            written[name] = add_borders(src)
            report["applied"][name] = "black borders added"
    if args.wide_dialogues:
        for name, data in wide_dialogues(base, written, args.game).items():
            written[name] = data
            report["applied"][name] = "dialogue scene across the screen"
    if args.borders_only:
        written = {name: data for name, data in written.items()
                   if name.startswith((BORDER_DIR, "UI/Common/WideBorder/"))
                   or b"Common/BlackBorder/" in data or b"Common/WideBorder/" in data}
    if args.game == "hof":
        top = set(read_layers([args.base_game / n for n in HOF_TOP_PAKS]))
        for name in unshadow(written, base, top):
            report["partial"][name] = "also in dataa1: needs the pak mounted above it (native/hof_paks.c)"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in written.items():
            info = zipfile.ZipInfo(name, date_time=ENTRY_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    if args.recipe:
        write_recipe(args, written, base)
    if args.report:
        args.report.write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n")
    print("%s: %d files (%d complete, %d partly); %d left out"
          % (args.out, len(written), len(report["applied"]), len(report["partial"]),
             len(report["skipped"])))


if __name__ == "__main__":
    main()

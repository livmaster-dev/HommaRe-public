"""Recipes for the interface mods: what a mod changes in the game's own UI
files, without the files. tools/widescreen_mod.py and tools/phone_ui_mod.py
write one beside (or instead of) the mod they build from the developer's
game; the app applies it to the player's own install when the game starts
(kit/host/ui_mods.cpp) and writes the mod into the player's profile. So no
file of the game, or made from one, ships with the app or in the repository.

A recipe is text, one record per line, fields separated by tabs:

    recipe  1
    name    <mod name>
    out     <profile path of the mod; phone/... only on an iPhone,
            touch/... only with native touch (host.touch/native)>
    date    YYYY-MM-DD HH:MM:SS      the date its zip entries carry
    over    <out of a recipe applied first>   (optional) whose files are
            taken before the layers', as the generator took them
    layer   <pak under the game's folder>    in load order, later over earlier
    file    <name> <sha256 of its source> [<source name, when another>]
    turn    left                             (format 2) the source, a 32-bit
            uncompressed DDS without mips, turned a quarter to the left
            (tools/touch_ui_mod.py); a picture takes no line edits
    @       <source line> <lines removed>    an edit; the lines it puts in follow
    new     <name>                           a file the port writes whole
    new64   <name>                           (format 3) the same for a picture
            the port drew: its bytes in base64, one + line per 76 characters
    +<text>  *<text>  =<text>                a line ending in LF, CRLF, nothing
    compose <name> <width> <height>          (format 4) a picture made on the device: an
                                             uncompressed 32-bit DDS without mips, transparent,
                                             then the steps below in order
    src     <source name> <sha256>           a picture of the game's it takes from (the same
                                             kind of DDS), numbered from 0 in their order
    blit    <src> <sx> <sy> <sw> <sh> <dx> <dy> <dw> <dh>
                                             that source's rectangle into this one, scaled
                                             to it by the nearest pixel, replacing
    fill    <dx> <dy> <dw> <dh> <RRGGBBAA>   a rectangle of one colour, replacing
    lay     <name> <dx> <dy>                 a new64 picture of this recipe (such a DDS),
                                             laid over at that place by its alpha

Line numbers count from 0 in the source file split after each LF; edits come
in source order and do not overlap. A file whose source differs from its
recorded SHA-256 (another build of the game) is left out of the mod. Lines a
recipe puts in may only hold interface layout (placements, anchors,
visibility, backgrounds, window references); `check_lines` keeps the game's
texts and descriptions out.
"""

import base64
import difflib
import hashlib
import re
import struct

FORMAT = 4
# Formats this reader takes: 1 has no `turn`, 2 no `new64`, 3 no `compose`.
FORMATS = (1, 2, 3, 4)

# What the edits may write: a window's placement (positions, sizes, anchors
# and their flags), a root screen's alignment, whether it (or an option) is
# shown, the background, foreground or shared description it names, and the
# windows it holds; a texture's description: the picture it names, its size,
# its source name and its record id.
LAYOUT_LINE = re.compile(
    rb"^\s*(</?(Placement|Position|Size|LowerMargin|UpperMargin|HorAllign|VerAllign|First|Second|x|y)>"
    rb"[-A-Za-z0-9_.]*(</(First|Second|x|y)>)?"
    rb"|<(HorAlign|VerAlign)>ALIGN_[A-Z_]+</(HorAlign|VerAlign)>"
    rb"|<(Visible|Hide)>(true|false)</(Visible|Hide)>"
    rb"|<(Background|Foreground|Shared|Item|Texture|DestName|SrcName) href=\"[^\"]*\"/>"
    rb"|<(Width|Height)>\d+</(Width|Height)>"
    rb"|<[A-Za-z]+ ObjectRecordID=\"\d+\">"
    rb"|<Background/>)\s*$")


def split_lines(data):
    """The lines of data, each with its LF (a CR before it stays in the line)."""
    return re.findall(rb"[^\n]*\n|[^\n]+$", data)


def edits(source, target):
    """[(source line, lines removed, [lines put in])] that turn source into target."""
    a, b = split_lines(source), split_lines(target)
    out = []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if tag != "equal":
            out.append((i1, i2 - i1, b[j1:j2]))
    return out


def apply_edits(source, file_edits):
    lines = split_lines(source)
    for at, removed, added in reversed(file_edits):
        lines[at:at + removed] = added
    return b"".join(lines)


def turn_left(dds):
    """An uncompressed 32-bit DDS without mips turned a quarter to the left
    (the new row r is the old column width-1-r, top down)."""
    if dds[:4] != b"DDS " or len(dds) < 128:
        raise ValueError("not a DDS")
    height, width = struct.unpack_from("<II", dds, 12)
    bits, = struct.unpack_from("<I", dds, 88)
    if bits != 32 or len(dds) < 128 + width * height * 4:
        raise ValueError("not an uncompressed 32-bit DDS")
    px = dds[128:128 + width * height * 4]
    rows = [px[y * width * 4:(y + 1) * width * 4] for y in range(height)]
    out = bytearray()
    for r in range(width):
        col = width - 1 - r
        for y in range(height):
            out += rows[y][col * 4:col * 4 + 4]
    head = bytearray(dds[:128])
    struct.pack_into("<II", head, 12, width, height)
    return bytes(head) + bytes(out) + dds[128 + width * height * 4:]


TURNS = {"left": turn_left}


def dds_pixels(dds):
    """(width, height, BGRA bytes) of an uncompressed 32-bit DDS without mips."""
    if dds[:4] != b"DDS " or len(dds) < 128:
        raise ValueError("not a DDS")
    height, width = struct.unpack_from("<II", dds, 12)
    bits, = struct.unpack_from("<I", dds, 88)
    if bits != 32 or len(dds) < 128 + width * height * 4:
        raise ValueError("not an uncompressed 32-bit DDS")
    return width, height, bytearray(dds[128:128 + width * height * 4])


def dds_bytes(width, height, bgra):
    """An uncompressed 32-bit DDS without mips (BGRA in memory), as the game's
    interface pictures are."""
    head = bytearray(128)
    head[0:4] = b"DDS "
    struct.pack_into("<7I", head, 4, 124, 0x1007, height, width, 0, 0, 1)  # size, flags, height, width, pitch, depth, mips
    struct.pack_into("<8I", head, 76, 32, 0x41, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000)
    struct.pack_into("<I", head, 108, 0x1000)                             # caps: a texture
    return bytes(head) + bytes(bgra)


def compose(width, height, sources, steps, pictures):
    """The picture a `compose` record makes: sources [DDS bytes] in order,
    steps [("blit", src, sx, sy, sw, sh, dx, dy, dw, dh) | ("fill", dx, dy, dw,
    dh, "RRGGBBAA") | ("lay", name, dx, dy)], pictures {name: DDS bytes}.
    The reference for kit/host/ui_mods.cpp compose()."""
    out = bytearray(width * height * 4)
    srcs = [dds_pixels(s) for s in sources]

    def inside(x, y):
        return 0 <= x < width and 0 <= y < height

    for step in steps:
        if step[0] == "blit":
            _, i, sx, sy, sw, sh, dx, dy, dw, dh = step
            w, h, px = srcs[i]
            for y in range(dh):
                ty = sy + y * sh // dh
                for x in range(dw):
                    tx = sx + x * sw // dw
                    if inside(dx + x, dy + y) and 0 <= tx < w and 0 <= ty < h:
                        o, q = ((dy + y) * width + dx + x) * 4, (ty * w + tx) * 4
                        out[o:o + 4] = px[q:q + 4]
        elif step[0] == "fill":
            _, dx, dy, dw, dh, colour = step
            v = int(colour, 16)
            bgra = bytes(((v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255, v & 255))
            for y in range(dy, dy + dh):
                for x in range(dx, dx + dw):
                    if inside(x, y):
                        out[(y * width + x) * 4:(y * width + x) * 4 + 4] = bgra
        elif step[0] == "lay":
            _, name, dx, dy = step
            w, h, px = dds_pixels(pictures[name])
            for y in range(h):
                for x in range(w):
                    if not inside(dx + x, dy + y):
                        continue
                    q, o = (y * w + x) * 4, ((dy + y) * width + dx + x) * 4
                    a = px[q + 3]
                    if a == 0:
                        continue
                    da = out[o + 3]
                    oa = a + da * (255 - a) // 255
                    for c in range(3):
                        out[o + c] = (px[q + c] * a + out[o + c] * da * (255 - a) // 255) // oa if oa else 0
                    out[o + 3] = oa
        else:
            raise ValueError("unknown step %r" % (step[0],))
    return dds_bytes(width, height, out)


def check_lines(name, lines, allowed=()):
    """The lines that are not interface layout (nor in `allowed`)."""
    return ["%s: %r" % (name, line) for line in lines
            if not LAYOUT_LINE.match(line) and line not in allowed]


def _line(raw):
    if raw.endswith(b"\r\n"):
        return b"*" + raw[:-2]
    if raw.endswith(b"\n"):
        return b"+" + raw[:-1]
    return b"=" + raw


def _unline(rec):
    kind, text = rec[:1], rec[1:]
    return text + {b"*": b"\r\n", b"+": b"\n", b"=": b""}[kind]


def write(path, *, name, out, date, layers, files, sources, authored=(), over=None, allowed_lines=(),
          turned=(), drawn=(), composed=None, comment=""):
    """Write the recipe for `files` ({name: bytes}, the mod's contents).
    sources: {name: (source name, source bytes)} for each file made from one
    of the game's; authored: the names written whole (ui_mod_parts.py);
    turned: {name: turn} for the pictures made by turning their source;
    drawn: the pictures written whole, in base64 (the port's own art);
    composed: {name: (width, height, [(source name, source bytes)], steps)}
    for the pictures made on the device (compose; the files' bytes must be
    what compose() makes of them).
    Raises ValueError when a file has neither, or an edit writes anything but
    layout (or one of allowed_lines)."""
    out_lines = [b"# " + c.encode() for c in comment.splitlines()]
    # The oldest format that holds it, for the apps that read only that.
    composed = composed or {}
    version = FORMAT if composed else 3 if any(n in files for n in drawn) else 2
    out_lines += [b"recipe\t%d" % version, b"name\t" + name.encode(), b"out\t" + out.encode(),
                  b"date\t" + date.encode()]
    if over:
        out_lines.append(b"over\t" + over.encode())
    out_lines += [b"layer\t" + layer.encode() for layer in layers]
    bad = []
    for fname in sorted(files):
        data = files[fname]
        if fname in sources:
            src_name, src = sources[fname]
            head = b"file\t%s\t%s" % (fname.encode(), hashlib.sha256(src).hexdigest().encode())
            if src_name != fname:
                head += b"\t" + src_name.encode()
            out_lines.append(head)
            if fname in turned:
                out_lines.append(b"turn\t" + turned[fname].encode())
                if TURNS[turned[fname]](src) != data:
                    raise ValueError("%s: not its source turned %s" % (fname, turned[fname]))
                continue
            for at, removed, added in edits(src, data):
                bad += check_lines(fname, added, allowed_lines)
                out_lines.append(b"@\t%d\t%d" % (at, removed))
                out_lines += [_line(raw) for raw in added]
        elif fname in authored:
            out_lines.append(b"new\t" + fname.encode())
            out_lines += [_line(raw) for raw in split_lines(data)]
        elif fname in composed:
            width, height, srcs, steps = composed[fname]
            pics = {n: files[n] for n in drawn if n in files}
            if compose(width, height, [b for _, b in srcs], steps, pics) != data:
                raise ValueError("%s: not what its steps make" % fname)
            out_lines.append(b"compose\t%s\t%d\t%d" % (fname.encode(), width, height))
            out_lines += [b"src\t%s\t%s" % (n.encode(), hashlib.sha256(b).hexdigest().encode()) for n, b in srcs]
            out_lines += [b"\t".join(str(v).encode() for v in step) for step in steps]
        elif fname in drawn:
            out_lines.append(b"new64\t" + fname.encode())
            text = base64.b64encode(data)
            out_lines += [b"+" + text[i:i + 76] for i in range(0, len(text), 76)]
        else:
            raise ValueError("%s: neither made from a game file nor written by the port" % fname)
    if bad:
        raise ValueError("lines that are not interface layout:\n  " + "\n  ".join(bad[:20]))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\n".join(out_lines) + b"\n")


def read(path):
    """The recipe as {"name", "out", "date", "over", "layers", "files": [...]};
    each file {"name", "sha256", "source", "edits", "turn"} or {"name", "text"}."""
    rec = {"over": None, "layers": [], "files": []}
    cur = None
    for raw in path.read_bytes().split(b"\n"):
        if not raw or raw.startswith(b"#"):
            continue
        if raw[:1] in b"+*=":
            line = _unline(raw)
            if "b64" in cur:
                cur["b64"] += line.rstrip(b"\n")
            elif "text" in cur:
                cur["text"] += line
            else:
                cur["edits"][-1][2].append(line)
            continue
        key, *vals = raw.decode().split("\t")
        if key == "recipe":
            if int(vals[0]) not in FORMATS:
                raise ValueError("%s: recipe format %s" % (path, vals[0]))
        elif key in ("name", "out", "date", "over"):
            rec[key] = vals[0]
        elif key == "layer":
            rec["layers"].append(vals[0])
        elif key == "file":
            cur = {"name": vals[0], "sha256": vals[1], "source": vals[2] if len(vals) > 2 else vals[0],
                   "edits": [], "turn": None}
            rec["files"].append(cur)
        elif key == "turn":
            if vals[0] not in TURNS:
                raise ValueError("%s: unknown turn %r" % (path, vals[0]))
            cur["turn"] = vals[0]
        elif key == "@":
            cur["edits"].append((int(vals[0]), int(vals[1]), []))
        elif key == "new":
            cur = {"name": vals[0], "text": b""}
            rec["files"].append(cur)
        elif key == "new64":
            cur = {"name": vals[0], "text": b"", "b64": b""}
            rec["files"].append(cur)
        elif key == "compose":
            cur = {"name": vals[0], "compose": (int(vals[1]), int(vals[2])), "srcs": [], "steps": []}
            rec["files"].append(cur)
        elif key == "src" and cur and "compose" in cur:
            cur["srcs"].append((vals[0], vals[1]))
        elif key in ("blit", "fill", "lay") and cur and "compose" in cur:
            if key == "blit":
                step = (key, *[int(v) for v in vals[:9]])
            elif key == "fill":
                step = (key, *[int(v) for v in vals[:4]], vals[4])
            else:
                step = (key, vals[0], int(vals[1]), int(vals[2]))
            cur["steps"].append(step)
        else:
            raise ValueError("%s: unknown record %r" % (path, key))
    for f in rec["files"]:
        if "b64" in f:
            f["text"] = base64.b64decode(f.pop("b64"), validate=True)
    return rec


def apply(rec, layers, over=None):
    """{name: bytes} of the mod, from the recipe and the game's UI files
    (`layers`: {name: bytes}, later paks over earlier) and the mod it goes
    over ({name: bytes}); and the names left out (source missing or another
    build). The reference for kit/host/ui_mods.cpp."""
    over = over or {}
    files, left = {}, []
    pictures = {f["name"]: f["text"] for f in rec["files"] if "text" in f}
    for f in rec["files"]:
        if "text" in f:
            files[f["name"]] = f["text"]
            continue
        if "compose" in f:
            srcs = [over.get(n, layers.get(n)) for n, _ in f["srcs"]]
            if any(b is None or hashlib.sha256(b).hexdigest() != sha for b, (_, sha) in zip(srcs, f["srcs"])):
                left.append(f["name"])
                continue
            files[f["name"]] = compose(*f["compose"], srcs, f["steps"], pictures)
            continue
        src = over.get(f["source"], layers.get(f["source"]))
        if src is None or hashlib.sha256(src).hexdigest() != f["sha256"]:
            left.append(f["name"])
            continue
        if f["turn"]:
            src = TURNS[f["turn"]](src)
        files[f["name"]] = apply_edits(src, f["edits"])
    return files, left

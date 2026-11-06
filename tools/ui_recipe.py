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

FORMAT = 3
# Formats this reader takes: 1 has no `turn`, 2 no `new64`.
FORMATS = (1, 2, 3)

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
          turned=(), drawn=(), comment=""):
    """Write the recipe for `files` ({name: bytes}, the mod's contents).
    sources: {name: (source name, source bytes)} for each file made from one
    of the game's; authored: the names written whole (ui_mod_parts.py);
    turned: {name: turn} for the pictures made by turning their source;
    drawn: the pictures written whole, in base64 (the port's own art).
    Raises ValueError when a file has neither, or an edit writes anything but
    layout (or one of allowed_lines)."""
    out_lines = [b"# " + c.encode() for c in comment.splitlines()]
    # The oldest format that holds it, for the apps that read only that.
    version = FORMAT if any(n in files for n in drawn) else 2
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
    for f in rec["files"]:
        if "text" in f:
            files[f["name"]] = f["text"]
            continue
        src = over.get(f["source"], layers.get(f["source"]))
        if src is None or hashlib.sha256(src).hexdigest() != f["sha256"]:
            left.append(f["name"])
            continue
        if f["turn"]:
            src = TURNS[f["turn"]](src)
        files[f["name"]] = apply_edits(src, f["edits"])
    return files, left

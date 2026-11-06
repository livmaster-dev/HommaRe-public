"""The few interface files the wide-screen mod adds that the base game has no
copy of, written here rather than taken from Tribes of the East's data: the
black curtains beside a letterboxed screen (four windows of one plain
colour) and the types.xml declarations that let the 1.60 exe read a root
screen's <HorAlign>/<VerAlign> (design notes ultrawide-plan.md). They describe what
Tribes of the East's patch 3.1 does with the same file formats, so the game
reads them the same way; tools/widescreen_mod.py checks that against the
developer's own copy when it has one.

The game's own XML is CRLF-terminated, tab-indented; these are too."""

BORDER_DIR = "UI/Common/BlackBorder/"
BORDER_SHARED = "Left.(WindowSimpleShared).xdb"
BORDER_BACK = "back.(BackgroundSimpleTexture).xdb"

# Each curtain sits a whole screen (1024 x 768 interface units) off one edge,
# anchored to it, and takes its size from the shared description: so it
# covers whatever lies beyond that edge on a wider screen.
# side: (x, y, vertical anchor, set here, horizontal anchor, set here)
SIDES = {
    "Left": (-1024, 0, "EPA_LOW_END", False, "EPA_LOW_END", True),
    "Right": (1024, 0, "EPA_LOW_END", False, "EPA_HIGH_END", True),
    "Top": (0, -768, "EPA_LOW_END", True, "EPA_LOW_END", False),
    "Bottom": (0, 768, "EPA_HIGH_END", True, "EPA_LOW_END", False),
}

# The Align enum's type id in types.xml, which the root screens' new fields
# refer to.
ALIGN_TYPE = b"37077649"


def _crlf(lines):
    return ("\r\n".join(lines) + "\r\n").encode()


def _flag(v):
    return "true" if v else "false"


def _pair(tag, x, y, second, indent="\t\t"):
    return [indent + "<%s>" % tag, indent + "\t<First>", indent + "\t\t<x>%d</x>" % x,
            indent + "\t\t<y>%d</y>" % y, indent + "\t</First>",
            indent + "\t<Second>%s</Second>" % _flag(second), indent + "</%s>" % tag]


def _anchor(tag, value, second, indent="\t\t"):
    return [indent + "<%s>" % tag, indent + "\t<First>%s</First>" % value,
            indent + "\t<Second>%s</Second>" % _flag(second), indent + "</%s>" % tag]


def _placement(x, y, vert, vert_set, hor, hor_set, size, size_set, pos_set):
    return (["\t<Placement>"] + _pair("Position", x, y, pos_set) + _anchor("VerAllign", vert, vert_set)
            + _anchor("HorAllign", hor, hor_set) + _pair("Size", size[0], size[1], size_set)
            + _pair("LowerMargin", 0, 0, False) + _pair("UpperMargin", 0, 0, False) + ["\t</Placement>"])


def border_window(side):
    """A curtain's window: where it sits, its look from BORDER_SHARED."""
    x, y, vert, vert_set, hor, hor_set = SIDES[side]
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<WindowSimple>",
                  "\t<ClassTypeID>285700801</ClassTypeID>",
                  '\t<Shared href="%s#xpointer(/WindowSimpleShared)"/>' % BORDER_SHARED,
                  "\t<Name/>", '\t<TooltipFileRef href=""/>', "\t<Visible>true</Visible>",
                  "\t<Priority>0</Priority>", "\t<GameMessageReactions/>"]
                 + _placement(x, y, vert, vert_set, hor, hor_set, (0, 0), False, True)
                 + ["\t<Enabled>true</Enabled>", "\t<TextString/>", "\t<relatedTexts/>",
                    "\t<relatedTextures/>", "\t<Flags>", "\t\t<SuperActive>false</SuperActive>",
                    "\t</Flags>", "</WindowSimple>"])


def border_shared(background):
    """The curtains' shared description: a screen-sized opaque window painted
    with `background` (an href)."""
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<WindowSimpleShared>",
                  "\t<Crap>false</Crap>", "\t<Children/>", '\t<Background href="%s"/>' % background,
                  "\t<Foreground/>", "\t<Flags>", "\t\t<Transparent>false</Transparent>", "\t</Flags>"]
                 + _placement(0, 0, "EPA_MARGIN", True, "EPA_MARGIN", True, (1024, 768), True, False)
                 + ["\t<ActiveArea/>", "\t<IgnoreDblClick>false</IgnoreDblClick>", "\t<ActiveMask/>",
                    "</WindowSimpleShared>"])


def border_background():
    """Plain black (ARGB 0xff000000), no texture."""
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<BackgroundSimpleTexture>",
                  "\t<ClassTypeID>285694785</ClassTypeID>", "\t<Texture/>", "\t<Color>-16777216</Color>",
                  "\t<TextureX>EPA_LOW_END</TextureX>", "\t<TextureY>EPA_LOW_END</TextureY>",
                  "</BackgroundSimpleTexture>"])


def black_borders():
    """{name: bytes} of UI/Common/BlackBorder/."""
    files = {BORDER_DIR + "%s.(WindowSimple).xdb" % side: border_window(side) for side in SIDES}
    files[BORDER_DIR + BORDER_SHARED] = border_shared(
        "%s#xpointer(/BackgroundSimpleTexture)" % BORDER_BACK)
    files[BORDER_DIR + BORDER_BACK] = border_background()
    return files


# A screen's backdrop as a window of its own, centred: where Tribes of the
# East's patch 3.1 took a root screen's background off the root (which stays
# at the top left) and hung it on a centred 1024 x 768 child, so the picture
# moves with the windows a2p1 re-anchored to the centre (the network game's
# create screen, whose parchment carries its panels' frames).
def backdrop_window(shared_leaf):
    """The backdrop's window: centred, its size from `shared_leaf`."""
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<WindowSimple>",
                  "\t<ClassTypeID>285700801</ClassTypeID>",
                  '\t<Shared href="%s#xpointer(/WindowSimpleShared)"/>' % shared_leaf,
                  "\t<Name/>", '\t<TooltipFileRef href=""/>', "\t<Visible>true</Visible>",
                  "\t<Priority>0</Priority>", "\t<GameMessageReactions/>"]
                 + _placement(0, 0, "ERA_CENTER", True, "ERA_CENTER", True, (0, 0), False, False)
                 + ["\t<Enabled>true</Enabled>", "\t<TextString/>", "\t<relatedTexts/>",
                    "\t<relatedTextures/>", "\t<Flags>", "\t\t<SuperActive>false</SuperActive>",
                    "\t</Flags>", "</WindowSimple>"])


def backdrop_shared(background):
    """The backdrop's shared description: a 1024 x 768 window painted with
    `background` (an href to the screen's own background texture)."""
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<WindowSimpleShared>",
                  "\t<Crap>false</Crap>", "\t<Children/>", '\t<Background href="%s"/>' % background,
                  "\t<Foreground/>", "\t<Flags>", "\t\t<Transparent>false</Transparent>", "\t</Flags>"]
                 + _placement(0, 0, "EPA_LOW_END", False, "EPA_LOW_END", False, (1024, 768), False, False)
                 + ["\t<ActiveArea/>", "\t<IgnoreDblClick>false</IgnoreDblClick>", "\t<ActiveMask/>",
                    "</WindowSimpleShared>"])


def types_fields():
    """The root screen's two fields (WindowScreen chunks 5 and 6), as items of
    its <Fields>, tab-indented for that place in types.xml, LF-terminated
    (patch_types matches the file's own line ends)."""
    out = []
    for name, chunk, default in (("HorAlign", 5, "ALIGN_CENTER"), ("VerAlign", 6, "ALIGN_LOW_END")):
        out += ["\t\t\t\t<Item>", "\t\t\t\t\t<Type>%s</Type>" % ALIGN_TYPE.decode(),
                "\t\t\t\t\t<Name>%s</Name>" % name, "\t\t\t\t\t<ChunkID>%d</ChunkID>" % chunk,
                "\t\t\t\t\t<Description>how a root screen sits on a screen wider than 4:3</Description>",
                "\t\t\t\t\t<Constraints/>", "\t\t\t\t\t<Attributes/>", "\t\t\t\t\t<DefaultValue>",
                "\t\t\t\t\t\t<Type>03000000</Type>", "\t\t\t\t\t\t<Data>%s</Data>" % default,
                "\t\t\t\t\t</DefaultValue>", "\t\t\t\t\t<ComplexDefaultValue>",
                "\t\t\t\t\t\t<Type>00000000</Type>", "\t\t\t\t\t</ComplexDefaultValue>", "\t\t\t\t</Item>"]
    return ("\n".join(out) + "\n").encode()


def types_enum():
    """The Align enum as an item of <SharedClasses>, LF-terminated. Its names
    are the exe's (ALIGN_HIGN_END as it spells it)."""
    out = ["\t\t<Item>", "\t\t\t<__ClassTypeID>270082824</__ClassTypeID>",
           "\t\t\t<__ServerPtr>%s</__ServerPtr>" % ALIGN_TYPE.decode(), "\t\t\t<Type>TYPE_TYPE_ENUM</Type>",
           "\t\t\t<TypeName>Align</TypeName>", "\t\t\t<EnclosingNamespace>d804301f</EnclosingNamespace>",
           "\t\t\t<Entries>"]
    for value, name in enumerate(("ALIGN_LOW_END", "ALIGN_CENTER", "ALIGN_HIGN_END", "ALIGN_SCREEN")):
        out += ["\t\t\t\t<Item>", "\t\t\t\t\t<Name>%s</Name>" % name, "\t\t\t\t\t<Value>%d</Value>" % value,
                "\t\t\t\t</Item>"]
    out += ["\t\t\t</Entries>", "\t\t\t<Attributes/>", "\t\t</Item>"]
    return ("\n".join(out) + "\n").encode()

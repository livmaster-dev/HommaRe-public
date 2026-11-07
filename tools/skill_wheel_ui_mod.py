#!/usr/bin/env python3
"""Build a user mod that puts the skill wheel's button in the game's own windows.

    tools/skill_wheel_ui_mod.py --base-game ~/Heroes\\ V/game --out build/SkillWheelUI.h5u

The skill wheel (native-screens/SkillWheel.swift) opens on the hero a window
shows: the level-up box's (NUI::CLevelUpBox) and a hero screen's. The mod gives
both windows a button of their own, drawn and placed by the game like their
other buttons, so it scales with them on a phone and stays where it belongs:

  - the level-up box: a plate left of OK, as wide as the room there, with a
    wheel and the game's word for skills (UI/HeroScreen3/Skills/SkillsHeader.txt
    in the button style of the game's message boxes, so in the install's own
    language);
  - the hero screen: a sixth bookmark under the five.

The pictures are made on the player's device from the game's own (a recipe's
compose, tools/ui_recipe.py): the plate is the level-up box's OK in each state,
its middle stretched to the plate's width, the wheel's emblem
(ui-mods/art/skill-wheel-emblem.png, the port's own, drawn by Leonardo for
it) at its left; the bookmark is the fifth one's frame in each state, its
picture painted over and the emblem in it. No picture of the game's is in the
recipe, only the steps.

The game knows nothing of the wheel. A button's press shows a hidden child of
the button, SkillWheelFlag (a UI message, UI_SHOW_WINDOW, what the game's own
windows send to show one by name); native/skill_wheel.c sees the flag up when
the button is drawn next, puts it down and asks for the wheel.
"""

import argparse
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import ui_recipe  # noqa: E402
from touch_ui_mod import ENTRY_DATE, paks_for, read, replace_once  # noqa: E402

LEVELUP_SHARED = "UI/LevelUPBox/LevelUpBox.(WindowSimpleShared).xdb"
BOOKMARKS_SHARED = "UI/HeroScreen3/Bookmarks/HeroSingle.(WindowSimpleShared).xdb"
DIR = "UI/SkillWheel/"
BUTTON_NAME = "SkillWheelButton"  # native/skill_wheel.c looks for these two names
FLAG_NAME = "SkillWheelFlag"
WORD = "/UI/HeroScreen3/Skills/SkillsHeader.txt"      # "Skills", plain, every language
BUTTON_FORMAT = "/Text/Interface/CommonBoxes/ButtonFormat_Text.txt"  # "<button>": the message boxes' buttons
EMBLEM = ROOT / "ui-mods/art/skill-wheel-emblem.png"
OK_DIR = "Textures/Interface/LevelUPBox/Button_OK/"
OK_STATES = {"normal": OK_DIR + "Normal.dds", "over": OK_DIR + "Highlight.dds", "pushed": OK_DIR + "Pushed.dds"}
MARK_DIR = "Textures/HeroScreen3/Bookmarks/"
MARK_STATES = {"normal": MARK_DIR + "Bookmark5_Passive.(Texture).dds",
               "over": MARK_DIR + "Bookmark5_Active.(Texture).dds",
               "pushed": MARK_DIR + "Bookmark5_Pressed.(Texture).dds"}
RECIPE_OUT = {"base": "UserMODs/SkillWheelUI.h5u", "hof": "data/zz-skill-wheel-ui.pak",
              "toe": "UserMODs/SkillWheelUI.h5u"}

# The level-up box is 526 x 630; its OK sits at 382, 533, 97 x 49. The plate
# stands at the box's left, under the left edge of its panels (the hero's at 43), at
# the OK's height, as wide as two of it.
PLATE = (43, 533, 194, 49)
CAP = 22          # the OK's rounded ends, kept as they are; its middle is stretched
EMBLEM_PLATE = 37  # the emblem's size on the plate, and on the bookmark (its picture's square, less a margin)
# The bookmarks: 87 x 73 each, 76 apart, the fifth at 0, 305.
BOOKMARK = (0, 381, 87, 73)
BOOKMARKS_HEIGHT = 381 + 73


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


def _placement(x, y, size, pos_set, size_set, indent="\t"):
    i = indent + "\t"
    return ([indent + "<Placement>"] + _pair("Position", x, y, pos_set, i)
            + _anchor("VerAllign", "EPA_LOW_END", False, i) + _anchor("HorAllign", "EPA_LOW_END", False, i)
            + _pair("Size", size[0], size[1], size_set, i) + _pair("LowerMargin", 0, 0, False, i)
            + _pair("UpperMargin", 0, 0, False, i) + [indent + "</Placement>"])


def _commands(items, indent):
    if not items:
        return [indent + "<Commands/>", indent + "<Reversable>false</Reversable>"]
    return ([indent + "<Commands>"] + [indent + '\t<Item href="%s"/>' % h for h in items]
            + [indent + "</Commands>", indent + "<Reversable>false</Reversable>"])


def button(name, shared, x, y, tooltip, on_press):
    """A WindowMSButton: one state, whose entering (on release, by the shared
    description's trigger mode) runs `on_press`."""
    t = "\t\t\t"
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<WindowMSButton>",
                  "\t<ClassTypeID>285694855</ClassTypeID>",
                  '\t<Shared href="%s#xpointer(/WindowMSButtonShared)"/>' % shared,
                  "\t<Name>%s</Name>" % name, '\t<TooltipFileRef href="%s"/>' % tooltip,
                  "\t<Visible>true</Visible>", "\t<Priority>2</Priority>", "\t<GameMessageReactions/>"]
                 + _placement(x, y, (0, 0), True, False)
                 + ["\t<Enabled>true</Enabled>", "\t<TextString/>", "\t<relatedTexts/>", "\t<relatedTextures/>",
                    "\t<Flags>", "\t\t<SuperActive>false</SuperActive>", "\t</Flags>",
                    "\t<ButtonStates>", "\t\t<Item>", "\t\t\t<MessageOnEnterState/>",
                    "\t\t\t<CommandsOnEnterState>"] + _commands([on_press], t + "\t")
                 + ["\t\t\t</CommandsOnEnterState>", "\t\t\t<CommandsOnRightClick>"] + _commands([], t + "\t")
                 + ["\t\t\t</CommandsOnRightClick>", "\t\t\t<commandsOnLDblKlick>"] + _commands([], t + "\t")
                 + ["\t\t\t</commandsOnLDblKlick>", "\t\t\t<WaitVisual>false</WaitVisual>",
                    "\t\t\t<ReverseCommands>false</ReverseCommands>", "\t\t\t<Name/>", "\t\t</Item>",
                    "\t</ButtonStates>", "\t<ButtonGroupID>0</ButtonGroupID>",
                    "\t<AutoChangeState>false</AutoChangeState>", "\t<PushEffect>"] + _commands([], "\t\t")
                 + ["\t</PushEffect>", "\t<State>0</State>", '\t<TextFileRef href=""/>', "\t<TextFormat/>",
                    "</WindowMSButton>"])


def _state(tag, background, foreground=None):
    back = '<Background href="%s"/>' % background if background else "<Background/>"
    fore = '<Foreground href="%s"/>' % foreground if foreground else "<Foreground/>"
    return (["\t\t\t<%s>" % tag, "\t\t\t\t" + back, "\t\t\t\t" + fore, "\t\t\t\t<TextString/>",
             "\t\t\t\t<OnEnterSubState>"] + _commands([], "\t\t\t\t\t")
            + ["\t\t\t\t</OnEnterSubState>", "\t\t\t\t<TextFormat/>", "\t\t\t</%s>" % tag])


def button_shared(size, children, normal, over, pushed):
    """The button's look: its size, children, and a background per state
    (hrefs); the press counts on release, as the game's buttons do."""
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<WindowMSButtonShared>", "\t<Crap>false</Crap>",
                  "\t<Children>"] + ['\t\t<Item href="%s"/>' % c for c in children]
                 + ["\t</Children>", "\t<Background/>", "\t<Foreground/>", "\t<Flags>",
                    "\t\t<Transparent>false</Transparent>", "\t</Flags>"]
                 + _placement(0, 0, size, False, True)
                 + ["\t<ActiveArea/>", "\t<IgnoreDblClick>false</IgnoreDblClick>", "\t<ActiveMask/>",
                    "\t<VisualStates>", "\t\t<Item>"]
                 + _state("Normal", normal) + _state("MouseOver", normal, over) + _state("Pushed", pushed)
                 + _state("Disabled", normal) + _state("RightButtonDown", None)
                 + ["\t\t\t<DefaultSubState>BST_NORMAL</DefaultSubState>", "\t\t\t<VisualOnEnterState>"]
                 + _commands([], "\t\t\t\t")
                 + ["\t\t\t</VisualOnEnterState>", "\t\t</Item>", "\t</VisualStates>",
                    "\t<TriggerMode>BCST_ON_RELEASE</TriggerMode>", "</WindowMSButtonShared>"])


def simple(name, shared, x, y, visible):
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<WindowSimple>",
                  "\t<ClassTypeID>285700801</ClassTypeID>",
                  '\t<Shared href="%s#xpointer(/WindowSimpleShared)"/>' % shared,
                  "\t<Name>%s</Name>" % name if name else "\t<Name/>", '\t<TooltipFileRef href=""/>',
                  "\t<Visible>%s</Visible>" % _flag(visible), "\t<Priority>0</Priority>",
                  "\t<GameMessageReactions/>"]
                 + _placement(x, y, (0, 0), True, False)
                 + ["\t<Enabled>true</Enabled>", "\t<TextString/>", "\t<relatedTexts/>", "\t<relatedTextures/>",
                    "\t<Flags>", "\t\t<SuperActive>false</SuperActive>", "\t</Flags>", "</WindowSimple>"])


def simple_shared(size, background=None, transparent=True):
    back = '\t<Background href="%s"/>' % background if background else "\t<Background/>"
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<WindowSimpleShared>", "\t<Crap>false</Crap>",
                  "\t<Children/>", back, "\t<Foreground/>", "\t<Flags>",
                  "\t\t<Transparent>%s</Transparent>" % _flag(transparent), "\t</Flags>"]
                 + _placement(0, 0, size, False, True)
                 + ["\t<ActiveArea/>", "\t<IgnoreDblClick>false</IgnoreDblClick>", "\t<ActiveMask/>",
                    "</WindowSimpleShared>"])


def static_text(shared, x, y, text_string):
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<WindowStaticText>",
                  "\t<ClassTypeID>554818304</ClassTypeID>",
                  '\t<Shared href="%s#xpointer(/WindowStaticTextShared)"/>' % shared,
                  "\t<Name/>", '\t<TooltipFileRef href=""/>', "\t<Visible>true</Visible>",
                  "\t<Priority>0</Priority>", "\t<GameMessageReactions/>"]
                 + _placement(x, y, (0, 0), True, False)
                 + ["\t<Enabled>true</Enabled>",
                    '\t<TextString href="%s#xpointer(/ForegroundTextString)"/>' % text_string,
                    "\t<relatedTexts/>", "\t<relatedTextures/>", "\t<Flags>",
                    "\t\t<SuperActive>false</SuperActive>", "\t</Flags>", "</WindowStaticText>"])


def static_text_shared(size):
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<WindowStaticTextShared>", "\t<Crap>false</Crap>",
                  "\t<Children/>", "\t<Background/>", "\t<Foreground/>", "\t<Flags>",
                  "\t\t<Transparent>true</Transparent>", "\t</Flags>"]
                 + _placement(0, 0, size, False, True)
                 + ["\t<ActiveArea/>", "\t<IgnoreDblClick>false</IgnoreDblClick>", "\t<ActiveMask/>",
                    "</WindowStaticTextShared>"])


def text_string(style, text):
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<ForegroundTextString>",
                  "\t<ClassTypeID>285694787</ClassTypeID>", '\t<Shared href="%s"/>' % style,
                  '\t<TextStringFileRef href="%s"/>' % text, "</ForegroundTextString>"])


def text_style():
    """The message boxes' button style ("<button>", the OK's yellow), the
    word centred in its window."""
    i = "\t\t"
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<ForegroundTextStringShared>",
                  '\t<TextStringFileRef href=""/>', "\t<Position>"]
                 + _pair("Position", 0, 0, False, i) + _anchor("VerAllign", "ERA_CENTER", True, i)
                 + _anchor("HorAllign", "ERA_CENTER", True, i) + _pair("Size", 0, 0, False, i)
                 + _pair("LowerMargin", 0, 0, False, i) + _pair("UpperMargin", 0, 0, False, i)
                 + ["\t</Position>", '\t<FormatStringFileRef href="%s"/>' % BUTTON_FORMAT,
                    "</ForegroundTextStringShared>"])


def texture(dds, size):
    """A picture's description: the port's own, its size, no mips."""
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<Texture>", '\t<SrcName href=""/>',
                  '\t<DestName href="%s"/>' % dds, "\t<Type>TEXTURE_2D</Type>",
                  "\t<ConversionType>CONVERT_TRANSPARENT</ConversionType>", "\t<AddrType>CLAMP</AddrType>",
                  "\t<Format>TF_8888</Format>", "\t<Width>%d</Width>" % size[0],
                  "\t<Height>%d</Height>" % size[1], "\t<MappingSize>0</MappingSize>", "\t<NMips>1</NMips>",
                  "\t<Gain>0</Gain>", "\t<AverageColor>0</AverageColor>", "\t<InstantLoad>false</InstantLoad>",
                  "\t<IsDXT>false</IsDXT>", "\t<FlipY>false</FlipY>", "\t<StandardExport>true</StandardExport>",
                  "\t<UseS3TC>false</UseS3TC>", "</Texture>"])


def show_flag():
    """UI_SHOW_WINDOW SkillWheelFlag: shows the window of that name."""
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<UISSendUIMessage>",
                  "\t<ClassTypeID>285694982</ClassTypeID>", "\t<SUIStateBaseFakeField>0</SUIStateBaseFakeField>",
                  "\t<szMessageID>", "\t\t<First>UI_SHOW_WINDOW</First>", "\t\t<Second>true</Second>",
                  "\t</szMessageID>", "\t<szParam>", "\t\t<First>%s</First>" % FLAG_NAME,
                  "\t\t<Second>true</Second>", "\t</szParam>", "\t<nForwardParam>", "\t\t<First>1</First>",
                  "\t\t<Second>true</Second>", "\t</nForwardParam>", "\t<nBackParam>", "\t\t<First>0</First>",
                  "\t\t<Second>true</Second>", "\t</nBackParam>", "</UISSendUIMessage>"])


def background(texture):
    return _crlf(['<?xml version="1.0" encoding="UTF-8"?>', "<BackgroundSimpleTexture>",
                  "\t<ClassTypeID>285694785</ClassTypeID>", '\t<Texture href="%s#xpointer(/Texture)"/>' % texture,
                  "\t<Color>-1</Color>", "\t<TextureX>EPA_LOW_END</TextureX>", "\t<TextureY>EPA_LOW_END</TextureY>",
                  "</BackgroundSimpleTexture>"])


def xp(path, tag):
    return "%s#xpointer(/%s)" % (path, tag)


def build(files, plate_textures, bookmark_textures):
    """name -> bytes of the windows. *_textures: {state: texture xdb path}
    (normal, over, pushed)."""
    out = {}
    flag = DIR + "Flag.(WindowSimple).xdb"
    out[flag] = simple(FLAG_NAME, "Flag.(WindowSimpleShared).xdb", 0, 0, False)
    out[DIR + "Flag.(WindowSimpleShared).xdb"] = simple_shared((1, 1))
    out[DIR + "Open.(UISSendUIMessage).xdb"] = show_flag()
    press = "/" + DIR + "Open.(UISSendUIMessage).xdb#xpointer(/UISSendUIMessage)"
    flag_ref = "/" + xp(flag, "WindowSimple")

    backs = {}
    for kind, textures in (("Plate", plate_textures), ("Bookmark", bookmark_textures)):
        for state, tex in textures.items():
            name = DIR + "%s%s.(BackgroundSimpleTexture).xdb" % (kind, state.capitalize())
            out[name] = background(tex)
            backs[kind, state] = "/" + xp(name, "BackgroundSimpleTexture")

    # The level-up box's plate: the wheel's word in the button style.
    x, y, w, h = PLATE
    out[DIR + "PlateWord.(ForegroundTextStringShared).xdb"] = text_style()
    out[DIR + "PlateWord.(ForegroundTextString).xdb"] = text_string(
        "/" + xp(DIR + "PlateWord.(ForegroundTextStringShared).xdb", "ForegroundTextStringShared"), WORD)
    left = 8 + EMBLEM_PLATE  # the word in the room right of the emblem
    # A line's height at the OK's own word's place (16 down, 22 high).
    out[DIR + "PlateWord.(WindowStaticTextShared).xdb"] = static_text_shared((w - left - CAP // 2, 22))
    out[DIR + "PlateWord.(WindowStaticText).xdb"] = static_text(
        "PlateWord.(WindowStaticTextShared).xdb", left, 16, "/" + DIR + "PlateWord.(ForegroundTextString).xdb")
    out[DIR + "Plate.(WindowMSButtonShared).xdb"] = button_shared(
        (w, h), ["/" + xp(DIR + "PlateWord.(WindowStaticText).xdb", "WindowStaticText"), flag_ref],
        backs["Plate", "normal"], backs["Plate", "over"], backs["Plate", "pushed"])
    out[DIR + "Plate.(WindowMSButton).xdb"] = button(BUTTON_NAME, "Plate.(WindowMSButtonShared).xdb", x, y, WORD,
                                                     press)
    plate_ref = '\t\t<Item href="/%s"/>\r\n' % xp(DIR + "Plate.(WindowMSButton).xdb", "WindowMSButton")
    box = files[LEVELUP_SHARED]
    ok = b'\t\t<Item href="ButtonOk.(WindowMSButton).xdb#xpointer(/WindowMSButton)"/>\r\n'
    out[LEVELUP_SHARED] = replace_once(box, ok, ok + plate_ref.encode())

    # The hero screen's sixth bookmark.
    x, y, w, h = BOOKMARK
    out[DIR + "Bookmark.(WindowMSButtonShared).xdb"] = button_shared(
        (w, h), [flag_ref], backs["Bookmark", "normal"], backs["Bookmark", "over"], backs["Bookmark", "pushed"])
    out[DIR + "Bookmark.(WindowMSButton).xdb"] = button(BUTTON_NAME, "Bookmark.(WindowMSButtonShared).xdb", x, y,
                                                        WORD, press)
    marks = files[BOOKMARKS_SHARED]
    fifth = b'\t\t<Item href="/UI/HeroScreen3/Bookmark5.(WindowMSButton).xdb#xpointer(/WindowMSButton)"/>\r\n'
    mark_ref = '\t\t<Item href="/%s"/>\r\n' % xp(DIR + "Bookmark.(WindowMSButton).xdb", "WindowMSButton")
    marks = replace_once(marks, fifth, fifth + mark_ref.encode())
    marks = replace_once(marks, b"<x>87</x>\r\n\t\t\t\t<y>378</y>", b"<x>87</x>\r\n\t\t\t\t<y>%d</y>" % BOOKMARKS_HEIGHT)
    out[BOOKMARKS_SHARED] = marks
    return out


def emblem(size):
    """The emblem as a DDS of size x size, scaled smooth (its colours weighted
    by their cover, so its edge fades to its own dark outline)."""
    from PIL import Image
    im = Image.open(EMBLEM).convert("RGBA")
    im = im.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")
    rgba = bytearray(im.tobytes())
    rgba[0::4], rgba[2::4] = rgba[2::4], rgba[0::4]
    return ui_recipe.dds_bytes(size, size, rgba)


# The bookmark's picture in its frame, the dark square (x, y, w, h) per state:
# the hover's frame is drawn larger. Read off the base game's pictures.
INNER = {"normal": (9, 13, 45, 45), "over": (11, 12, 47, 47), "pushed": (11, 14, 44, 43)}


def art(sources):
    """{name: (width, height, [(source, bytes)], steps)} of the composed
    pictures, the emblems' DDS ({name: bytes}), and the textures' xdbs."""
    composed, drawn, xdbs = {}, {}, {}
    x, y, w, h = PLATE
    plate_pic = "Textures/Interface/SkillWheel/Emblem%d.dds" % EMBLEM_PLATE
    drawn[plate_pic] = emblem(EMBLEM_PLATE)
    for state, src in OK_STATES.items():
        sw, sh, _ = ui_recipe.dds_pixels(sources[src])
        steps = [("blit", 0, 0, 0, CAP, sh, 0, 0, CAP, h),
                 ("blit", 0, CAP, 0, sw - 2 * CAP, sh, CAP, 0, w - 2 * CAP, h),
                 ("blit", 0, sw - CAP, 0, CAP, sh, w - CAP, 0, CAP, h)]
        if state != "over":  # the hover's picture lies over the plate's
            dy = (h - EMBLEM_PLATE) // 2 + (1 if state == "pushed" else 0)
            steps.append(("lay", plate_pic, 10, dy))
        name = "Textures/Interface/SkillWheel/Plate_%s.dds" % state
        composed[name] = (w, h, [(src, sources[src])], steps)
        xdbs["plate", state] = name[:-4] + ".xdb"
    for state, src in MARK_STATES.items():
        sw, sh, _ = ui_recipe.dds_pixels(sources[src])
        ix, iy, iw, ih = INNER[state]
        size = min(iw, ih) - 2
        pic = "Textures/Interface/SkillWheel/Emblem%d.dds" % size
        drawn.setdefault(pic, emblem(size))
        steps = [("blit", 0, 0, 0, sw, sh, 0, 0, sw, sh), ("fill", ix, iy, iw, ih, "3A2A1AFF"),
                 ("lay", pic, ix + (iw - size) // 2, iy + (ih - size) // 2)]
        name = "Textures/Interface/SkillWheel/Bookmark_%s.dds" % state
        composed[name] = (sw, sh, [(src, sources[src])], steps)
        xdbs["bookmark", state] = name[:-4] + ".xdb"
    return composed, drawn, xdbs


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--base-game", type=Path, required=True,
                   help="the game's install folder (the base game's for hof, Tribes of the East's for toe)")
    p.add_argument("--game", choices=("base", "hof", "toe"), default="base")
    p.add_argument("--out", type=Path, required=True, help="the .h5u (or for hof the .pak) to write")
    p.add_argument("--recipe", type=Path, help="also write the recipe (tools/ui_recipe.py) the app makes it from")
    args = p.parse_args()
    paks = [args.base_game / n for n in paks_for(args.game)]
    names = [LEVELUP_SHARED, BOOKMARKS_SHARED] + list(OK_STATES.values()) + list(MARK_STATES.values())
    files = read(paks, names)
    missing = [n for n in names if n not in files]
    if missing:
        sys.exit("Not in the game's data: " + ", ".join(missing))
    composed, drawn, xdbs = art(files)
    pictures = dict(drawn)
    out = build(files, {s: "/" + xdbs["plate", s] for s in OK_STATES},
                {s: "/" + xdbs["bookmark", s] for s in MARK_STATES})
    for (kind, state), xdb in xdbs.items():
        name = composed[xdb[:-4] + ".dds"]
        out[xdb] = texture(Path(xdb).name[:-4] + ".dds", (name[0], name[1]))
    for name, (w, h, srcs, steps) in composed.items():
        out[name] = ui_recipe.compose(w, h, [b for _, b in srcs], steps, pictures)
    out.update(drawn)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(out):
            info = zipfile.ZipInfo(name, date_time=ENTRY_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, out[name])
    if args.recipe:
        sources = {n: (n, files[n]) for n in (LEVELUP_SHARED, BOOKMARKS_SHARED)}
        authored = [n for n in out if n not in sources and n not in composed and n not in drawn]
        ui_recipe.write(args.recipe, name="SkillWheelUI" + ("" if args.game == "base" else "-" + args.game),
                        out=RECIPE_OUT[args.game], date="%04d-%02d-%02d %02d:%02d:%02d" % ENTRY_DATE,
                        layers=paks_for(args.game), files=out, sources=sources, authored=authored,
                        drawn=tuple(drawn), composed=composed,
                        comment="Made by tools/skill_wheel_ui_mod.py --game %s; applied by kit/host/ui_mods.cpp."
                                % args.game)
    print("%s: %d files" % (args.out, len(out)))


if __name__ == "__main__":
    main()

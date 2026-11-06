"""tools/extract_icon.py rebuilds the executable's icon group and writes the iPad icon set."""

import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("extract_icon", ROOT / "tools/extract_icon.py")
extract_icon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract_icon)

EXE = ROOT / "original/gog/D3DPopTB.exe"


class SidecarIconTests(unittest.TestCase):
    def test_missing_resource_uses_case_insensitive_sibling_icon(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exe = root / "Sample.exe"
            Image.new("RGBA", (32, 32), (20, 40, 60, 255)).save(root / "sample.ICO")
            with patch.object(extract_icon, "ico_bytes", side_effect=extract_icon.MissingIconError("missing")):
                paths = extract_icon.write_icons(exe, root / "icons")
            self.assertEqual(len(paths), len(extract_icon.IOS_ICONS))
            with Image.open(paths[0]) as image:
                self.assertEqual(image.getpixel((0, 0)), (20, 40, 60))

    def test_corrupt_executable_is_not_hidden_by_sidecar_fallback(self):
        with patch.object(extract_icon, "ico_bytes", side_effect=ValueError("corrupt")):
            with self.assertRaisesRegex(ValueError, "corrupt"):
                extract_icon.write_icons(Path("invalid.exe"), Path("unused"))


class ArtworkIconTests(unittest.TestCase):
    def test_artwork_replaces_executable_icon_on_ipad(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            Image.new("RGB", (300, 200), (200, 10, 10)).save(root / "icon.png")
            paths = extract_icon.write_icons(Path("unused.exe"), root / "icons", image=root / "icon.png")
            for path, (_, px) in zip(paths, extract_icon.IOS_ICONS):
                with Image.open(path) as image:
                    self.assertEqual((image.size, image.mode), ((px, px), "RGB"))
                    self.assertEqual(image.getpixel((px // 2, px // 2)), (200, 10, 10))

    def test_mac_icon_sits_on_the_grid_with_transparent_margin(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            Image.new("RGB", (64, 64), (10, 200, 10)).save(Path(tmp) / "icon.png")
            icon = extract_icon.mac_icon(Path(tmp) / "icon.png")
            self.assertEqual(icon.size, (1024, 1024))
            self.assertEqual(icon.getpixel((4, 4))[3], 0)
            self.assertEqual(icon.getpixel((512, 512)), (10, 200, 10, 255))

    @unittest.skipUnless(shutil.which("iconutil"), "needs macOS iconutil")
    def test_writes_icns(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            Image.new("RGB", (64, 64), (10, 10, 200)).save(Path(tmp) / "icon.png")
            icns = extract_icon.write_icns(Path(tmp) / "icon.png", Path(tmp) / "App.icns")
            self.assertEqual(icns.read_bytes()[:4], b"icns")

    def test_writes_ico_with_every_windows_size(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            Image.new("RGB", (300, 200), (200, 10, 10)).save(Path(tmp) / "icon.png")
            ico = extract_icon.write_ico(Path(tmp) / "icon.png", Path(tmp) / "out" / "app.ico")
            self.assertEqual(ico.read_bytes()[:4], b"\0\0\1\0")
            with Image.open(ico) as image:
                self.assertEqual(sorted(image.info["sizes"]), [(px, px) for px in extract_icon.WINDOWS_ICONS])


@unittest.skipUnless(EXE.is_file(), "needs the developer's game installation")
class IconTests(unittest.TestCase):
    def test_ico_has_three_frames_and_largest_is_48(self):
        ico = extract_icon.ico_bytes(EXE)
        self.assertEqual(ico[:6], b"\x00\x00\x01\x00\x03\x00")
        frame = extract_icon.largest_frame(ico)
        self.assertEqual(frame.size, (48, 48))

    def test_writes_every_ios_size(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            paths = extract_icon.write_icons(EXE, tmp)
            self.assertEqual([p.name for p in paths],
                             ["Icon-29@2x.png", "Icon-40@2x.png", "Icon-60@2x.png", "Icon-76@2x.png", "Icon-83.5@2x.png"])
            for path, (_, px) in zip(paths, extract_icon.IOS_ICONS):
                with Image.open(path) as image:
                    self.assertEqual(image.size, (px, px))
                    self.assertEqual(image.mode, "RGB")


if __name__ == "__main__":
    unittest.main()

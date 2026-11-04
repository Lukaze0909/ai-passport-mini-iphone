"""Classic shell asset checks: every non-ASCII character in a main/classic
string literal exists in the fonts that draw text (cs_font12 / cs_font14, and
cs_font18 or its fallback), the 18 px font really falls back to 14 px, and the
generated image table has every image the UI references (with a negative
control so the glyph check cannot pass unconditionally)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/classic"))
import gen_fonts  # noqa: E402

FONT_DIR = ROOT / "assets/fonts/classic"
GLYPH_RE = re.compile(r"/\* U\+([0-9A-F]{4,6}) ")


def codepoints(name):
    text = (FONT_DIR / f"{name}.c").read_text(encoding="utf-8")
    return {int(m.group(1), 16) for m in GLYPH_RE.finditer(text)}, text


class ClassicAssets(unittest.TestCase):
    def test_literals_covered(self):
        chars = gen_fonts.source_chars()
        self.assertIn("钟", chars)
        for name in ("cs_font12", "cs_font14"):
            present, text = codepoints(name)
            self.assertIn("Bpp: 4", text)
            self.assertEqual(sorted(c for c in chars if ord(c) not in present), [], name)
        p18, t18 = codepoints("cs_font18")
        self.assertIn(".fallback = &cs_font14,", t18)
        self.assertEqual(sorted(c for c in chars if ord(c) not in p18), [])

    def test_dynamic_text_coverage(self):
        present, _ = codepoints("cs_font14")
        for word in ("上海北京多云阴小雨雷阵雨霾雾雪优良轻度中重严污染周日一二三四五六晚安有声静音"):
            self.assertIn(ord(word), present, word)
        self.assertGreater(len(present), 6800)

    def test_negative_control(self):
        present, _ = codepoints("cs_font14")
        self.assertNotIn(0x9F98, present)          # 龘: outside GB2312 and the UI
        nums, _ = codepoints("cs_num52")
        self.assertEqual(nums, {ord(c) for c in "0123456789:"})

    def test_images(self):
        h = (ROOT / "main/classic/cs_assets.h").read_text(encoding="utf-8")
        ui = (ROOT / "main/classic/cs_ui.c").read_text(encoding="utf-8")
        used = set(re.findall(r"cs_img_(\w+)", ui))
        declared = set(re.findall(r"extern const lv_image_dsc_t cs_img_(\w+);", h))
        self.assertTrue(used, "cs_ui.c references no images?")
        self.assertEqual(sorted(used - declared), [])
        for name in ("lock", "home", "settings", "pomo_pick", "pomo_run", "pomo_done", "weather", "call"):
            self.assertIn(f"cs_img_bg_{name};  // 240x320 RGB565\n", h)


if __name__ == "__main__":
    unittest.main()

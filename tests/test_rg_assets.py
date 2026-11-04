"""Release Guidance asset checks: generated copy table is current and every
glyph of the fixed copy exists in the font slot that draws it (with a
negative control so the check cannot pass unconditionally)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/release_guidance"))
import gen_content  # noqa: E402
import rg_spec  # noqa: E402

FONT_DIR = ROOT / "assets/fonts/release_guidance"
GLYPH_RE = re.compile(r"/\* U\+([0-9A-F]{4,6}) ")


def font_codepoints(name):
    text = (FONT_DIR / f"rg_wenkai_{name}.c").read_text(encoding="utf-8")
    return {int(m.group(1), 16) for m in GLYPH_RE.finditer(text)}, text


def missing(required, present):
    return sorted(c for c in required if ord(c) not in present)


class ReleaseGuidanceAssets(unittest.TestCase):
    def test_generated_content_is_current(self):
        h, c, charsets = gen_content.render()
        self.assertEqual((ROOT / "main/rg/rg_content.h").read_text(encoding="utf-8"), h)
        self.assertEqual((ROOT / "main/rg/rg_content.c").read_text(encoding="utf-8"), c)
        for name, chars in charsets.items():
            inv = (FONT_DIR / f"rg_wenkai_{name}.chars").read_text(encoding="utf-8")
            self.assertEqual(inv, chars + "\n", name)

    def test_every_glyph_present_in_its_font(self):
        _, _, charsets = gen_content.render()
        for name, px in rg_spec.FONTS:
            present, text = font_codepoints(name)
            self.assertIn(f"Size: {px} px", text, name)
            self.assertIn("Bpp: 4", text, name)
            self.assertIn(f"const lv_font_t rg_wenkai_{name} = {{", text, name)
            self.assertEqual(missing(charsets[name], present), [], name)

    def test_negative_control(self):
        present, _ = font_codepoints("q25")
        # U+9F98 exists in the source TTF but is outside every subset.
        self.assertNotIn(0x9F98, present)
        self.assertEqual(missing("能龘", present), ["龘"])

    def test_copy_has_no_orphan_question_particle(self):
        for sid, _, _, _, source, lines in rg_spec.SENTENCES:
            self.assertEqual("".join(t for line in lines for t, _ in line), source, sid)
            for line in lines:
                self.assertNotEqual("".join(t for t, _ in line), "吗？", sid)


if __name__ == "__main__":
    unittest.main()

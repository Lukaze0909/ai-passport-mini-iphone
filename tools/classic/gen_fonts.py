#!/usr/bin/env python3
"""Generate the classic shell's LVGL fonts from Noto Sans CJK SC.

Usage: python3 tools/classic/gen_fonts.py <work_dir>
Needs Node.js (npx, lv_font_conv 1.5.3) and the Noto Sans CJK TTCs at
NOTO_DIR (default /usr/share/fonts/opentype/noto). Faces are pulled out of the
TTCs with ttc_extract.py (index 2 = SC).

Slots (4 bpp, uncompressed):
  cs_font12 / cs_font14  Bold, ASCII + all GB2312 hanzi + every non-ASCII char in
                         main/classic sources + EXTRA: covers dynamic text
                         (weather, captions) as well as the UI.
  cs_font18              Bold, ASCII + UI chars + EXTRA; falls back to cs_font14.
  cs_num40 / cs_num52    Regular, digits and clock punctuation.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import ttc_extract  # noqa: E402

OUT = ROOT / "assets/fonts/classic"
NOTO = Path(os.environ.get("NOTO_DIR", "/usr/share/fonts/opentype/noto"))
CONV = ["npx", "--yes", "lv_font_conv@1.5.3"]

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
SYMBOLS = "·…—–°✓×、，。？！：；“”‘’（）《》【】～％＋－"
# Weather words, AQI levels and city names that may arrive over MQTT.
EXTRA = SYMBOLS + "霾沙尘冰雹雾凇霰阵雷暴晴阴转渐" + "优良轻度中重严污染" + "沪苏杭甬穗鹏蓉渝"
NUMS40 = "0123456789:-° "
NUMS52 = "0123456789:"

SLOTS = [  # name, face, px, charset kind
    ("cs_font12", "Bold", 12, "full"),
    ("cs_font14", "Bold", 14, "full"),
    ("cs_font18", "Bold", 18, "ui"),
    ("cs_num40", "Regular", 40, NUMS40),
    ("cs_num52", "Regular", 52, NUMS52),
]
FALLBACK = {"cs_font18": "cs_font14"}


def gb2312_hanzi():
    """GB2312 levels 1 and 2 (6763 hanzi)."""
    out = []
    for a in range(0xB0, 0xF8):
        for b in range(0xA1, 0xFF):
            try:
                out.append(bytes([a, b]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return "".join(out)


STRING_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def source_chars():
    chars = set()
    for p in sorted((ROOT / "main/classic").glob("*.[ch]")):
        if p.name.startswith("cs_assets"):
            continue
        for lit in STRING_RE.findall(p.read_text(encoding="utf-8")):
            chars.update(c for c in lit if ord(c) > 0x7E)
    return chars


def charsets():
    ui = source_chars() | set(EXTRA)
    full = set(ASCII) | set(gb2312_hanzi()) | ui
    small = set(ASCII) | ui
    return {"full": "".join(sorted(full)), "ui": "".join(sorted(small))}


def main():
    work = Path(sys.argv[1])
    work.mkdir(parents=True, exist_ok=True)
    faces = {}
    for face in ("Bold", "Regular"):
        dst = work / f"NotoSansSC-{face}.otf"
        if not dst.exists():
            dst.write_bytes(ttc_extract.extract((NOTO / f"NotoSansCJK-{face}.ttc").read_bytes(), 2))
        faces[face] = dst
    sets = charsets()
    OUT.mkdir(parents=True, exist_ok=True)
    for name, face, px, kind in SLOTS:
        chars = sets.get(kind, kind)
        (OUT / f"{name}.chars").write_text(chars + "\n", encoding="utf-8")
        sym = work / f"{name}.sym"
        sym.write_text(chars, encoding="utf-8")
        dst = OUT / f"{name}.c"
        subprocess.run(CONV + ["--font", str(faces[face]), "--symbols", chars, "--size", str(px),
                               "--bpp", "4", "--format", "lvgl", "--no-compress",
                               "--lv-font-name", name, "--lv-include", "lvgl.h", "-o", str(dst)],
                       check=True)
        text = dst.read_text(encoding="utf-8")
        if name in FALLBACK:
            fb = FALLBACK[name]
            text = text.replace(".fallback = NULL,", f".fallback = &{fb},")
            text = text.replace(f"const lv_font_t {name} = {{",
                                f"extern const lv_font_t {fb};\nconst lv_font_t {name} = {{", 1)
            if f".fallback = &{fb}," not in text:
                raise SystemExit(f"{name}: could not set fallback")
        # Remove the machine-specific absolute paths lv_font_conv records.
        text = re.sub(r"(--font )\S+", r"\1NotoSansSC-" + face + ".otf", text)
        text = re.sub(r"( -o )\S+", r"\1" + f"assets/fonts/classic/{name}.c", text)
        dst.write_text(text, encoding="utf-8")
        print(f"{name}: {len(chars)} chars, {dst.stat().st_size} bytes of C")


if __name__ == "__main__":
    main()

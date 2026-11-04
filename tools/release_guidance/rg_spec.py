"""Single source of truth for the Release Guidance app copy, layout and theme.

gen_content.py turns this module into main/rg/rg_content.{h,c} and per-font
character inventories; preview.py renders host previews from the same data.
Copy text follows the release-guidance work order section 4 character for
character; only line breaks and the emphasised word are chosen here.
"""

SCREEN_W = 240
SCREEN_H = 320

# Font slots: (symbol suffix, pixel size). One LXGW WenKai subset per slot.
FONTS = [
    ("note19", 19),     # explanation sentence (loop B #2)
    ("opt21", 21),      # options
    ("long22", 22),     # longest question
    ("q25", 25),        # questions; also emphasis inside the 22 px question
    ("entry27", 27),    # entry main sentence
    ("key28", 28),      # emphasised word inside 25 px questions
    ("key30", 30),      # emphasised word inside the entry sentence
    ("small13", 13),    # transient status flash (mute / theme)
]
FONT_PX = dict(FONTS)

E = True  # marks an emphasised segment


def L(*parts):
    """A line: plain strings or (text, E) tuples."""
    out = []
    for p in parts:
        out.append((p, False) if isinstance(p, str) else (p[0], True))
    return out


# kind: question | willing | note | entry
# Each sentence: id, kind, base font, emphasis font, exact source text, lines.
SENTENCES = [
    ("ENTRY", "entry", "entry27", "key30", "仅仅通过释放获得一切。",
     [L("仅仅通过", ("释放", E)), L("获得一切。")]),
    ("FEEL_1", "question", "q25", "key28", "现在感受一下，此时此刻是哪种想要浮上心头？",
     [L("现在感受一下，"), L("此时此刻"), L("是哪种", ("想要", E)), L("浮上心头？")]),
    ("ALLOW", "question", "q25", "key28", "能允许它存在吗？",
     [L("能", ("允许", E)), L("它存在吗？")]),
    ("LETGO", "question", "q25", "key28", "能允许自己轻松放开这个紧抓不放的想要吗？",
     [L("能允许自己"), L("轻松", ("放开", E)), L("这个紧抓不放的"), L("想要吗？")]),
    ("WILLING", "willing", "q25", "key28", "愿意现在放开它吗？",
     [L("愿意"), L("现在", ("放开", E), "它吗？")]),
    ("GOAL_1", "question", "long22", "q25", "心里想着你的目标，感受一下这个目标勾起了什么想要？",
     [L("心里想着你的", ("目标", E), "，"), L("感受一下"), L("这个目标"), L("勾起了什么想要？")]),
    ("GOAL_5", "question", "q25", "key28", "如果目标实现不了，会勾起什么想要？",
     [L("如果目标"), L(("实现不了", E), "，"), L("会勾起什么想要？")]),
    ("LOOP_A_1", "question", "q25", "key28", "现在还有什么想要？",
     [L("现在还有什么"), L(("想要", E), "？")]),
    ("LOOP_B_1", "question", "q25", "key28", "能允许感受浮现上来吗？",
     [L("能允许感受"), L(("浮现", E), "上来吗？")]),
    ("LOOP_B_2", "note", "note19", "note19", "没有感受说明感受被压抑了，只是允许感受浮现出来。",
     [L("没有感受"), L("说明感受被压抑了，"), L("只是允许感受"), L("浮现出来。")]),
    ("LOOP_B_3", "question", "q25", "key28", "现在是哪种想要？",
     [L("现在是"), L("哪种", ("想要", E), "？")]),
]

SEQUENCES = {
    "FEEL": ["FEEL_1", "ALLOW", "LETGO", "WILLING"],
    "GOAL": ["GOAL_1", "ALLOW", "LETGO", "WILLING", "GOAL_5", "ALLOW", "LETGO", "WILLING"],
    "LOOP_A": ["LOOP_A_1", "ALLOW", "LETGO", "WILLING"],
    "LOOP_B": ["LOOP_B_1", "LOOP_B_2", "LOOP_B_3", "ALLOW", "LETGO", "WILLING"],
}

ENTRY_OPTIONS = ["释放感受", "释放目标"]
WILLING_OPTIONS = ["愿意", "还不愿意"]
OPTION_FONT = "opt21"
# Short status flashes for the entry-page long-hold settings (not work-order copy).
STATUS_WORDS = ["静音", "有声", "自动", "暮色", "晨空"]
STATUS_FONT = "small13"

# ---- layout (pixels, 240 x 320 portrait) ----
QUESTION_CENTER_Y = 170
QUESTION_BASELINE_OFFSET = 10
LINE_PITCH = {"q25": 40, "long22": 36, "note19": 32}
ENTRY_BASELINES = [132, 174]
ENTRY_OPTION_BASELINES = [232, 266]
WILLING_BASELINES = [138, 178]
WILLING_OPTION_BASELINES = [234, 268]
OPTION_DOT_GAP = 14           # dot centre is this far left of the option text
OPTION_DOT_RADIUS = 3         # 6 px dot
OPTION_DOT_RISE_PCT = 45      # dot centre sits this % of the ascent above the baseline
UNDERLINE_DROP = 5            # underline starts 5 px under the baseline
UNDERLINE_SLANT = 1           # and ends 1 px lower (hand-drawn tilt)
UNDERLINE_INSET = 2
UNDERLINE_WIDTH = 2
UNDERLINE_OPA = 110
NOTE_OPA = 158                # ~62 % for the explanation sentence
STATUS_BASELINE = 300

# Right-edge button hints. The three keys sit vertically on the right side of the
# case (top UP, middle DOWN, bottom OK). The Y values below are PLACEHOLDERS until
# they are measured against the physical keys.
HINT_X = 231
HINT_Y_UP = 132
HINT_Y_DOWN = 166
HINT_Y_OK = 200

# ---- themes (RGB hex) ----
THEMES = {
    "dusk": dict(top=0x2E3A58, bottom=0x1A1A22, text=0xF2EDE6, dim=0x9696A4,
                 faint=0x5A5C6C, accent=0xF0A66C, cloud=0x606882),
    "morning": dict(top=0xC4DAEA, bottom=0xF7F1E8, text=0x343842, dim=0x828894,
                    faint=0xBABEC4, accent=0xD07640, cloud=0xFAFBFC),
    "rain": dict(top=0xAABAC8, bottom=0xECEAE6, text=0x343842, dim=0x828894,
                 faint=0xBABEC4, accent=0xD07640, cloud=0xFAFBFC),
}

# Clouds: (centre x, centre y, scale). Drawn x = ((cx + drift) mod (240 + 80)) - 40.
CLOUDS = [(60, 52, 0.85), (175, 34, 0.6), (255, 66, 0.5)]
CLOUD_WRAP = SCREEN_W + 80
# Puff circles relative to the cloud centre, in units of `scale` px: (dx, dy, r).
CLOUD_PUFFS = [(-14, 2, 9), (-4, -4, 12), (8, -1, 10), (17, 3, 7), (0, 5, 9)]
CLOUD_BLUR = 2.2

MOON = dict(x=188, y=30, r=11, glow=24, color=0xF4E9CC)
SUN = dict(x=186, y=40, r=11, ray_in=16, ray_out=22, rays=12, glow=38,
           color=0xFFD27A, ray_color=0xFFC864)
RAINBOW = dict(r=58, band=2, y=112, x_right=172, x_left=68, opa=70,
               colors=[0xE88A8A, 0xF0B070, 0xF0E08A, 0x9CD49A, 0x8AB8E8, 0xB49AE0])

# Twinkling stars: (x, y, size_px x10, phase_deg). Fixed positions in the sky band.
STARS = [
    (22, 24, 10, 0), (54, 38, 7, 40), (80, 18, 12, 95), (112, 46, 8, 150),
    (140, 22, 14, 210), (160, 60, 9, 260), (214, 18, 7, 300), (226, 54, 11, 330),
    (12, 74, 8, 20), (40, 96, 13, 70), (72, 70, 7, 120), (96, 100, 10, 175),
    (126, 84, 8, 230), (150, 108, 12, 285), (178, 92, 7, 345), (206, 80, 9, 15),
    (230, 112, 10, 60), (24, 128, 7, 110), (118, 132, 9, 200), (200, 134, 8, 250),
]

FX_PERIOD_MS = 80
RAIN_DROPS = 16
LEAVES = 4

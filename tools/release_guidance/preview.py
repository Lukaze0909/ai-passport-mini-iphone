#!/usr/bin/env python3
"""Render host previews of the Release Guidance screens from the implementation
parameters (rg_spec.py + the sprites gen_assets.py compiles into the firmware).

Usage: python3 tools/release_guidance/preview.py <font.ttf> <out_dir>
Requires Pillow and NumPy. Text is placed exactly like rg_ui.c: segments are
centred per line by advance width and anchored on the generated baseline.
This approximates LVGL's 4 bpp rasteriser; it is not a device screenshot.
"""
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen_assets as A  # noqa: E402
import rg_spec as S  # noqa: E402

W, H = S.SCREEN_W, S.SCREEN_H
SENT = {s[0]: s for s in S.SENTENCES}


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


class Canvas:
    def __init__(self, pal):
        self.px = np.repeat(A.gradient_rgb(pal)[:, None, :], W, axis=1)

    def blend(self, x, y, rgb_arr, alpha):
        """alpha in 0..1, rgb_arr HxWx3 (or a colour tuple)."""
        h, w = alpha.shape
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(W, x + w), min(H, y + h)
        if x0 >= x1 or y0 >= y1:
            return
        a = alpha[y0 - y:y1 - y, x0 - x:x1 - x][..., None]
        src = rgb_arr if isinstance(rgb_arr, tuple) else rgb_arr[y0 - y:y1 - y, x0 - x:x1 - x]
        src = np.array(src, float)
        self.px[y0:y1, x0:x1] = self.px[y0:y1, x0:x1] * (1 - a) + src * a

    def image(self):
        return Image.fromarray(np.clip(self.px, 0, 255).astype(np.uint8), "RGB")


def draw_sprite(cv, name, cx, cy, color=None, rotation=0.0, sprites=None):
    kind, data, pivot = sprites[name]
    if kind == "a8":
        a = data
        if rotation:
            im = Image.fromarray((a * 255).astype(np.uint8), "L").rotate(-rotation, Image.BILINEAR)
            a = np.asarray(im).astype(float) / 255
        cv.blend(cx - pivot[0], cy - pivot[1], rgb(color), a)
    else:
        cv.blend(cx - pivot[0], cy - pivot[1], data[..., :3].astype(float),
                 data[..., 3].astype(float) / 255)


def text_layer(cv, draw_ops):
    """draw_ops: list of (fn(ImageDraw)) drawing onto an L mask per colour/opa."""
    for color, opa, fn in draw_ops:
        mask = Image.new("L", (W, H), 0)
        fn(ImageDraw.Draw(mask))
        cv.blend(0, 0, rgb(color), np.asarray(mask).astype(float) / 255 * (opa / 255))


def font(path, slot):
    return ImageFont.truetype(path, S.FONT_PX[slot])


def baselines(kind, base, n):
    if kind == "entry":
        return S.ENTRY_BASELINES
    if kind == "willing":
        return S.WILLING_BASELINES
    pitch = S.LINE_PITCH[base]
    y0 = S.QUESTION_CENTER_Y - (n - 1) * pitch / 2.0 + S.QUESTION_BASELINE_OFFSET
    return [int(round(y0 + i * pitch)) for i in range(n)]


def render(ttf, pal_name, day, weather, sid, focus=0, frame=0, rainbow_left=False, seed=7):
    pal = S.THEMES[pal_name]
    cv = Canvas(pal)
    sp = A.all_sprites() if not hasattr(render, "sp") else render.sp
    render.sp = sp
    rng = np.random.default_rng(seed)
    if day and weather == "rainbow":
        cx = S.RAINBOW["x_left"] if rainbow_left else S.RAINBOW["x_right"]
        draw_sprite(cv, "rainbow", cx, S.RAINBOW["y"], sprites=sp)
    if day and weather == "sun":
        draw_sprite(cv, "sun_body", S.SUN["x"], S.SUN["y"], S.SUN["color"], sprites=sp)
        draw_sprite(cv, "sun_rays", S.SUN["x"], S.SUN["y"], S.SUN["ray_color"],
                    rotation=(frame // 2) % 360, sprites=sp)
    if not day:
        draw_sprite(cv, "moon", S.MOON["x"], S.MOON["y"], S.MOON["color"], sprites=sp)
        for i, (x, y, s10, ph) in enumerate(S.STARS):
            speed = 5 + i % 5
            sine = int(32767 * math.sin(math.radians((ph + frame * speed) % 360)))
            base = 89 + (166 * (sine + 32767)) // 65534
            opa = base * s10 // 10 if s10 < 11 else base * (s10 - 6) // 8
            px = 1 if s10 < 11 else 2
            cv.blend(x, y, rgb(pal["text"]), np.full((px, px), min(opa, 255) / 255))
    speeds = [64, 51, 38]
    for i, (cx, cy, _) in enumerate(S.CLOUDS):
        name = f"cloud{i}_{'morning' if day else 'dusk'}"
        _, data, pivot = sp[name]
        drift = (frame * speeds[i]) >> 8
        left = ((cx + drift) % S.CLOUD_WRAP) - 40 - data.shape[1] // 2
        cv.blend(left, cy - data.shape[0] // 2, data[..., :3].astype(float),
                 data[..., 3].astype(float) / 255)
    if day and weather == "rain":
        for _ in range(S.RAIN_DROPS):
            x, y = int(rng.integers(10, 250)), int(rng.integers(-12, 308))
            m = Image.new("L", (W, H), 0)
            ImageDraw.Draw(m).line([(x, y), (x - 2, y + 7)], fill=150, width=1)
            cv.blend(0, 0, rgb(0x8296AA), np.asarray(m).astype(float) / 255)
    if day and weather == "leaves":
        for i in range(S.LEAVES):
            x, y = int(rng.integers(10, 230)), int(rng.integers(10, 200))
            draw_sprite(cv, "leaf", x, y, 0xD8822E if i & 1 else 0xE8A040,
                        rotation=float(rng.integers(0, 360)), sprites=sp)

    _, kind, base, emph, _, lines = SENT[sid]
    ops = []
    under = []
    for line, bl in zip(lines, baselines(kind, base, len(lines))):
        fonts = [font(ttf, emph if e else base) for _, e in line]
        widths = [int(round(f.getlength(t))) for (t, _), f in zip(line, fonts)]
        x = (W - sum(widths)) // 2
        for (t, e), f, w in zip(line, fonts, widths):
            col = pal["accent"] if e else pal["text"]
            opa = S.NOTE_OPA if kind == "note" else 255
            ops.append((col, opa, lambda d, x=x, t=t, f=f, bl=bl: d.text((x, bl), t, font=f, anchor="ls", fill=255)))
            if e:
                under.append((x + S.UNDERLINE_INSET, bl + S.UNDERLINE_DROP,
                              x + w - S.UNDERLINE_INSET, bl + S.UNDERLINE_DROP + S.UNDERLINE_SLANT))
            x += w
    text_layer(cv, ops)
    for x0, y0, x1, y1 in under:
        m = Image.new("L", (W * 4, H * 4), 0)
        ImageDraw.Draw(m).line([(x0 * 4, y0 * 4), (x1 * 4, y1 * 4)], fill=255,
                               width=S.UNDERLINE_WIDTH * 4, joint="curve")
        m = m.resize((W, H), Image.LANCZOS)
        cv.blend(0, 0, rgb(pal["accent"]), np.asarray(m).astype(float) / 255 * S.UNDERLINE_OPA / 255)

    if kind in ("entry", "willing"):
        opts = S.ENTRY_OPTIONS if kind == "entry" else S.WILLING_OPTIONS
        bls = S.ENTRY_OPTION_BASELINES if kind == "entry" else S.WILLING_OPTION_BASELINES
        f = font(ttf, S.OPTION_FONT)
        ascent = S.FONT_PX[S.OPTION_FONT] - 3      # lv_font_conv: line_height - base_line
        for i, (o, bl) in enumerate(zip(opts, bls)):
            w = int(round(f.getlength(o)))
            x = (W - w) // 2
            col = pal["text"] if i == focus else pal["dim"]
            text_layer(cv, [(col, 255, lambda d, x=x, o=o, bl=bl: d.text((x, bl), o, font=f, anchor="ls", fill=255))])
            if i == focus:
                cy = bl - ascent * S.OPTION_DOT_RISE_PCT // 100
                r = S.OPTION_DOT_RADIUS
                m = Image.new("L", (W * 4, H * 4), 0)
                dx = x - S.OPTION_DOT_GAP
                ImageDraw.Draw(m).ellipse([(dx - r) * 4, (cy - r) * 4, (dx + r) * 4, (cy + r) * 4], fill=255)
                m = m.resize((W, H), Image.LANCZOS)
                cv.blend(0, 0, rgb(pal["accent"]), np.asarray(m).astype(float) / 255)
    if kind in ("question", "note"):
        m = Image.new("L", (W, H), 0)
        d = ImageDraw.Draw(m)
        hx = S.HINT_X
        d.line([(hx - 3, S.HINT_Y_UP + 2), (hx, S.HINT_Y_UP - 1), (hx + 3, S.HINT_Y_UP + 2)], fill=255, width=1)
        d.line([(hx - 3, S.HINT_Y_DOWN - 1), (hx, S.HINT_Y_DOWN + 2), (hx + 3, S.HINT_Y_DOWN - 1)], fill=255, width=1)
        d.rectangle([hx - 1, S.HINT_Y_OK - 1, hx, S.HINT_Y_OK], fill=255)
        cv.blend(0, 0, rgb(pal["faint"]), np.asarray(m).astype(float) / 255)

    img = cv.image()
    # Panel corner mask (BSP_LVGL_SCREEN_RADIUS = 30): pixels outside are black.
    mask = Image.new("L", (W * 4, H * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W * 4 - 1, H * 4 - 1], radius=30 * 4, fill=255)
    mask = mask.resize((W, H), Image.LANCZOS)
    black = Image.new("RGB", (W, H), (0, 0, 0))
    return Image.composite(img, black, mask)


def sheet(images, path, scale=2):
    gap = 10
    sh = Image.new("RGB", (len(images) * (W + gap) - gap, H), (120, 120, 120))
    for i, im in enumerate(images):
        sh.paste(im, (i * (W + gap), 0))
    sh.resize((sh.width * scale, sh.height * scale), Image.NEAREST).save(path)


def main():
    ttf, out = sys.argv[1], Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    dusk = [("entry", "ENTRY", 0), ("allow", "ALLOW", 0), ("willing", "WILLING", 0),
            ("willing_not", "WILLING", 1), ("note", "LOOP_B_2", 0), ("longest", "GOAL_1", 0),
            ("letgo", "LETGO", 0), ("feel1", "FEEL_1", 0), ("goal5", "GOAL_5", 0),
            ("loopa", "LOOP_A_1", 0), ("loopb1", "LOOP_B_1", 0), ("loopb3", "LOOP_B_3", 0)]
    imgs = []
    for name, sid, focus in dusk:
        im = render(ttf, "dusk", False, None, sid, focus, frame=0)
        im.save(out / f"dusk_{name}.png")
        imgs.append(im)
    sheet(imgs[:6], out / "sheet_dusk_main.png")
    sheet(imgs[6:], out / "sheet_dusk_more.png")
    day = []
    for weather, pal in (("sun", "morning"), ("rain", "rain"), ("rainbow", "morning"), ("leaves", "morning")):
        im = render(ttf, pal, True, weather, "ALLOW", frame=0)
        im.save(out / f"morning_{weather}_allow.png")
        day.append(im)
    left = render(ttf, "morning", True, "rainbow", "ENTRY", rainbow_left=True)
    left.save(out / "morning_rainbow_left_entry.png")
    day.append(left)
    sheet(day, out / "sheet_morning_weather.png")
    frames = [render(ttf, "dusk", False, None, "ALLOW", frame=f * 4) for f in range(30)]
    frames[0].save(out / "dusk_allow_motion.gif", save_all=True, append_images=frames[1:],
                   duration=S.FX_PERIOD_MS * 4, loop=0)
    print(f"previews written to {out}")


if __name__ == "__main__":
    main()

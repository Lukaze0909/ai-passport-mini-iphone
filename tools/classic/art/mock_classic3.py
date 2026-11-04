#!/usr/bin/env python3
"""v3: call with black panel, pomodoro timer, weather+AQI, settings (Wi-Fi switch)."""
import math, sys
from PIL import Image, ImageDraw, ImageFilter
import mock_classic as mc
import mock_classic2 as m2
from mock_classic import COMPANION, S, W, H, font, vgrad, paste_grad, text_c, status_bar

OUT = sys.argv[1] if len(sys.argv) > 1 else "."


def nav_bar(im, title, y0=14 * S, h=30 * S):
    paste_grad(im, (0, y0, W, y0 + h), [(0, (176, 188, 205, 255)), (0.5, (134, 151, 175, 255)),
                                       (0.51, (115, 134, 160, 255)), (1, (108, 128, 156, 255))])
    d = ImageDraw.Draw(im)
    d.line([(0, y0 + h - S), (W, y0 + h - S)], fill=(45, 60, 80, 255), width=S)
    text_c(d, W / 2, y0 + 5 * S, title, font(13), (255, 255, 255, 255), shadow=(60, 74, 96, 255))


def pinstripe():
    im = Image.new("RGBA", (W, H), (197, 204, 212, 255))
    d = ImageDraw.Draw(im)
    for x in range(0, W, 5 * S // 2):
        d.line([(x, 0), (x, H)], fill=(203, 210, 218, 255), width=S)
    return im


def glossy_button(im, box, top, mid, low, bottom, label, r=8 * S, size=14):
    paste_grad(im, box, [(0, top + (255,)), (0.5, mid + (255,)), (0.51, low + (255,)), (1, bottom + (255,))], r)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle(box, r, outline=(0, 0, 0, 120), width=S)
    text_c(d, (box[0] + box[2]) / 2, box[1] + (box[3] - box[1]) / 2 - size * .62 * S, label, font(size),
           (255, 255, 255, 255), shadow=(0, 0, 0, 120))


# ---------------------------------------------------------------- call, black panel
def call3():
    im = m2.ripple_wallpaper()
    status_bar(im, label=COMPANION, bg=(0, 0, 0, 70))
    paste_grad(im, (0, 14 * S, W, 74 * S), [(0, (40, 44, 52, 130)), (1, (10, 12, 16, 150))])
    d = ImageDraw.Draw(im)
    text_c(d, W / 2, 16 * S, COMPANION, font(22, bold=False), (255, 255, 255, 255), shadow=(0, 0, 0, 160))
    text_c(d, W / 2, 50 * S, "正在说… 0:07", font(10.5, bold=False), (230, 236, 244, 255))
    px0, py0, px1, py1 = 22 * S, 96 * S, 218 * S, 222 * S
    paste_grad(im, (px0, py0, px1, py1), [(0, (34, 36, 40, 225)), (0.5, (18, 19, 22, 225)),
                                         (1, (8, 8, 10, 230))], 8 * S)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([px0, py0, px1, py1], 8 * S, outline=(150, 156, 166, 150), width=S)
    cw, ch = (px1 - px0) / 3, (py1 - py0) / 2
    for k in (1, 2):
        d.line([(px0 + cw * k, py0 + S), (px0 + cw * k, py1 - S)], fill=(0, 0, 0, 255), width=S)
        d.line([(px0 + cw * k + S, py0 + S), (px0 + cw * k + S, py1 - S)], fill=(90, 94, 102, 160), width=S)
    d.line([(px0 + S, py0 + ch), (px1 - S, py0 + ch)], fill=(0, 0, 0, 255), width=S)
    d.line([(px0 + S, py0 + ch + S), (px1 - S, py0 + ch + S)], fill=(90, 94, 102, 160), width=S)
    # reuse v2 cell drawing by rendering glyphs on top: simple copy of the logic
    cells = ["说话", "删除", "发送", "静音", "免提", "回听"]
    for i, n in enumerate(cells):
        cx = px0 + cw * (i % 3 + .5)
        cy0 = py0 + ch * (i // 3)
        if i == 0:
            hl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            ImageDraw.Draw(hl).rectangle([px0 + 2 * S, cy0 + 2 * S, px0 + cw - S, cy0 + ch - S],
                                         fill=(220, 60, 50, 150))
            im.alpha_composite(hl)
            d = ImageDraw.Draw(im)
        gy = cy0 + ch * .38
        w_ = (255, 255, 255, 240)
        if n == "说话":
            d.rounded_rectangle([cx - 4.5 * S, gy - 11 * S, cx + 4.5 * S, gy + 3 * S], 4.5 * S, fill=w_)
            d.arc([cx - 8 * S, gy - 6 * S, cx + 8 * S, gy + 8 * S], 0, 180, fill=w_, width=int(1.6 * S))
            d.line([(cx, gy + 8 * S), (cx, gy + 12 * S)], fill=w_, width=int(1.6 * S))
            d.ellipse([cx + 7 * S, gy - 13 * S, cx + 12 * S, gy - 8 * S], fill=(255, 60, 50, 255))
        elif n == "删除":
            d.rectangle([cx - 7 * S, gy - 10 * S, cx + 7 * S, gy - 8 * S], fill=w_)
            d.rectangle([cx - 2.5 * S, gy - 12.5 * S, cx + 2.5 * S, gy - 10 * S], fill=w_)
            d.polygon([(cx - 6 * S, gy - 6.5 * S), (cx + 6 * S, gy - 6.5 * S), (cx + 4.5 * S, gy + 9 * S),
                       (cx - 4.5 * S, gy + 9 * S)], fill=w_)
        elif n == "发送":
            d.polygon([(cx - 10 * S, gy - 1 * S), (cx + 10 * S, gy - 9 * S), (cx + 3 * S, gy + 10 * S),
                       (cx, gy + 2 * S)], fill=w_)
        elif n in ("静音", "免提"):
            d.polygon([(cx - 9 * S, gy - 4 * S), (cx - 4 * S, gy - 4 * S), (cx + 2 * S, gy - 10 * S),
                       (cx + 2 * S, gy + 10 * S), (cx - 4 * S, gy + 4 * S), (cx - 9 * S, gy + 4 * S)], fill=w_)
            if n == "静音":
                d.line([(cx + 5 * S, gy - 5 * S), (cx + 11 * S, gy + 5 * S)], fill=w_, width=int(1.6 * S))
                d.line([(cx + 11 * S, gy - 5 * S), (cx + 5 * S, gy + 5 * S)], fill=w_, width=int(1.6 * S))
            else:
                for r in (5, 9):
                    d.arc([cx + 2 * S - r * S, gy - r * S, cx + 2 * S + r * S, gy + r * S], -45, 45,
                          fill=w_, width=int(1.5 * S))
        else:
            d.arc([cx - 9 * S, gy - 9 * S, cx + 9 * S, gy + 9 * S], 200, 520, fill=w_, width=int(1.8 * S))
            d.polygon([(cx - 3 * S, gy - 5 * S), (cx + 5 * S, gy), (cx - 3 * S, gy + 5 * S)], fill=w_)
        text_c(d, cx, cy0 + ch * .70, n, font(8.5), (255, 255, 255, 255), shadow=(0, 0, 0, 200), sh_off=(0, -1))
    paste_grad(im, (0, 246 * S, W, H), [(0, (40, 44, 52, 150)), (1, (8, 10, 14, 190))])
    glossy_button(im, (22 * S, 264 * S, 218 * S, 300 * S), (246, 112, 104), (214, 44, 38), (194, 22, 18),
                  (214, 50, 44), "结束")
    return im


# ---------------------------------------------------------------- pomodoro
def timer3(running=False):
    im = Image.new("RGBA", (W, H), (20, 20, 22, 255))
    status_bar(im, label=COMPANION, bg=(0, 0, 0, 255))
    nav_bar(im, "番茄钟")
    d = ImageDraw.Draw(im)
    if not running:
        # picker wheel
        x0, y0, x1, y1 = 16 * S, 60 * S, 224 * S, 210 * S
        paste_grad(im, (x0 - 6 * S, y0 - 6 * S, x1 + 6 * S, y1 + 6 * S),
                   [(0, (70, 72, 80, 255)), (1, (30, 31, 36, 255))], 8 * S)
        paste_grad(im, (x0, y0, x1, y1), [(0, (150, 150, 154, 255)), (0.18, (250, 250, 250, 255)),
                                         (0.82, (250, 250, 250, 255)), (1, (150, 150, 154, 255))], 4 * S)
        d = ImageDraw.Draw(im)
        items = ["3 分钟", "5 分钟", "15 分钟", "30 分钟", "45 分钟"]
        cy = (y0 + y1) / 2
        for k, s in enumerate(items):
            off = (k - 2) * 28 * S
            a = 255 if k == 2 else max(70, 210 - abs(k - 2) * 60)
            text_c(d, W / 2, cy + off - 11 * S, s, font(16 if k == 2 else 14),
                   (20, 20, 24, a))
        # selection bar
        sel = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        sd = ImageDraw.Draw(sel)
        sd.rectangle([x0 - 4 * S, cy - 15 * S, x1 + 4 * S, cy + 15 * S], fill=(140, 160, 210, 90),
                     outline=(110, 120, 150, 200), width=S)
        im.alpha_composite(sel)
        d = ImageDraw.Draw(im)
        text_c(d, W / 2, 222 * S, "1 · 3 · 5 · 15 · 30 · 45 · 60 分钟", font(9, bold=False), (190, 194, 202, 255))
        glossy_button(im, (22 * S, 252 * S, 218 * S, 290 * S), (130, 210, 110), (60, 170, 50),
                      (36, 148, 30), (60, 170, 50), "开始")
    else:
        text_c(d, W / 2, 84 * S, "12:34", font(52, bold=False), (255, 255, 255, 255))
        text_c(d, W / 2, 152 * S, "15 分钟 · 专注中", font(11, bold=False), (190, 194, 202, 255))
        # progress ring arc
        cx, cy, r = W / 2, 120 * S, 0
        bx0, bx1, by = 30 * S, 210 * S, 186 * S
        d.rounded_rectangle([bx0, by, bx1, by + 6 * S], 3 * S, fill=(60, 62, 70, 255))
        d.rounded_rectangle([bx0, by, bx0 + (bx1 - bx0) * .18, by + 6 * S], 3 * S, fill=(240, 90, 70, 255))
        glossy_button(im, (22 * S, 252 * S, 218 * S, 290 * S), (246, 112, 104), (214, 44, 38),
                      (194, 22, 18), (214, 50, 44), "取消")
    d = ImageDraw.Draw(im)
    text_c(d, W / 2, 300 * S, "时间到了会响铃，不用联网", font(8.5, bold=False), (140, 144, 152, 255))
    return im


# ---------------------------------------------------------------- weather
def weather3():
    im = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    status_bar(im, label=COMPANION, bg=(0, 0, 0, 255))
    x0, y0, x1, y1 = 10 * S, 22 * S, 230 * S, 310 * S
    paste_grad(im, (x0, y0, x1, y1), [(0, (92, 154, 222, 255)), (1, (34, 86, 160, 255))], 12 * S)
    gl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(gl).rounded_rectangle([x0, y0, x1, y0 + 70 * S], 12 * S, fill=(255, 255, 255, 40))
    im.alpha_composite(gl)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([x0, y0, x1, y1], 12 * S, outline=(255, 255, 255, 120), width=S)
    d.text((22 * S, 30 * S), "上海", font=font(18), fill=(255, 255, 255, 255))
    d.text((22 * S, 56 * S), "多云  最高 19° 最低 13°", font=font(9, bold=False), fill=(230, 238, 250, 255))
    # sun+cloud
    d.ellipse([150 * S, 34 * S, 186 * S, 70 * S], fill=(255, 214, 70, 255))
    for (cx, cy, r) in [(168, 70, 14), (186, 64, 16), (204, 72, 12)]:
        d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], fill=(255, 255, 255, 245))
    d.rectangle([156 * S, 70 * S, 214 * S, 84 * S], fill=(255, 255, 255, 245))
    text_c(d, 70 * S, 82 * S, "17°", font(46, bold=False), (255, 255, 255, 255), shadow=(20, 50, 100, 160))
    # AQI pill
    ax0, ay0, ax1, ay1 = 22 * S, 150 * S, 218 * S, 196 * S
    paste_grad(im, (ax0, ay0, ax1, ay1), [(0, (255, 255, 255, 60)), (1, (255, 255, 255, 30))], 8 * S)
    d = ImageDraw.Draw(im)
    d.text((32 * S, 156 * S), "空气质量", font=font(9, bold=False), fill=(230, 238, 250, 255))
    d.text((32 * S, 170 * S), "AQI 62", font=font(15), fill=(255, 255, 255, 255))
    d.rounded_rectangle([150 * S, 166 * S, 206 * S, 186 * S], 10 * S, fill=(250, 210, 60, 255))
    text_c(d, 178 * S, 167.5 * S, "良", font(11), (90, 60, 0, 255))
    # forecast rows
    rows = [("周日", "☀", "20°", "12°"), ("周一", "☁", "18°", "13°"), ("周二", "☂", "16°", "12°")]
    for k, (day, ic, hi, lo) in enumerate(rows):
        y = 210 * S + k * 30 * S
        d.line([(22 * S, y - 4 * S), (218 * S, y - 4 * S)], fill=(255, 255, 255, 70), width=S)
        d.text((26 * S, y), day, font=font(11), fill=(255, 255, 255, 255))
        text_c(d, 120 * S, y - 1 * S, ic, font(13, bold=False), (255, 255, 255, 255))
        d.text((170 * S, y), hi, font=font(11), fill=(255, 255, 255, 255))
        d.text((198 * S, y), lo, font=font(11, bold=False), fill=(200, 215, 240, 255))
    text_c(d, W / 2, 296 * S, "22:55 更新", font(8, bold=False), (210, 222, 240, 255))
    return im


# ---------------------------------------------------------------- settings
def settings3(on=True):
    im = pinstripe()
    status_bar(im, label=COMPANION, bg=(0, 0, 0, 255))
    nav_bar(im, "设置")
    d = ImageDraw.Draw(im)
    gx0, gx1 = 10 * S, 230 * S
    gy0 = 62 * S
    d.rounded_rectangle([gx0, gy0, gx1, gy0 + 40 * S], 9 * S, fill=(255, 255, 255, 255),
                        outline=(170, 174, 182, 255), width=S)
    # wifi icon tile
    tile = m2.vgrad(24 * S, 24 * S, [(0, (90, 170, 240, 255)), (1, (30, 100, 200, 255))])
    im.alpha_composite(tile, (18 * S, gy0 + 8 * S))
    d = ImageDraw.Draw(im)
    cx, cy = 30 * S, gy0 + 27 * S
    for k in range(3):
        r = (4 + k * 4) * S
        d.arc([cx - r, cy - r, cx + r, cy + r], 225, 315, fill=(255, 255, 255, 255), width=int(1.6 * S))
    d.text((50 * S, gy0 + 10 * S), "Wi-Fi", font=font(13), fill=(20, 20, 24, 255))
    # classic switch
    sx0, sy0, sx1, sy1 = 158 * S, gy0 + 8 * S, 222 * S, gy0 + 32 * S
    if on:
        paste_grad(im, (sx0, sy0, sx1, sy1), [(0, (40, 110, 220, 255)), (1, (90, 160, 250, 255))], 12 * S)
        d = ImageDraw.Draw(im)
        text_c(d, sx0 + 21 * S, sy0 + 3 * S, "开", font(11), (255, 255, 255, 255))
        kx = sx1 - 24 * S
    else:
        paste_grad(im, (sx0, sy0, sx1, sy1), [(0, (220, 220, 222, 255)), (1, (250, 250, 250, 255))], 12 * S)
        d = ImageDraw.Draw(im)
        text_c(d, sx1 - 21 * S, sy0 + 3 * S, "关", font(11), (120, 120, 124, 255))
        kx = sx0
    paste_grad(im, (kx, sy0, kx + 24 * S, sy1), [(0, (250, 250, 250, 255)), (1, (206, 206, 210, 255))], 12 * S)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([sx0, sy0, sx1, sy1], 12 * S, outline=(110, 110, 116, 255), width=S)
    # status group
    gy1 = gy0 + 56 * S
    d.rounded_rectangle([gx0, gy1, gx1, gy1 + 64 * S], 9 * S, fill=(255, 255, 255, 255),
                        outline=(170, 174, 182, 255), width=S)
    d.line([(gx0, gy1 + 32 * S), (gx1, gy1 + 32 * S)], fill=(210, 212, 216, 255), width=S)
    d.text((22 * S, gy1 + 8 * S), "网络", font=font(12), fill=(20, 20, 24, 255))
    d.text((150 * S, gy1 + 9 * S), "家里 ✓", font=font(11, bold=False), fill=(56, 84, 135, 255))
    d.text((22 * S, gy1 + 40 * S), COMPANION, font=font(12), fill=(20, 20, 24, 255))
    d.text((150 * S, gy1 + 41 * S), "已连上", font=font(11, bold=False), fill=(56, 84, 135, 255))
    text_c(d, W / 2, gy1 + 76 * S, "开着时会先找家里的网，", font(9, bold=False), (76, 86, 108, 255),
           shadow=(255, 255, 255, 200), sh_off=(0, 1))
    text_c(d, W / 2, gy1 + 90 * S, "找不到就连你的手机热点。", font(9, bold=False), (76, 86, 108, 255),
           shadow=(255, 255, 255, 200), sh_off=(0, 1))
    return im


if __name__ == "__main__":
    mc.OUT = OUT
    shots = [mc.finish(call3(), "e1_call"), mc.finish(timer3(False), "e2_timer"),
             mc.finish(timer3(True), "e2b_timer_run"), mc.finish(weather3(), "e3_weather"),
             mc.finish(settings3(True), "e4_settings")]
    order = [shots[0], shots[1], shots[2], shots[3], shots[4]]
    sheet = Image.new("RGB", (5 * 400 + 6 * 20, 533 + 40), (236, 236, 238))
    for i, s in enumerate(order):
        sheet.paste(s.resize((400, 533), Image.LANCZOS).convert("RGB"), (20 + i * 420, 20))
    sheet.save(f"{OUT}/classic_v3_sheet.png")
    print("ok")

#!/usr/bin/env python3
"""v2: water-drop wallpaper, 4-column classic icons, native-style dock and call screen.
All artwork is drawn here from primitives (no third-party images or icon sets)."""
import math, random, sys
from PIL import Image, ImageDraw, ImageFilter, ImageChops
import mock_classic as mc
from mock_classic import COMPANION, S, W, H, font, vgrad, rmask, paste_grad, text_c, status_bar

OUT = sys.argv[1] if len(sys.argv) > 1 else "."


# ---------------------------------------------------------------- wallpapers
def droplet_wallpaper(seed=4):
    im = vgrad(W, H, [(0, (150, 168, 180, 255)), (0.5, (118, 138, 152, 255)),
                      (1, (82, 100, 114, 255))])
    # faint diagonal light
    li = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(li).polygon([(0, 0), (W * .7, 0), (0, H * .55)], fill=(255, 255, 255, 34))
    im.alpha_composite(li.filter(ImageFilter.GaussianBlur(30 * S)))
    rnd = random.Random(seed)
    drops = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for _ in range(170):
        r = rnd.choice([1.2, 1.6, 2.0, 2.6, 3.4, 4.5, 6.0]) * S
        x, y = rnd.uniform(0, W), rnd.uniform(0, H)
        ry = r * rnd.uniform(1.0, 1.25)
        dd = ImageDraw.Draw(drops)
        # shadow under drop (bottom-right)
        dd.ellipse([x - r + .6 * S, y - ry + 1.2 * S, x + r + .6 * S, y + ry + 1.2 * S],
                   fill=(30, 44, 56, 90))
        # body: slightly darker than background
        dd.ellipse([x - r, y - ry, x + r, y + ry], fill=(96, 116, 130, 150))
        # bright rim at bottom (refraction)
        dd.arc([x - r, y - ry, x + r, y + ry], 20, 160, fill=(235, 245, 250, 170),
               width=max(1, int(r * .28)))
        # specular dot top-left
        sr = max(.5 * S, r * .26)
        dd.ellipse([x - r * .45 - sr, y - ry * .5 - sr, x - r * .45 + sr, y - ry * .5 + sr],
                   fill=(255, 255, 255, 220))
    im.alpha_composite(drops.filter(ImageFilter.GaussianBlur(.35 * S)))
    return im


def ripple_wallpaper():
    im = vgrad(W, H, [(0, (70, 120, 170, 255)), (1, (24, 70, 120, 255))])
    d = ImageDraw.Draw(im)
    rnd = random.Random(9)
    for y in range(0, H, int(3 * S)):
        ph = rnd.uniform(0, 6.28)
        pts = [(x, y + math.sin(x / (14 * S) + ph) * 1.6 * S) for x in range(0, W + 8, 8)]
        d.line(pts, fill=(255, 255, 255, rnd.randint(10, 34)), width=S)
    return im.filter(ImageFilter.GaussianBlur(.8 * S))


# ---------------------------------------------------------------- icon base
def icon(im, x, y, sz, top, bottom, draw, name, gloss=True, label=True):
    r = int(sz * 0.2)
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([x, y + 1.5 * S, x + sz, y + sz + 1.5 * S], r,
                                         fill=(0, 0, 0, 150))
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(1.2 * S)))
    tile = vgrad(sz, sz, [(0, top + (255,)), (1, bottom + (255,))])
    draw(tile, sz)
    if gloss:
        gl = Image.new("RGBA", (sz, sz), (0, 0, 0, 0))
        ImageDraw.Draw(gl).ellipse([-sz * .4, -sz * .70, sz * 1.4, sz * .48], fill=(255, 255, 255, 80))
        tile.alpha_composite(gl)
    tile.putalpha(ImageChops.multiply(tile.split()[3], rmask(sz, sz, r)))
    im.alpha_composite(tile, (int(x), int(y)))
    if label:
        d = ImageDraw.Draw(im)
        text_c(d, x + sz / 2, y + sz + 1.5 * S, name, font(8.2), (255, 255, 255, 255),
               shadow=(0, 0, 0, 220), sh_off=(0, 1))


D = ImageDraw.Draw


def ic_messages(t, s):
    d = D(t)
    d.ellipse([s * .16, s * .20, s * .84, s * .70], fill=(255, 255, 255, 255))
    d.polygon([(s * .28, s * .60), (s * .22, s * .82), (s * .44, s * .66)], fill=(255, 255, 255, 255))


def ic_calendar(t, s):
    d = D(t)
    d.rectangle([0, 0, s, s * .30], fill=(206, 40, 36, 255))
    text_c(d, s / 2, s * .02, "周六", font(s / S * .19), (255, 255, 255, 255))
    text_c(d, s / 2, s * .26, "31", font(s / S * .50, bold=False), (30, 30, 30, 255))


def ic_photos(t, s):
    d = D(t)
    cx, cy = s * .5, s * .52
    for k in range(14):
        a = k * 2 * math.pi / 14
        px, py = cx + math.cos(a) * s * .24, cy + math.sin(a) * s * .24
        d.ellipse([px - s * .11, py - s * .11, px + s * .11, py + s * .11], fill=(250, 196, 30, 255))
    d.ellipse([cx - s * .17, cy - s * .17, cx + s * .17, cy + s * .17], fill=(110, 70, 26, 255))
    d.ellipse([cx - s * .10, cy - s * .10, cx + s * .10, cy + s * .10], fill=(80, 50, 18, 255))


def ic_weather(t, s):
    d = D(t)
    d.ellipse([s * .18, s * .16, s * .62, s * .60], fill=(255, 214, 60, 255))
    text_c(d, s * .62, s * .50, "18°", font(s / S * .26), (255, 255, 255, 255))


def ic_release(t, s):
    d = D(t)
    d.ellipse([s * .54, s * .18, s * .80, s * .44], fill=(255, 220, 100, 255))
    for (cx, cy, r) in [(.36, .60, .15), (.52, .53, .18), (.68, .62, .13)]:
        d.ellipse([s * (cx - r), s * (cy - r), s * (cx + r), s * (cy + r)], fill=(255, 255, 255, 255))
    d.rectangle([s * .22, s * .60, s * .80, s * .75], fill=(255, 255, 255, 255))


def ic_clock(t, s):
    d = D(t)
    c, r = s / 2, s * .38
    d.ellipse([c - r, c - r, c + r, c + r], fill=(250, 250, 250, 255))
    for k in range(12):
        a = k * math.pi / 6
        d.line([(c + math.cos(a) * r * .78, c + math.sin(a) * r * .78),
                (c + math.cos(a) * r * .92, c + math.sin(a) * r * .92)], fill=(40, 40, 40, 255),
               width=max(1, int(s * .02)))
    d.line([(c, c), (c + r * .45, c - r * .25)], fill=(20, 20, 20, 255), width=int(s * .05))
    d.line([(c, c), (c - r * .15, c - r * .68)], fill=(20, 20, 20, 255), width=int(s * .035))
    d.line([(c, c), (c + r * .2, c + r * .7)], fill=(210, 40, 30, 255), width=max(1, int(s * .015)))


def ic_notes(t, s):
    d = D(t)
    d.rectangle([0, 0, s, s * .2], fill=(126, 80, 40, 255))
    for k in range(5):
        y = s * (.36 + k * .13)
        d.line([(s * .08, y), (s * .92, y)], fill=(170, 150, 90, 255), width=max(1, int(s * .015)))
    d.line([(s * .22, s * .2), (s * .22, s)], fill=(220, 110, 100, 200), width=max(1, int(s * .012)))


def ic_camera(t, s):
    d = D(t)
    d.rounded_rectangle([s * .12, s * .28, s * .88, s * .76], s * .06, fill=(60, 62, 68, 255))
    c = s / 2
    for r, col in [(.20, (200, 202, 208)), (.15, (40, 44, 60)), (.09, (70, 90, 140))]:
        d.ellipse([c - s * r, s * .52 - s * r, c + s * r, s * .52 + s * r], fill=col + (255,))
    d.ellipse([c - s * .05, s * .44, c - s * .01, s * .48], fill=(255, 255, 255, 220))


def ic_settings(t, s):
    d = D(t)
    for (cx, cy, rr, n) in [(.42, .44, .26, 10), (.70, .72, .16, 8)]:
        cx, cy, rr = s * cx, s * cy, s * rr
        for k in range(n):
            a = k * 2 * math.pi / n
            d.line([(cx + math.cos(a) * rr * .6, cy + math.sin(a) * rr * .6),
                    (cx + math.cos(a) * rr, cy + math.sin(a) * rr)], fill=(200, 202, 208, 255),
                   width=int(rr * .45))
        d.ellipse([cx - rr * .78, cy - rr * .78, cx + rr * .78, cy + rr * .78], fill=(200, 202, 208, 255))
        d.ellipse([cx - rr * .32, cy - rr * .32, cx + rr * .32, cy + rr * .32], fill=(80, 82, 90, 255))


def ic_phone(t, s):
    k = 4
    big = s * k
    hs = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hs)
    cx, cy, r = big * .70, big * .50, big * .34
    hd.arc([cx - r, cy - r, cx + r, cy + r], 130, 230, fill=(255, 255, 255, 255), width=int(big * .13))
    for ang in (130, 230):
        a = math.radians(ang)
        ex, ey = cx + math.cos(a) * (r - big * .065), cy + math.sin(a) * (r - big * .065)
        pts = [(ex + math.cos(math.radians(q)) * big * .14 + big * .06,
                ey + math.sin(math.radians(q)) * big * .085) for q in range(0, 360, 10)]
        hd.polygon(pts, fill=(255, 255, 255, 255))
    hs = hs.rotate(-30, resample=Image.BICUBIC).resize((s, s), Image.LANCZOS)
    t.alpha_composite(hs, (int(-s * .10), int(-s * .04)))


def ic_mail(t, s):
    d = D(t)
    d.rectangle([s * .14, s * .28, s * .86, s * .72], fill=(255, 255, 255, 255))
    d.line([(s * .14, s * .28), (s * .5, s * .54), (s * .86, s * .28)], fill=(150, 170, 200, 255),
           width=max(1, int(s * .03)))


def ic_safari(t, s):
    d = D(t)
    c, r = s / 2, s * .38
    d.ellipse([c - r, c - r, c + r, c + r], fill=(245, 248, 252, 255))
    d.ellipse([c - r * .86, c - r * .86, c + r * .86, c + r * .86], fill=(70, 150, 230, 255))
    d.polygon([(c + r * .55, c - r * .55), (c + r * .1, c + r * .1), (c - r * .1, c - r * .1)],
              fill=(230, 50, 40, 255))
    d.polygon([(c - r * .55, c + r * .55), (c + r * .1, c + r * .1), (c - r * .1, c - r * .1)],
              fill=(255, 255, 255, 255))


def ic_music(t, s):
    d = D(t)
    d.ellipse([s * .22, s * .56, s * .44, s * .74], fill=(255, 255, 255, 255))
    d.ellipse([s * .54, s * .50, s * .76, s * .68], fill=(255, 255, 255, 255))
    d.rectangle([s * .38, s * .22, s * .44, s * .66], fill=(255, 255, 255, 255))
    d.rectangle([s * .70, s * .16, s * .76, s * .60], fill=(255, 255, 255, 255))
    d.polygon([(s * .38, s * .22), (s * .76, s * .16), (s * .76, s * .28), (s * .38, s * .34)],
              fill=(255, 255, 255, 255))


APPS = [
    ("留言", (120, 220, 100), (40, 160, 40), ic_messages),
    ("日历", (255, 255, 255), (226, 226, 226), ic_calendar),
    ("相册", (250, 250, 250), (220, 222, 226), ic_photos),
    ("天气", (90, 170, 240), (30, 100, 200), ic_weather),
    ("释放", (128, 196, 246), (52, 124, 210), ic_release),
    ("时钟", (40, 40, 44), (8, 8, 10), ic_clock),
    ("备忘录", (255, 240, 150), (246, 222, 110), ic_notes),
    ("相机", (196, 198, 204), (130, 132, 140), ic_camera),
    ("设置", (150, 154, 162), (90, 94, 102), ic_settings),
]
DOCK = [
    ("通话", (120, 220, 100), (36, 150, 40), ic_phone),
    ("邮件", (110, 180, 250), (36, 110, 220), ic_mail),
    ("浏览器", (240, 244, 250), (190, 200, 214), ic_safari),
    ("音乐", (255, 170, 60), (236, 110, 20), ic_music),
]


def translucent_status(im):
    status_bar(im, label=COMPANION, bg=(0, 0, 0, 90))


def home2(selected=4):
    im = droplet_wallpaper()
    translucent_status(im)
    sz = 42 * S
    pitch = 60 * S
    x0 = (60 * S - sz) / 2
    for i, (n, a, b, f) in enumerate(APPS):
        x = x0 + (i % 4) * pitch
        y = 22 * S + (i // 4) * 66 * S
        if i == selected:
            halo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            p = 4 * S
            ImageDraw.Draw(halo).rounded_rectangle([x - p, y - p, x + sz + p, y + sz + p], 12 * S,
                                                   fill=(255, 255, 255, 150))
            im.alpha_composite(halo.filter(ImageFilter.GaussianBlur(3 * S)))
        icon(im, x, y, sz, a, b, f, n)
    # one page indicator dot
    d = ImageDraw.Draw(im)
    d.ellipse([W / 2 - 2 * S, 238 * S, W / 2 + 2 * S, 242 * S], fill=(255, 255, 255, 230))
    # native-style dock: reflective glass shelf
    dock = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dock)
    top, bot = 252 * S, H
    shelf = vgrad(W, bot - top, [(0, (255, 255, 255, 120)), (0.45, (220, 228, 236, 80)),
                                 (0.46, (150, 160, 172, 120)), (1, (120, 130, 140, 150))])
    mask = Image.new("L", (W, bot - top), 0)
    ImageDraw.Draw(mask).polygon([(10 * S, 0), (W - 10 * S, 0), (W, bot - top), (0, bot - top)], fill=255)
    shelf.putalpha(ImageChops.multiply(shelf.split()[3], mask))
    dock.alpha_composite(shelf, (0, top))
    dd.line([(10 * S, top), (W - 10 * S, top)], fill=(255, 255, 255, 220), width=S)
    im.alpha_composite(dock)
    for i, (n, a, b, f) in enumerate(DOCK):
        x = x0 + i * pitch
        y = 258 * S
        icon(im, x, y, sz, a, b, f, n)
        # reflection
        refl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        tile = vgrad(int(sz), int(sz), [(0, b + (255,)), (1, a + (255,))])
        tile.putalpha(ImageChops.multiply(rmask(int(sz), int(sz), int(sz * .2)),
                                          Image.linear_gradient("L").resize((int(sz), int(sz))).point(
                                              lambda v: int((255 - v) * .25))))
        refl.alpha_composite(tile, (int(x), int(y + sz + 13 * S)))
        im.alpha_composite(refl)
    return im


def lock2():
    im = droplet_wallpaper(seed=12)
    status_bar(im, label=COMPANION, bg=(0, 0, 0, 0))
    paste_grad(im, (0, 14 * S, W, 88 * S), [(0, (60, 60, 64, 120)), (0.5, (30, 30, 34, 110)),
                                           (0.51, (10, 10, 12, 115)), (1, (0, 0, 0, 120))])
    d = ImageDraw.Draw(im)
    text_c(d, W / 2, 12 * S, "22:55", font(40, bold=False), (255, 255, 255, 255), shadow=(0, 0, 0, 160))
    text_c(d, W / 2, 67 * S, "10月31日 星期六", font(10.5), (255, 255, 255, 235))
    paste_grad(im, (0, 262 * S, W, H), [(0, (60, 60, 64, 120)), (0.5, (30, 30, 34, 110)),
                                       (0.51, (10, 10, 12, 115)), (1, (0, 0, 0, 125))])
    tx0, ty0, tx1, ty1 = 16 * S, 276 * S, 224 * S, 306 * S
    paste_grad(im, (tx0, ty0, tx1, ty1), [(0, (0, 0, 0, 170)), (1, (40, 40, 44, 170))], 9 * S)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([tx0, ty0, tx1, ty1], 9 * S, outline=(255, 255, 255, 50), width=S)
    kx0 = tx0 + 2 * S
    paste_grad(im, (kx0, ty0 + 2 * S, kx0 + 44 * S, ty1 - 2 * S),
               [(0, (252, 252, 252, 255)), (0.5, (222, 222, 226, 255)), (0.51, (196, 198, 204, 255)),
                (1, (226, 228, 232, 255))], 7 * S)
    d = ImageDraw.Draw(im)
    ax, ay = kx0 + 22 * S, (ty0 + ty1) / 2
    d.polygon([(ax - 6 * S, ay - 7 * S), (ax + 6 * S, ay), (ax - 6 * S, ay + 7 * S)], fill=(130, 132, 140, 255))
    f = font(12, bold=False)
    s = "按一下解锁"
    w = d.textlength(s, font=f)
    d.text(((kx0 + 44 * S + tx1) / 2 - w / 2, ay - 9 * S), s, font=f, fill=(255, 255, 255, 200))
    return im


def call2(state="recording"):
    im = ripple_wallpaper()
    status_bar(im, label=COMPANION, bg=(0, 0, 0, 70))
    paste_grad(im, (0, 14 * S, W, 74 * S), [(0, (40, 44, 52, 130)), (1, (10, 12, 16, 150))])
    d = ImageDraw.Draw(im)
    d.line([(0, 74 * S), (W, 74 * S)], fill=(0, 0, 0, 120), width=S)
    text_c(d, W / 2, 16 * S, COMPANION, font(22, bold=False), (255, 255, 255, 255), shadow=(0, 0, 0, 160))
    sub = {"recording": "正在说… 0:07", "idle": "对讲中", "sent": "已发送 ✓"}[state]
    text_c(d, W / 2, 50 * S, sub, font(10.5, bold=False), (230, 236, 244, 255))
    px0, py0, px1, py1 = 22 * S, 96 * S, 218 * S, 222 * S
    paste_grad(im, (px0, py0, px1, py1), [(0, (255, 255, 255, 60)), (1, (255, 255, 255, 34))], 8 * S)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([px0, py0, px1, py1], 8 * S, outline=(255, 255, 255, 120), width=S)
    cw, ch = (px1 - px0) / 3, (py1 - py0) / 2
    for k in (1, 2):
        d.line([(px0 + cw * k, py0), (px0 + cw * k, py1)], fill=(255, 255, 255, 80), width=S)
    d.line([(px0, py0 + ch), (px1, py0 + ch)], fill=(255, 255, 255, 80), width=S)
    cells = ["说话", "删除", "发送", "静音", "免提", "回听"]
    sel = 0
    for i, n in enumerate(cells):
        cx = px0 + cw * (i % 3 + .5)
        cy0 = py0 + ch * (i // 3)
        if i == sel:
            hl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            ImageDraw.Draw(hl).rectangle([px0 + cw * (i % 3) + S, cy0 + S, px0 + cw * (i % 3 + 1) - S,
                                          cy0 + ch - S],
                                         fill=(255, 90, 80, 120) if state == "recording" else (120, 190, 255, 110))
            im.alpha_composite(hl)
            d = ImageDraw.Draw(im)
        gy = cy0 + ch * .38
        wcol = (255, 255, 255, 240)
        if n == "说话":
            d.rounded_rectangle([cx - 4.5 * S, gy - 11 * S, cx + 4.5 * S, gy + 3 * S], 4.5 * S, fill=wcol)
            d.arc([cx - 8 * S, gy - 6 * S, cx + 8 * S, gy + 8 * S], 0, 180, fill=wcol, width=int(1.6 * S))
            d.line([(cx, gy + 8 * S), (cx, gy + 12 * S)], fill=wcol, width=int(1.6 * S))
            if state == "recording":
                d.ellipse([cx + 7 * S, gy - 13 * S, cx + 12 * S, gy - 8 * S], fill=(255, 60, 50, 255))
        elif n == "删除":
            d.rectangle([cx - 7 * S, gy - 10 * S, cx + 7 * S, gy - 8 * S], fill=wcol)
            d.rectangle([cx - 2.5 * S, gy - 12.5 * S, cx + 2.5 * S, gy - 10 * S], fill=wcol)
            d.polygon([(cx - 6 * S, gy - 6.5 * S), (cx + 6 * S, gy - 6.5 * S), (cx + 4.5 * S, gy + 9 * S),
                       (cx - 4.5 * S, gy + 9 * S)], fill=wcol)
        elif n == "发送":
            d.polygon([(cx - 10 * S, gy - 1 * S), (cx + 10 * S, gy - 9 * S), (cx + 3 * S, gy + 10 * S),
                       (cx, gy + 2 * S)], fill=wcol)
        elif n in ("静音", "免提"):
            d.polygon([(cx - 9 * S, gy - 4 * S), (cx - 4 * S, gy - 4 * S), (cx + 2 * S, gy - 10 * S),
                       (cx + 2 * S, gy + 10 * S), (cx - 4 * S, gy + 4 * S), (cx - 9 * S, gy + 4 * S)], fill=wcol)
            if n == "静音":
                d.line([(cx + 5 * S, gy - 5 * S), (cx + 11 * S, gy + 5 * S)], fill=wcol, width=int(1.6 * S))
                d.line([(cx + 11 * S, gy - 5 * S), (cx + 5 * S, gy + 5 * S)], fill=wcol, width=int(1.6 * S))
            else:
                for r in (5, 9):
                    d.arc([cx + 2 * S - r * S, gy - r * S, cx + 2 * S + r * S, gy + r * S], -45, 45,
                          fill=wcol, width=int(1.5 * S))
        else:  # 回听
            d.arc([cx - 9 * S, gy - 9 * S, cx + 9 * S, gy + 9 * S], 200, 520, fill=wcol, width=int(1.8 * S))
            d.polygon([(cx - 3 * S, gy - 5 * S), (cx + 5 * S, gy), (cx - 3 * S, gy + 5 * S)], fill=wcol)
        text_c(d, cx, cy0 + ch * .70, n, font(8.5), (255, 255, 255, 255), shadow=(0, 0, 0, 140), sh_off=(0, 1))
    # bottom bar + End
    paste_grad(im, (0, 246 * S, W, H), [(0, (40, 44, 52, 150)), (1, (8, 10, 14, 190))])
    bx0, by0, bx1, by1 = 22 * S, 264 * S, 218 * S, 300 * S
    paste_grad(im, (bx0, by0, bx1, by1), [(0, (246, 112, 104, 255)), (0.5, (214, 44, 38, 255)),
                                         (0.51, (194, 22, 18, 255)), (1, (214, 50, 44, 255))], 8 * S)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([bx0, by0, bx1, by1], 8 * S, outline=(90, 0, 0, 200), width=S)
    # down-turned handset glyph + label
    hx, hy = W / 2 - 22 * S, (by0 + by1) / 2
    d.arc([hx - 9 * S, hy - 3 * S, hx + 9 * S, hy + 9 * S], 200, 340, fill=(255, 255, 255, 255), width=int(3 * S))
    text_c(d, W / 2 + 12 * S, by0 + 8 * S, "结束", font(14), (255, 255, 255, 255), shadow=(90, 0, 0, 200))
    return im


if __name__ == "__main__":
    fin = mc.finish
    mc.OUT = OUT
    shots = [fin(lock2(), "d1_lock"), fin(home2(), "d2_home"), fin(call2("recording"), "d3_call")]
    sheet = Image.new("RGB", (3 * 480 + 4 * 24, 640 + 48), (236, 236, 238))
    for i, s in enumerate(shots):
        sheet.paste(s.resize((480, 640), Image.LANCZOS).convert("RGB"), (24 + i * (480 + 24), 24))
    sheet.save(f"{OUT}/classic_v2_sheet.png")
    print("ok")

#!/usr/bin/env python3
"""Classic (early-2010s skeuomorphic) system UI mockups for Passport, 240x320.
Everything is drawn from scratch here: gradients, gloss, glass bars."""
import math, random, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

S = 4                      # supersample
W, H = 240 * S, 320 * S
OUT = sys.argv[1] if len(sys.argv) > 1 else "."
FB = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"
FR = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
COMPANION = "Claude"       # sample companion name (the firmware draws CONFIG_CS_COMPANION_NAME live)


def font(sz, bold=True):
    try:
        return ImageFont.truetype(FB if bold else FR, int(sz * S), index=2)
    except OSError:
        return ImageFont.truetype(FB, int(sz * S), index=2)


def vgrad(w, h, stops):
    """stops: list of (pos 0..1, (r,g,b,a))"""
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        t = y / max(1, h - 1)
        for i in range(len(stops) - 1):
            p0, c0 = stops[i]
            p1, c1 = stops[i + 1]
            if p0 <= t <= p1:
                k = (t - p0) / max(1e-6, p1 - p0)
                c = tuple(int(c0[j] + (c1[j] - c0[j]) * k) for j in range(4))
                break
        else:
            c = stops[-1][1]
        for x in range(w):
            px[x, y] = c
    return im


def rmask(w, h, r):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w - 1, h - 1], r, fill=255)
    return m


def paste_grad(base, box, stops, r=0):
    x0, y0, x1, y1 = [int(v) for v in box]
    g = vgrad(x1 - x0, y1 - y0, stops)
    if r:
        a = ImageChops.multiply(g.split()[3], rmask(x1 - x0, y1 - y0, r))
        g.putalpha(a)
    base.alpha_composite(g, (x0, y0))


def text_c(d, cx, y, s, f, fill, shadow=None, sh_off=(0, -1)):
    w = d.textlength(s, font=f)
    if shadow:
        d.text((cx - w / 2 + sh_off[0] * S, y + sh_off[1] * S), s, font=f, fill=shadow)
    d.text((cx - w / 2, y), s, font=f, fill=fill)


# ---------------------------------------------------------------- wallpaper
def dusk_wallpaper(seed=7, moon=True):
    im = vgrad(W, H, [(0, (18, 22, 52, 255)), (0.55, (58, 52, 98, 255)),
                      (0.85, (150, 96, 110, 255)), (1, (214, 142, 112, 255))])
    d = ImageDraw.Draw(im)
    rnd = random.Random(seed)
    for _ in range(46):
        x, y = rnd.uniform(0, W), rnd.uniform(0, H * 0.62)
        r = rnd.choice([0.6, 0.8, 1.0, 1.3]) * S
        a = rnd.randint(120, 255)
        d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 248, 230, a))
    if not moon:
        return im
    # moon
    mx, my, mr = W * 0.76, H * 0.30, 13 * S
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([mx - mr * 2.2, my - mr * 2.2, mx + mr * 2.2, my + mr * 2.2],
                                 fill=(255, 236, 200, 60))
    im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(10 * S)))
    cres = Image.new("L", (W, H), 0)
    cd = ImageDraw.Draw(cres)
    cd.ellipse([mx - mr, my - mr, mx + mr, my + mr], fill=255)
    cd.ellipse([mx - mr + 7 * S, my - mr - 3 * S, mx + mr + 7 * S, my + mr - 3 * S], fill=0)
    im.paste((255, 241, 214, 255), (0, 0), cres)
    return im


def linen():
    rnd = random.Random(3)
    im = Image.new("RGBA", (W, H), (58, 62, 70, 255))
    d = ImageDraw.Draw(im)
    for y in range(0, H, 2):
        c = 58 + rnd.randint(-7, 7)
        d.line([(0, y), (W, y)], fill=(c, c + 4, c + 12, 255), width=1)
    for x in range(0, W, 3):
        c = rnd.randint(-10, 10)
        d.line([(x, 0), (x, H)], fill=(60 + c, 64 + c, 72 + c, 60), width=1)
    return im.filter(ImageFilter.GaussianBlur(0.6 * S))


# ---------------------------------------------------------------- chrome
def status_bar(im, label=COMPANION, wifi=2, bg=(0, 0, 0, 150)):
    h = 14 * S
    bar = Image.new("RGBA", (W, h), bg)
    im.alpha_composite(bar, (0, 0))
    d = ImageDraw.Draw(im)
    # signal bars
    for i in range(5):
        bh = (3 + i * 2) * S
        x = (5 + i * 4) * S
        on = i < 4
        d.rectangle([x, h - 3 * S - bh, x + 2.5 * S, h - 3 * S],
                    fill=(255, 255, 255, 235 if on else 70))
    d.text((27 * S, 1 * S), label, font=font(9), fill=(255, 255, 255, 240))
    # wifi fan
    cx, cy = 66 * S, 12 * S
    for k in range(3):
        r = (3 + k * 3) * S
        col = (255, 255, 255, 235 if k < wifi else 70)
        d.arc([cx - r, cy - r, cx + r, cy + r], 225, 315, fill=col, width=int(1.6 * S))
    d.ellipse([cx - 1.2 * S, cy - 2.2 * S, cx + 1.2 * S, cy + 0.2 * S], fill=(255, 255, 255, 235))
    text_c(d, W / 2, 1 * S, "22:47", font(9), (255, 255, 255, 245))
    # battery
    bx, by = 212 * S, 3.5 * S
    d.rounded_rectangle([bx, by, bx + 20 * S, by + 8 * S], 1.5 * S, outline=(255, 255, 255, 230),
                        width=int(1 * S))
    d.rectangle([bx + 20 * S, by + 2.5 * S, bx + 21.5 * S, by + 5.5 * S], fill=(255, 255, 255, 230))
    d.rectangle([bx + 1.8 * S, by + 1.8 * S, bx + 15 * S, by + 6.2 * S], fill=(255, 255, 255, 235))


def glass_bar(im, y0, y1, dark=True):
    if dark:
        stops = [(0, (40, 40, 44, 205)), (0.5, (18, 18, 20, 200)), (0.51, (6, 6, 8, 205)),
                 (1, (0, 0, 0, 210))]
    else:
        stops = [(0, (255, 255, 255, 90)), (0.5, (255, 255, 255, 40)), (0.51, (255, 255, 255, 20)),
                 (1, (255, 255, 255, 30))]
    paste_grad(im, (0, y0, W, y1), stops)
    d = ImageDraw.Draw(im)
    d.line([(0, y0), (W, y0)], fill=(255, 255, 255, 60), width=S)
    d.line([(0, y1 - S), (W, y1 - S)], fill=(0, 0, 0, 140), width=S)


def glossy_icon(im, x, y, size, top, bottom, glyph, selected=False):
    r = int(size * 0.22)
    if selected:
        halo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        hd = ImageDraw.Draw(halo)
        pad = 5 * S
        hd.rounded_rectangle([x - pad, y - pad, x + size + pad, y + size + pad], r + pad,
                             fill=(255, 236, 170, 170))
        im.alpha_composite(halo.filter(ImageFilter.GaussianBlur(4 * S)))
    # drop shadow
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([x, y + 2 * S, x + size, y + size + 2 * S], r,
                                         fill=(0, 0, 0, 140))
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(1.6 * S)))
    paste_grad(im, (x, y, x + size, y + size), [(0, top + (255,)), (1, bottom + (255,))], r)
    glyph(im, x, y, size)
    # gloss: elliptical highlight on top half
    gl = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(gl)
    gd.ellipse([-size * 0.35, -size * 0.62, size * 1.35, size * 0.50], fill=(255, 255, 255, 88))
    gl.putalpha(ImageChops.multiply(gl.split()[3], rmask(size, size, r)))
    im.alpha_composite(gl, (x, y))
    ImageDraw.Draw(im).rounded_rectangle([x, y, x + size, y + size], r,
                                         outline=(255, 255, 255, 70), width=max(1, S // 2))


# ---------------------------------------------------------------- glyphs
def g_release(im, x, y, s):
    d = ImageDraw.Draw(im)
    # small sun + cloud
    d.ellipse([x + s * .55, y + s * .22, x + s * .80, y + s * .47], fill=(255, 214, 92, 255))
    for (cx, cy, r) in [(.36, .62, .16), (.52, .55, .19), (.68, .64, .14)]:
        d.ellipse([x + s * (cx - r), y + s * (cy - r), x + s * (cx + r), y + s * (cy + r)],
                  fill=(255, 255, 255, 250))
    d.rectangle([x + s * .22, y + s * .62, x + s * .80, y + s * .78], fill=(255, 255, 255, 250))


def g_call(im, x, y, s):
    # classic handset: a curved band with fat ear/mouth pieces, tilted
    k = 4
    big = s * k
    hs = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hs)
    cx, cy, r = big * .70, big * .50, big * .34
    hd.arc([cx - r, cy - r, cx + r, cy + r], 130, 230, fill=(255, 255, 255, 255), width=int(big * .13))
    for ang in (130, 230):
        a = math.radians(ang)
        ex, ey = cx + math.cos(a) * (r - big * .065), cy + math.sin(a) * (r - big * .065)
        # piece perpendicular-ish to the band, pushed outward to the right
        pts = []
        for t in range(0, 360, 10):
            tt = math.radians(t)
            px = ex + math.cos(tt) * big * .14 + big * .06
            py = ey + math.sin(tt) * big * .085
            pts.append((px, py))
        hd.polygon(pts, fill=(255, 255, 255, 255))
    hs = hs.rotate(-30, resample=Image.BICUBIC).resize((s, s), Image.LANCZOS)
    im.alpha_composite(hs, (int(x - s * .10), int(y - s * .04)))


def g_wifi(on):
    def f(im, x, y, s):
        d = ImageDraw.Draw(im)
        cx, cy = x + s * .5, y + s * .72
        for k in range(3):
            r = s * (.14 + k * .14)
            d.arc([cx - r, cy - r, cx + r, cy + r], 222, 318,
                  fill=(255, 255, 255, 255 if on else 110), width=int(s * .07))
        d.ellipse([cx - s * .05, cy - s * .05, cx + s * .05, cy + s * .05],
                  fill=(255, 255, 255, 255 if on else 110))
    return f


def g_moon(im, x, y, s):
    d = ImageDraw.Draw(im)
    d.ellipse([x + s * .26, y + s * .22, x + s * .74, y + s * .70], fill=(255, 238, 196, 255))
    d.ellipse([x + s * .38, y + s * .16, x + s * .86, y + s * .64], fill=(70, 72, 120, 255))


def g_gear(im, x, y, s):
    d = ImageDraw.Draw(im)
    cx, cy = x + s / 2, y + s / 2
    for k in range(8):
        a = k * math.pi / 4
        d.line([(cx + math.cos(a) * s * .18, cy + math.sin(a) * s * .18),
                (cx + math.cos(a) * s * .33, cy + math.sin(a) * s * .33)],
               fill=(240, 240, 244, 255), width=int(s * .11))
    d.ellipse([cx - s * .24, cy - s * .24, cx + s * .24, cy + s * .24], fill=(240, 240, 244, 255))
    d.ellipse([cx - s * .10, cy - s * .10, cx + s * .10, cy + s * .10], fill=(120, 124, 132, 255))


def label(im, cx, y, s):
    d = ImageDraw.Draw(im)
    text_c(d, cx, y, s, font(9.5), (255, 255, 255, 255), shadow=(0, 0, 0, 200), sh_off=(0, 1))


# ---------------------------------------------------------------- screens
def lock_screen():
    im = dusk_wallpaper()
    status_bar(im, bg=(0, 0, 0, 0))
    glass_bar(im, 14 * S, 88 * S)
    d = ImageDraw.Draw(im)
    text_c(d, W / 2, 12 * S, "22:47", font(40, bold=False), (255, 255, 255, 255),
           shadow=(0, 0, 0, 200))
    text_c(d, W / 2, 67 * S, "10月31日 星期六", font(10.5), (255, 255, 255, 235))
    # bottom slider
    glass_bar(im, 262 * S, H)
    tx0, ty0, tx1, ty1 = 16 * S, 276 * S, 224 * S, 306 * S
    paste_grad(im, (tx0, ty0, tx1, ty1), [(0, (0, 0, 0, 200)), (1, (40, 40, 44, 200))], 9 * S)
    d.rounded_rectangle([tx0, ty0, tx1, ty1], 9 * S, outline=(255, 255, 255, 50), width=S)
    # knob
    kx0 = tx0 + 2 * S
    paste_grad(im, (kx0, ty0 + 2 * S, kx0 + 44 * S, ty1 - 2 * S),
               [(0, (252, 252, 252, 255)), (0.5, (222, 222, 226, 255)), (0.51, (196, 198, 204, 255)),
                (1, (226, 228, 232, 255))], 7 * S)
    d = ImageDraw.Draw(im)
    ax = kx0 + 22 * S
    ay = (ty0 + ty1) / 2
    d.polygon([(ax - 6 * S, ay - 7 * S), (ax + 6 * S, ay), (ax - 6 * S, ay + 7 * S)],
              fill=(130, 132, 140, 255))
    # shimmering label
    f = font(12, bold=False)
    s = "按一下解锁"
    w = d.textlength(s, font=f)
    lx = (kx0 + 44 * S + tx1) / 2 - w / 2
    txt = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(txt).text((lx, ay - 9 * S), s, font=f, fill=(255, 255, 255, 255))
    shine = vgrad(W, 1, [(0, (0, 0, 0, 255))]).resize((W, H))
    sh = Image.new("L", (W, H), 0)
    sd = ImageDraw.Draw(sh)
    for xx in range(W):
        dist = abs(xx - (lx + w * 0.62))
        sd.line([(xx, 0), (xx, H)], fill=int(110 + 145 * max(0, 1 - dist / (22 * S))))
    txt.putalpha(ImageChops.multiply(txt.split()[3], sh))
    im.alpha_composite(txt)
    return im


def home_screen():
    im = dusk_wallpaper(seed=11, moon=False)
    status_bar(im)
    sz = 52 * S
    cols = [22 * S, 94 * S, 166 * S]
    rows = [36 * S, 120 * S]
    apps = [
        ("释放", (108, 182, 236), (40, 110, 196), g_release, True),
        ("通话", (126, 216, 104), (34, 150, 46), g_call, False),
        ("联网", (172, 176, 186), (96, 100, 110), g_wifi(False), False),
        ("晚安", (104, 108, 168), (44, 46, 96), g_moon, False),
        ("设置", (172, 176, 186), (92, 96, 106), g_gear, False),
    ]
    for i, (name, top, bot, gl, sel) in enumerate(apps):
        x, y = cols[i % 3], rows[i // 3]
        glossy_icon(im, x, y, sz, top, bot, gl, selected=sel)
        label(im, x + sz / 2, y + sz + 3 * S, name if name != "联网" else "联网 · 关")
    # page dots
    d = ImageDraw.Draw(im)
    for k, a in enumerate([255, 110]):
        cx = W / 2 + (k - 0.5) * 10 * S
        d.ellipse([cx - 2.2 * S, 236 * S - 2.2 * S, cx + 2.2 * S, 236 * S + 2.2 * S],
                  fill=(255, 255, 255, a))
    # glass dock shelf
    dk = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dk)
    dd.polygon([(14 * S, 250 * S), (226 * S, 250 * S), (240 * S, 320 * S), (0, 320 * S)],
               fill=(220, 226, 240, 70))
    dd.line([(14 * S, 250 * S), (226 * S, 250 * S)], fill=(255, 255, 255, 170), width=S)
    im.alpha_composite(dk)
    d = ImageDraw.Draw(im)
    text_c(d, W / 2, 268 * S, "▲▼ 选择　● 打开", font(10, bold=False), (255, 255, 255, 220),
           shadow=(0, 0, 0, 180), sh_off=(0, 1))
    text_c(d, W / 2, 288 * S, "连按两下 ● 回到这里", font(9, bold=False), (255, 255, 255, 170),
           shadow=(0, 0, 0, 180), sh_off=(0, 1))
    return im


def call_screen(talking=True):
    im = linen()
    # portrait area: soft dusk photo-like panel
    wp = dusk_wallpaper(seed=5).resize((W, H)).crop((0, 0, W, H))
    im.alpha_composite(wp)
    status_bar(im, label=COMPANION)
    glass_bar(im, 14 * S, 78 * S)
    d = ImageDraw.Draw(im)
    text_c(d, W / 2, 18 * S, COMPANION, font(22), (255, 255, 255, 255), shadow=(0, 0, 0, 220))
    text_c(d, W / 2, 52 * S, "通话中  03:12" if talking else "正在呼叫…", font(11, bold=False),
           (220, 224, 232, 255))
    # glass button panel
    px0, py0, px1, py1 = 18 * S, 118 * S, 222 * S, 222 * S
    paste_grad(im, (px0, py0, px1, py1), [(0, (255, 255, 255, 70)), (1, (255, 255, 255, 30))], 10 * S)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([px0, py0, px1, py1], 10 * S, outline=(255, 255, 255, 110), width=S)
    cw = (px1 - px0) / 3
    for k in (1, 2):
        d.line([(px0 + cw * k, py0), (px0 + cw * k, py1)], fill=(255, 255, 255, 70), width=S)
    names = ["静音", "按住说话", "免提"]
    for k, n in enumerate(names):
        cx = px0 + cw * (k + 0.5)
        cy = (py0 + py1) / 2 - 8 * S
        if k == 1:
            if talking:
                glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
                ImageDraw.Draw(glow).rounded_rectangle([px0 + cw + 3 * S, py0 + 3 * S,
                                                        px0 + 2 * cw - 3 * S, py1 - 3 * S], 8 * S,
                                                       fill=(120, 200, 255, 120))
                im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(3 * S)))
                d = ImageDraw.Draw(im)
            # microphone
            d.rounded_rectangle([cx - 7 * S, cy - 16 * S, cx + 7 * S, cy + 6 * S], 7 * S,
                                fill=(255, 255, 255, 255))
            d.arc([cx - 12 * S, cy - 8 * S, cx + 12 * S, cy + 14 * S], 0, 180,
                  fill=(255, 255, 255, 255), width=int(2.2 * S))
            d.line([(cx, cy + 14 * S), (cx, cy + 20 * S)], fill=(255, 255, 255, 255),
                   width=int(2.2 * S))
        elif k == 0:
            d.polygon([(cx - 12 * S, cy - 5 * S), (cx - 5 * S, cy - 5 * S), (cx + 3 * S, cy - 13 * S),
                       (cx + 3 * S, cy + 13 * S), (cx - 5 * S, cy + 5 * S), (cx - 12 * S, cy + 5 * S)],
                      fill=(255, 255, 255, 235))
            d.line([(cx + 7 * S, cy - 6 * S), (cx + 14 * S, cy + 6 * S)], fill=(255, 255, 255, 235),
                   width=int(2 * S))
            d.line([(cx + 14 * S, cy - 6 * S), (cx + 7 * S, cy + 6 * S)], fill=(255, 255, 255, 235),
                   width=int(2 * S))
        else:
            d.polygon([(cx - 12 * S, cy - 5 * S), (cx - 5 * S, cy - 5 * S), (cx + 3 * S, cy - 13 * S),
                       (cx + 3 * S, cy + 13 * S), (cx - 5 * S, cy + 5 * S), (cx - 12 * S, cy + 5 * S)],
                      fill=(255, 255, 255, 235))
            for r in (6, 11):
                d.arc([cx + 1 * S - r * S, cy - r * S, cx + 1 * S + r * S, cy + r * S], -45, 45,
                      fill=(255, 255, 255, 235), width=int(1.8 * S))
        text_c(d, cx, py1 - 26 * S, n, font(9.5), (255, 255, 255, 255), shadow=(0, 0, 0, 160),
               sh_off=(0, 1))
    # bottom bar + red end button
    glass_bar(im, 250 * S, H)
    bx0, by0, bx1, by1 = 18 * S, 266 * S, 222 * S, 304 * S
    paste_grad(im, (bx0, by0, bx1, by1), [(0, (246, 112, 104, 255)), (0.5, (214, 44, 38, 255)),
                                         (0.51, (194, 22, 18, 255)), (1, (214, 50, 44, 255))], 9 * S)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([bx0, by0, bx1, by1], 9 * S, outline=(90, 0, 0, 200), width=S)
    text_c(d, W / 2, by0 + 8 * S, "结束通话", font(14), (255, 255, 255, 255), shadow=(90, 0, 0, 200))
    return im


def finish(im, name):
    small = im.resize((240, 320), Image.LANCZOS)
    small.convert("RGB").save(f"{OUT}/{name}_240x320.png")
    return small


if __name__ == "__main__":
    shots = [finish(lock_screen(), "c1_lock"), finish(home_screen(), "c2_home"),
             finish(call_screen(True), "c3_call")]
    sheet = Image.new("RGB", (3 * 480 + 4 * 24, 640 + 48), (236, 236, 238))
    for i, s in enumerate(shots):
        sheet.paste(s.resize((480, 640), Image.LANCZOS).convert("RGB"), (24 + i * (480 + 24), 24))
    sheet.save(f"{OUT}/classic_sheet.png")
    print("ok")

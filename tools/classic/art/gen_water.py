#!/usr/bin/env python3
"""Procedural lavender water surface with caustic ripples and star glints (no photo needed).
Usage: gen_water.py out.png [width height seed]"""
import sys, math
import numpy as np
from PIL import Image, ImageFilter

out = sys.argv[1]
W = int(sys.argv[2]) if len(sys.argv) > 2 else 960
H = int(sys.argv[3]) if len(sys.argv) > 3 else 1280
rng = np.random.default_rng(int(sys.argv[4]) if len(sys.argv) > 4 else 7)

y, x = np.mgrid[0:H, 0:W].astype(np.float32)
x /= W; y /= H

def noise(x, y, n=6, scale=1.0):
    v = np.zeros_like(x)
    for _ in range(n):
        a = rng.uniform(0, 2 * math.pi); f = rng.uniform(2.0, 5.0) * scale
        ph = rng.uniform(0, 2 * math.pi)
        v += np.sin((x * math.cos(a) + y * math.sin(a) * 1.6) * f * math.pi * 2 + ph)
    return v / n

# domain-warped long horizontal ripples (water seen at a low angle)
wx = x + 0.09 * noise(x, y, 5, 0.9)
wy = y + 0.05 * noise(x * 1.3, y, 5, 1.0)
r1 = np.sin((wy * 4.2 + 0.45 * np.sin(wx * 7.0 + wy * 3.0)) * math.pi * 2)
r2 = np.sin((wy * 7.0 + 0.6 * np.sin(wx * 4.0 - wy * 5.0) + 0.2 * noise(x, y, 4, 2.0)) * math.pi * 2)
caustic = np.clip(1 - np.abs(r1) * 1.1, 0, 1) ** 2 + 0.45 * np.clip(1 - np.abs(r2) * 1.3, 0, 1) ** 2
shade = 0.5 + 0.5 * noise(x * 0.7, y * 0.9, 6, 0.8)

base_dark = np.array([163, 163, 210], np.float32)
base_mid = np.array([196, 194, 232], np.float32)
hi = np.array([238, 234, 252], np.float32)
img = base_dark + (base_mid - base_dark) * shade[..., None]
img = img + (hi - img) * np.clip(caustic, 0, 1)[..., None] * 0.45
im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(W / 95))

# four-point star glints, faintly pink
glint = Image.new("L", (W, H), 0)
gp = np.array(glint, np.float32)
for _ in range(9):
    cx, cy = rng.uniform(0.08, 0.92) * W, rng.uniform(0.08, 0.92) * H
    L = rng.uniform(0.08, 0.17) * W
    dx, dy = x * W - cx, y * H - cy
    for ang in (math.pi / 4, -math.pi / 4):
        u = dx * math.cos(ang) + dy * math.sin(ang); v = -dx * math.sin(ang) + dy * math.cos(ang)
        ray = np.exp(-(v / (W / 900)) ** 2) * np.clip(1 - np.abs(u) / L, 0, 1) ** 2
        gp = np.maximum(gp, ray * 255)
    core = np.exp(-((dx ** 2 + dy ** 2) / (W / 160) ** 2)) * 255
    gp = np.maximum(gp, core)
g = np.clip(gp, 0, 255)[..., None] / 255.0
star_col = np.array([255, 226, 236], np.float32)
arr = np.array(im, np.float32)
arr = arr + (star_col - arr) * g
Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).save(out)
print("wrote", out, W, H)

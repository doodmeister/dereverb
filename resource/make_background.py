"""Generates resource/background_2x.png (editor background, 2x scale for HiDPI).

Concept: a tangle of decaying "reverb" waves on the left resolving into one clean
vocal wave. Colors follow the uidesc palette (Bg, Accent, Amber).
Run: python resource/make_background.py   (needs numpy + Pillow)
"""
from pathlib import Path

import numpy as np
from PIL import Image

W, H = 1440, 880  # 720x440 editor at 2x
rng = np.random.default_rng(7)
x = np.linspace(0.0, 1.0, W)[None, :]
y = np.arange(H, dtype=np.float64)[:, None]

BG = np.array([0x14, 0x17, 0x1C]) / 255.0
TEAL = np.array([0x2F, 0xC7, 0xB7]) / 255.0
BLUE = np.array([0x3A, 0x6F, 0xD8]) / 255.0
AMBER = np.array([0xF2, 0xB2, 0x4C]) / 255.0

light = np.zeros((H, W, 3))


def stroke(curve, width, color, gain):
    """Adds a soft line through curve (1 x W) with per-column brightness gain (1 x W)."""
    d = (y - curve) / width
    light[:] += (np.exp(-d * d) * gain)[..., None] * color


center = H * 0.56
# Envelope of a sung phrase: swells and dips instead of a flat tone.
vocal_env = 0.55 + 0.45 * np.sin(np.pi * x * 2.3 + 0.4) ** 2

# Reverb wash: many detuned, drifting waves whose amplitude and brightness die away to the right.
for i in range(34):
    freq = rng.uniform(2.0, 9.0)
    phase = rng.uniform(0, 2 * np.pi)
    drift = rng.uniform(-0.6, 0.6)
    decay = np.exp(-x * rng.uniform(2.6, 4.2))
    amp = H * rng.uniform(0.10, 0.30) * decay * vocal_env
    curve = center + amp * np.sin(2 * np.pi * freq * x + phase + drift * np.sin(3 * np.pi * x))
    t = rng.uniform()
    color = BLUE * (1 - t) + TEAL * t
    stroke(curve, 1.6, color, 0.22 * decay + 0.015)
    stroke(curve, 14.0, color, 0.025 * decay)

# Direct vocal: one clean wave, teal shading to amber, with a wide glow.
direct = center + H * 0.13 * vocal_env * np.sin(2 * np.pi * 4.0 * x + 0.8) * np.sin(2 * np.pi * 1.1 * x + 0.3)
grad = np.clip((x - 0.15) / 0.85, 0, 1)[..., None]
direct_color = TEAL * (1 - grad) + AMBER * grad  # 1 x W x 3
for width, gain in ((2.2, 1.1), (7.0, 0.30), (40.0, 0.07)):
    d = (y - direct) / width
    light += np.exp(-d * d)[..., None] * gain * direct_color

# Soft top-left glow and vignette.
glow = np.exp(-(((x - 0.15) / 0.5) ** 2) - ((y / H - 0.05) / 0.45) ** 2)
light += glow[..., None] * 0.06 * TEAL
vignette = 1.0 - 0.45 * (((x - 0.5) * 1.6) ** 2 + ((y / H - 0.5) * 1.4) ** 2)

img = BG + (1.0 - np.exp(-light)) * 0.85  # soft additive tone map
img *= np.clip(vignette, 0.35, 1.0)[..., None]
out = Path(__file__).with_name("background_2x.png")
Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).save(out, optimize=True)
print(f"wrote {out}")

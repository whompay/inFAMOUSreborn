"""v193: build assets/kbm_icons.bin (keyboard/mouse button glyphs for the in-game prompt font).
Inputs: assets/font_icons_l0.bin = raw level 0 of font_icons_00.dds (512x256 DXT5, oid 120625 in
lang_english.xpp; dump with tools/dump_icon_tex.py). The glyph cells (font_icons table) are redrawn by
kbm_glyphs_draw.py (key caps / mouse icons, bindings of cellPad.c v150); logo and '?' stay. GPU textures are stored
bottom-up, so the drawn (upright) image is flipped before DXT5 encoding.
Output: "IFKBMIC1", u32 w, h, fmt(0x88), nbytes, tex_csum(original) ; original level 0 ; KBM level 0.
Needs numpy + Pillow + DejaVuSans-Bold.ttf (falls back to Arial Bold on Windows; set KBM_FONT to use another .ttf).
usage (from infamous-recomp/tools): python make_kbm_icons.py"""
import os, sys, struct, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kbm_glyphs_dxt import dxt5_decode, dxt5_encode, tex_csum
from kbm_glyphs_draw import build
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
orig = open(os.path.join(ROOT, 'assets', 'font_icons_l0.bin'), 'rb').read()[:131072]
vis = dxt5_decode(orig, 512, 256)[::-1]
kbm = build(vis)
enc = dxt5_encode(np.ascontiguousarray(kbm[::-1]))
out = os.path.join(ROOT, 'assets', 'kbm_icons.bin')
open(out, 'wb').write(b'IFKBMIC1' + struct.pack('<5I', 512, 256, 0x88, 131072, tex_csum(orig, 131072)) + orig + enc)
print('wrote', out)

#!/usr/bin/env python3
"""Asset pipeline: regenerates every game asset from the .sb3.

Usage: tools/build_assets.py <project.sb3> <outdir>

Writes
  <outdir>/pc/   full-resolution PNGs (bled, non-premultiplied), original audio files, fonts
  <outdir>/3ds/  power-of-two PNGs sized for the 400x240 top screen, original audio files, fonts
  <outdir>/cia/  banner.png (256x128), icon.png (48x48), banner.wav (jingle)
  src/common/costumes_gen.h   costume table (svg size, rotation centre, crop, per-platform scale)
Mip levels are generated at load time by the game (alpha-weighted box filter on the bled base level).
"""
import io, json, math, os, re, shutil, subprocess, sys, tempfile, zipfile
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "tools", "fonts")

PC_STAGE_SCALE = 6.0          # 2160 px / 360 stage px (4K)
PC_MAX_TEX = 4096
DS_STAGE_SCALE = 240.0 / 360.0
DS_MAX_TEX = 1024
WORLD_MAX_DRAW = 3.0          # objects are deleted at Z<100 => 300%; bigger only for a frame

# (id, sprite, costume, kind, extra)
# kind: world (projected objects, size clamp), button (hover 120%), ui (100%), dot (10%), banner (not in game)
COSTUMES = [
    ("STAR01", "Stars", "0.1", "world", {}),
    ("STAR1", "Stars", "1", "world", {}),
    ("STAR001", "Stars", "0.01", "world", {}),
    ("EARTH", "Stars", "Earth", "world", {}),
    ("PLANET2", "Stars", "Planet2", "world", {}),
    ("POWERUP", "Stars", "Power up", "world", {"layered": True}),
    ("BULLET", "Stars", "Bullet", "world", {"layered": True}),
    ("TITLE", "Title screen", "Title", "ui", {}),
    ("DESCRIPTION", "Title screen", "Description", "ui", {}),
    ("CHANGELOG", "Title screen", "Changelog", "ui", {}),
    ("PLAY", "Title screen", "Play", "button", {}),
    ("BACK", "Title screen", "Back", "button", {}),
    ("SELECTMODE", "Title screen", "Select mode", "button", {}),
    ("HARDMODE", "Title screen", "Hard mode", "button", {}),
    ("WHATSNEW", "Title screen", "What's new?", "button", {}),
    ("NORMALMODE", "Title screen", "Normal mode", "button", {}),
    ("SELECTED", "Title screen", "[SELECTED]", "button", {}),
    ("DESC_NORMAL", "Title screen", "normal", "ui", {}),
    ("DESC_HARD", "Title screen", "hard", "ui", {}),
    ("TOGGLEMUSIC", "Title screen", "Toggle Music", "button", {"hues": [0, 50]}),
    ("PAUSE_PANEL", "Pause Screen", "costume1", "ui", {"crop_stage": (330, 200)}),
    ("PAUSE_BUTTON", "Pause Screen", "costume2", "ui", {}),
    ("WR", "You beat world record", "costume1", "ui", {}),
    ("DOT", "Cursor", "Dot", "dot", {}),
]

SOUNDS = {  # target name -> (sprite, sound name)
    "intro.mp3": ("Stage", "Intro"),
    "main.mp3": ("Stage", "Main"),
    "options_intro.mp3": ("Thumbnail", "Options intro"),
    "options_select.mp3": ("Thumbnail", "Options select"),
    "easter_egg.mp3": ("Stage", "Easter egg"),
    "lose.mp3": ("Stage", "Lose"),
    "pop_stage.wav": ("Stage", "Pop"),
    "pop.wav": ("Stars", "pop"),
    "crunch.wav": ("Stars", "Crunch"),
    "hit.wav": ("Stars", "Hit"),
}


def fontconfig_env(tmp):
    conf = os.path.join(tmp, "fonts.conf")
    with open(conf, "w") as f:
        f.write('<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd"><fontconfig>'
                '<include ignore_missing="yes">/etc/fonts/fonts.conf</include>'
                f'<dir>{FONTS}</dir><cachedir>{tmp}/fccache</cachedir></fontconfig>')
    env = dict(os.environ)
    env["FONTCONFIG_FILE"] = conf
    return env


def svg_size(svg):
    m = re.search(r'<svg[^>]*?\swidth="([\d.]+)"[^>]*?\sheight="([\d.]+)"', svg)
    return float(m.group(1)), float(m.group(2))


def render_svg(svg, scale, env, tmp):
    svg = svg.replace('font-family="Pixel"', 'font-family="Grand9K Pixel"')
    p = os.path.join(tmp, "in.svg")
    with open(p, "w") as f:
        f.write(svg)
    out = subprocess.run(["rsvg-convert", "-z", "%.6f" % scale, "-f", "png", p],
                         check=True, capture_output=True, env=env).stdout
    return np.asarray(Image.open(io.BytesIO(out)).convert("RGBA")).astype(np.float32) / 255.0


def bleed(img):
    """Push-pull fill of RGB under fully transparent pixels (no dark fringes with linear filtering)."""
    h, w = img.shape[:2]
    a = img[..., 3:4]
    levels = [(img[..., :3] * a, a.copy())]
    while max(levels[-1][1].shape[:2]) > 1:
        p, wt = levels[-1]
        hh, ww = p.shape[:2]
        if hh % 2: p = np.concatenate([p, p[-1:]], 0); wt = np.concatenate([wt, wt[-1:] * 0], 0)
        if ww % 2: p = np.concatenate([p, p[:, -1:]], 1); wt = np.concatenate([wt, wt[:, -1:] * 0], 1)
        p = p[0::2, 0::2] + p[1::2, 0::2] + p[0::2, 1::2] + p[1::2, 1::2]
        wt = wt[0::2, 0::2] + wt[1::2, 0::2] + wt[0::2, 1::2] + wt[1::2, 1::2]
        levels.append((p, wt))
    col = None
    for p, wt in reversed(levels):
        c = np.where(wt > 1e-6, p / np.maximum(wt, 1e-6), 0)
        if col is not None:
            up = np.repeat(np.repeat(col, 2, 0), 2, 1)[:c.shape[0], :c.shape[1]]
            c = np.where(wt > 1e-6, c, up)
        elif not (wt > 1e-6).any():
            c = np.zeros_like(c)
        col = c
    out = img.copy()
    mask = img[..., 3] <= 0
    out[..., :3][mask] = col[mask]
    return out


def rgb_to_hsv(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx = np.max(rgb, -1); mn = np.min(rgb, -1); d = mx - mn
    h = np.zeros_like(mx)
    nz = d > 1e-9
    rc = np.where(nz, (mx - r) / np.where(nz, d, 1), 0)
    gc = np.where(nz, (mx - g) / np.where(nz, d, 1), 0)
    bc = np.where(nz, (mx - b) / np.where(nz, d, 1), 0)
    h = np.where(r == mx, bc - gc, np.where(g == mx, 2 + rc - bc, 4 + gc - rc))
    h = np.where(nz, (h / 6.0) % 1.0, 0)
    s = np.where(mx > 1e-9, d / np.where(mx > 1e-9, mx, 1), 0)
    return np.stack([h, s, mx], -1)


def hsv_to_rgb(hsv):
    h, s, v = hsv[..., 0], hsv[..., 1], hsv[..., 2]
    i = np.floor(h * 6) % 6; f = h * 6 - np.floor(h * 6)
    p = v * (1 - s); q = v * (1 - s * f); t = v * (1 - s * (1 - f))
    conds = [i == k for k in range(6)]
    r = np.select(conds, [v, q, p, p, t, v]); g = np.select(conds, [t, v, v, q, p, p]); b = np.select(conds, [p, p, t, v, v, q])
    return np.stack([r, g, b], -1)


def scratch_color_effect(img, effect):
    """Scratch's colour effect (hue shift effect/200 of a turn, with min lightness/saturation rules)."""
    if effect == 0:
        return img
    hsv = rgb_to_hsv(img[..., :3])
    low_v = hsv[..., 2] < 0.11 / 2
    low_s = (~low_v) & (hsv[..., 1] < 0.09)
    hsv[..., 0] = np.where(low_v, 0, np.where(low_s, 0, hsv[..., 0]))
    hsv[..., 1] = np.where(low_v, 1, np.where(low_s, 0.09, hsv[..., 1]))
    hsv[..., 2] = np.where(low_v, 0.11 / 2, hsv[..., 2])
    hsv[..., 0] = (hsv[..., 0] + effect / 200.0) % 1.0
    out = img.copy()
    out[..., :3] = hsv_to_rgb(hsv)
    return out


def save_png(img, path):
    Image.fromarray(np.clip(img * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(path, optimize=False, compress_level=6)


def pot(n):
    p = 8
    while p < n:
        p *= 2
    return p


def main():
    sb3, outdir = sys.argv[1], sys.argv[2]
    z = zipfile.ZipFile(sb3)
    proj = json.loads(z.read("project.json"))
    targets = {t["name"]: t for t in proj["targets"]}
    for d in ("pc", "3ds", "cia"):
        os.makedirs(os.path.join(outdir, d), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="sd3d_assets_")
    env = fontconfig_env(tmp)
    rows = []
    ds_bytes = 0
    for cid, sprite, cname, kind, extra in COSTUMES:
        c = next(c for c in targets[sprite]["costumes"] if c["name"] == cname)
        svg = z.read(c["md5ext"]).decode("utf-8")
        w, h = svg_size(svg)
        rcx, rcy = c["rotationCenterX"], c["rotationCenterY"]
        if kind == "world":
            clamp_max = min(720.0 / w, 540.0 / h)
            maxdraw = min(clamp_max, WORLD_MAX_DRAW)
            maxdraw = max(maxdraw, 1.0)
        elif kind == "button":
            maxdraw = 1.2
        elif kind == "dot":
            maxdraw = 0.1
        else:
            maxdraw = 1.0
        # crop (costume units)
        cx0, cy0, cw, ch = 0.0, 0.0, w, h
        if "crop_stage" in extra:
            hx, hy = extra["crop_stage"]
            cx0, cy0 = max(0.0, rcx - hx), max(0.0, rcy - hy)
            cw, ch = min(w, rcx + hx) - cx0, min(h, rcy + hy) - cy0
        name = cid.lower()
        out = {}
        for plat, stage_scale, maxtex in (("pc", PC_STAGE_SCALE, PC_MAX_TEX), ("3ds", DS_STAGE_SCALE, DS_MAX_TEX)):
            s = maxdraw * stage_scale
            s = min(s, (maxtex - 2) / max(cw, ch))
            if plat == "pc":
                s = max(s, 4.0 / max(cw, ch) * 4)
            else:
                s = max(s, 2.0 / min(cw, ch))  # tiny stars: keep a few texels
            full = render_svg(svg, s, env, tmp)
            # crop region in pixels (rsvg output size = ceil(w*s))
            px0, py0 = int(math.floor(cx0 * s)), int(math.floor(cy0 * s))
            pw, ph = max(1, int(math.ceil(cw * s))), max(1, int(math.ceil(ch * s)))
            img = np.zeros((ph, pw, 4), np.float32)
            sub = full[py0:py0 + ph, px0:px0 + pw]
            img[:sub.shape[0], :sub.shape[1]] = sub
            # exact scale actually used for this crop: we keep s; texel (0,0) == costume (px0/s, py0/s)
            ox, oy = px0 / s, py0 / s
            variants = []
            if "hues" in extra:
                for e in extra["hues"]:
                    variants.append(("_h%d" % e, scratch_color_effect(img, e)))
            elif "layered" in extra and plat == "3ds":
                base = img.copy(); base[..., :3] = 1.0
                mask = img.copy(); mask[..., :3] = 1.0
                mask[..., 3] = img[..., 3] * np.clip(1.0 - img[..., 2], 0, 1)  # yellow amount (W->Y line, blue channel)
                variants.append(("", base))
                variants.append(("_mask", mask))
            else:
                variants.append(("", img))
            for suf, v in variants:
                if plat == "3ds":
                    tw, th = pot(pw), pot(ph)
                    canvas = np.zeros((th, tw, 4), np.float32)
                    canvas[:ph, :pw] = v
                    v = canvas
                    lvl, b = tw * th * 4, 0
                    while True:
                        b += lvl
                        if tw <= 8 or th <= 8:
                            break
                        tw //= 2; th //= 2; lvl = tw * th * 4
                    ds_bytes += b
                save_png(bleed(v), os.path.join(outdir, plat, name + suf + ".png"))
            out[plat] = (s, pw, ph, ox, oy)
        rows.append((cid, name, w, h, rcx, rcy, out, kind, extra))
        print("%-12s svg %7.2fx%-7.2f pc %4dx%-4d (x%.2f)  3ds %4dx%-4d (x%.3f)" % (
            cid, w, h, out["pc"][1], out["pc"][2], out["pc"][0], out["3ds"][1], out["3ds"][2], out["3ds"][0]))
    print("3DS texture memory incl. mips + POT padding: %.2f MB" % (ds_bytes / 1048576.0))

    # sounds: original files, unmodified
    for fname, (sprite, sname) in SOUNDS.items():
        snd = next(s for s in targets[sprite]["sounds"] if s["name"] == sname)
        data = z.read(snd["md5ext"])
        for plat in ("pc", "3ds"):
            with open(os.path.join(outdir, plat, fname), "wb") as f:
                f.write(data)
    for plat, fonts in (("pc", ["LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf", "DejaVuSans.ttf", "Grand9K-Pixel.ttf"]),
                        ("3ds", ["Grand9K-Pixel.ttf", "LiberationSans-Bold.ttf", "DejaVuSans.ttf"])):
        for fn in fonts:
            shutil.copy(os.path.join(FONTS, fn), os.path.join(outdir, plat, fn))

    # header
    hdr = ["/* Generated by tools/build_assets.py from the .sb3 - do not edit. */",
           "#ifndef COSTUMES_GEN_H", "#define COSTUMES_GEN_H", "", "enum {"]
    hdr += ["    CO_%s," % r[0] for r in rows] + ["    CO_COUNT", "};", ""]
    hdr += ["typedef struct {", "    const char *file;", "    float w, h;      /* svg size (Scratch costume size) */",
            "    float rcx, rcy;  /* rotation centre, costume px from top-left */",
            "    float pc_scale, pc_ox, pc_oy;   /* texels per costume px; costume coords of texel (0,0) */",
            "    int pc_w, pc_h;", "    float ds_scale, ds_ox, ds_oy;", "    int ds_w, ds_h;",
            "    int layered;     /* 3DS: <file>.png white base + <file>_mask.png hue layer */",
            "    int hues;        /* number of baked hue variants (<file>_h<effect>.png) */",
            "} CostumeInfo;", "", "static const CostumeInfo costume_info[CO_COUNT] = {"]
    for cid, name, w, h, rcx, rcy, out, kind, extra in rows:
        pc, ds = out["pc"], out["3ds"]
        hdr.append('    {"%s", %.5ff, %.5ff, %.5ff, %.5ff, %.6ff, %.4ff, %.4ff, %d, %d, %.6ff, %.4ff, %.4ff, %d, %d, %d, %d},' % (
            name, w, h, rcx, rcy, pc[0], pc[3], pc[4], pc[1], pc[2], ds[0], ds[3], ds[4], ds[1], ds[2],
            1 if extra.get("layered") else 0, len(extra.get("hues", []))))
    hdr += ["};", "", "#endif", ""]
    with open(os.path.join(ROOT, "src", "common", "costumes_gen.h"), "w") as f:
        f.write("\n".join(hdr))

    # CIA art: banner 256x128 from the Thumbnail costume, icon 48x48, jingle from Intro
    th = next(c for c in targets["Thumbnail"]["costumes"] if c["name"] == "Thumbnail")
    tsvg = z.read(th["md5ext"]).decode("utf-8")
    tw_, th_ = svg_size(tsvg)
    big = render_svg(tsvg, 1024.0 / th_, env, tmp)
    timg = Image.fromarray((big * 255).astype(np.uint8), "RGBA")
    banner = Image.new("RGBA", (256, 128), (0, 0, 0, 255))
    # crop to 2:1 around the logo (keep full width of the art, trim top/bottom evenly)
    W, H = timg.size
    cropH = int(W / 2.0 * 1.0)
    band = timg.crop((0, (H - cropH) // 2, W, (H - cropH) // 2 + cropH)) if cropH < H else timg
    fit = timg.resize((int(128 * W / H), 128), Image.LANCZOS)
    banner.alpha_composite(fit, ((256 - fit.size[0]) // 2, 0))
    banner.convert("RGB").save(os.path.join(outdir, "cia", "banner.png"))
    # icon: tight square crop on the big "3D" and the planet next to it (readable at 48x48)
    k = H / th_
    x0, y0, side = 290, 180, 180
    icon = timg.crop((int(x0 * k), int(y0 * k), int((x0 + side) * k), int((y0 + side) * k))).resize((48, 48), Image.LANCZOS)
    bg = Image.new("RGBA", (48, 48), (0, 0, 0, 255)); bg.alpha_composite(icon)
    bg.convert("RGB").save(os.path.join(outdir, "cia", "icon.png"))
    intro = os.path.join(tmp, "intro.mp3")
    with open(intro, "wb") as f:
        f.write(z.read(next(s for s in targets["Stage"]["sounds"] if s["name"] == "Intro")["md5ext"]))
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", intro, "-t", "2.9", "-af", "afade=t=out:st=2.0:d=0.9",
                    "-ac", "2", "-ar", "32000", "-c:a", "pcm_s16le", os.path.join(outdir, "cia", "banner.wav")], check=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print("assets written to", outdir)


if __name__ == "__main__":
    main()

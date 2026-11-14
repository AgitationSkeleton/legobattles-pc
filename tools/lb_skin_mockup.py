"""docs/493: mockups of a LEGO Battles skin for the port's ImGui settings panel.

Every element is cut from the cartridge's own art (tools/lb_skin_dump.py decodes it):
  UI_FE (4bpp, 16 banks = colour variants): studded brick bars, arrow buttons
  FramedBG (8bpp): the blue vignette and the rounded inset panel
  Iconic_FreeplaySettings (8bpp): rounded tiles, orange = selected
  UI_MainCastle (4bpp): the orange tick boxes
  Font/EN/font.NFTR: the game's 10x13 1bpp font
Drawn at the DS's own pixel size, then shown at 2x (nearest), as the port
would draw it at UI scale 2.
"""
import json, os
from PIL import Image, ImageDraw
import lb_skin_dump as dump

OUT = dump.OUT
S = 2  # display scale


def sheet(name, bank):
    p, *_ = dump.sheet(name, bank)
    return Image.open(p).convert("RGBA")


FONT = Image.open(os.path.join(OUT, "font.png"))
FJ = json.load(open(os.path.join(OUT, "font.json")))
CW, CH = FJ["cw"], FJ["ch"]
CMAP = {int(k): v for k, v in FJ["cmap"].items()}


def text_w(s):
    w = 0
    for ch in s:
        g = CMAP.get(ord(ch))
        w += FJ["widths"][g][2] if g is not None else 5
    return w


def draw_text(im, x, y, s, fill=(232, 236, 255), shadow=(16, 24, 72), center_w=None):
    if center_w:
        x += (center_w - text_w(s)) // 2
    for ch in s:
        g = CMAP.get(ord(ch))
        if g is None:
            x += 5
            continue
        left, gw, adv = FJ["widths"][g]
        gx, gy = (g % 32) * CW, (g // 32) * CH
        for yy in range(CH):
            for xx in range(CW):
                if FONT.getpixel((gx + xx, gy + yy)):
                    for dx, dy, c in ((1, 1, shadow), (0, 0, fill)):
                        px, py = x + left + xx + dx, y + yy + dy
                        if 0 <= px < im.width and 0 <= py < im.height:
                            if c is fill or im.getpixel((px, py))[:3] != fill:
                                im.putpixel((px, py), c + (255,))
        x += adv
    return x


def nine(src, box, border, w, h):
    """9-slice: box = (x0,y0,x1,y1) in src, border px kept, middle stretched."""
    x0, y0, x1, y1 = box
    b = border
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    xs = [(x0, x0 + b, 0, b), (x0 + b, x1 - b, b, w - b), (x1 - b, x1, w - b, w)]
    ys = [(y0, y0 + b, 0, b), (y0 + b, y1 - b, b, h - b), (y1 - b, y1, h - b, h)]
    for sx0, sx1, dx0, dx1 in xs:
        for sy0, sy1, dy0, dy1 in ys:
            if dx1 <= dx0 or dy1 <= dy0:
                continue
            piece = src.crop((sx0, sy0, sx1, sy1)).resize((dx1 - dx0, dy1 - dy0), Image.NEAREST)
            out.paste(piece, (dx0, dy0), piece)
    return out


def brick_bar(fe, w, dark=False, cap=False):
    """A studded brick bar w px wide (24 high): studs repeat every 16 px."""
    y0 = 232 if dark else 208
    bar = Image.new("RGBA", (w, 24), (0, 0, 0, 0))
    x = 0
    if cap:
        c = fe.crop((0, y0, 24, y0 + 24))
        bar.paste(c, (0, 0), c)
        x = 24
    # left edge of the body (has the highlight), then repeat studs, then right edge
    left = fe.crop((24, y0, 32, y0 + 24))
    bar.paste(left, (x, 0), left)
    x += 8
    body = fe.crop((32, y0, 48, y0 + 24))
    while x < w - 8:
        bar.paste(body, (x, 0), body)
        x += 16
    right = fe.crop((88, y0, 96, y0 + 24))
    bar.paste(right, (w - 8, 0), right)
    return bar


def tick_box(castle, on):
    if on:
        return castle.crop((0, 240, 16, 256))
    t = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    d = ImageDraw.Draw(t)
    d.rectangle((1, 1, 14, 14), fill=(10, 18, 64, 255), outline=(70, 110, 220, 255))
    d.line((2, 14, 14, 14), fill=(120, 160, 255, 255))
    return t


def slider(w, frac, fe):
    s = Image.new("RGBA", (w, 14), (0, 0, 0, 0))
    d = ImageDraw.Draw(s)
    d.rounded_rectangle((0, 3, w - 1, 10), 3, fill=(6, 12, 48, 255), outline=(60, 96, 210, 255))
    fx = int((w - 20) * frac)
    d.rounded_rectangle((1, 4, fx + 8, 9), 2, fill=(255, 176, 32, 255))
    grab = fe.crop((32, 208, 48, 224))   # one stud of the bar
    s.paste(grab, (fx, -1), grab)
    return s


def settings_mockup(bank, fname, title="Settings"):
    fe = sheet("UI_FE", bank)
    castle = sheet("UI_MainCastle", 0)
    framed = Image.open(os.path.join(OUT, "FramedBG_b0.png")).convert("RGBA")
    iconic = Image.open(os.path.join(OUT, "Iconic_FreeplaySettings_b0.png")).convert("RGBA")
    W, H = 440, 300
    im = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    # window background: the vignette, 9-sliced so its edges stay dark
    bg = nine(framed, (0, 0, 256, 192), 40, W, H)
    im.paste(bg, (0, 0), bg)
    # title bar: a studded brick bar, team-coloured by bank
    tb = brick_bar(fe, W, cap=False)
    im.paste(tb, (0, 0), tb)
    draw_text(im, 0, 7, title, center_w=W)
    # tabs: brick bars; the open one light, the others dark
    tabs = ["General", "Display", "Team Builder", "Balance"]
    x = 6
    for i, t in enumerate(tabs):
        w = text_w(t) + 22
        b = brick_bar(fe, w, dark=(i != 2))
        im.paste(b, (x, 30), b)
        draw_text(im, x, 37, t, center_w=w,
                  fill=(232, 236, 255) if i == 2 else (150, 170, 220))
        x += w + 3
    # content panel: the rounded inset from FramedBG (x 45..211, y 37..153)
    panel = nine(framed, (44, 36, 212, 154), 10, W - 16, H - 66)
    im.paste(panel, (8, 58), panel)
    y = 68
    rows = [("check", "Team-coloured unit cards", True),
            ("check", "Custom team colour", True),
            ("swatch", "Colour", None),
            ("slider", "Fast-forward speed", 0.4),
            ("check", "Remember music and effects volume", False),
            ("combo", "Teal team colour", "Red/Dark Blue (Unused)")]
    for kind, label, val in rows:
        if kind == "check":
            t = tick_box(castle, val)
            im.paste(t, (20, y), t)
            draw_text(im, 42, y + 2, label)
            y += 22
        elif kind == "slider":
            draw_text(im, 20, y + 1, label)
            s = slider(170, val, fe)
            im.paste(s, (226, y + 1), s)
            draw_text(im, 402, y + 1, "4x")
            y += 22
        elif kind == "combo":
            draw_text(im, 20, y + 4, label)
            d = ImageDraw.Draw(im)
            d.rounded_rectangle((226, y, 415, y + 19), 4, fill=(6, 12, 48, 255), outline=(60, 96, 210, 255))
            d.line((230, y + 18, 411, y + 18), fill=(110, 150, 255, 255))
            draw_text(im, 232, y + 4, val)
            a = fe.crop((32, 186, 64, 208)).resize((20, 14), Image.NEAREST)   # the game's down arrow
            im.paste(a, (394, y + 3), a)
            y += 26
        elif kind == "swatch":
            draw_text(im, 42, y + 3, label)
            sw = [(140, 82, 41), (214, 54, 46), (40, 96, 214), (60, 170, 70), (238, 196, 30), (150, 70, 200)]
            d = ImageDraw.Draw(im)
            for i, c in enumerate(sw):
                x = 110 + i * 24
                # the Iconic tiles' rounded frame: orange when selected, blue otherwise
                ring = (255, 168, 24) if i == 0 else (40, 90, 220)
                d.rounded_rectangle((x, y, x + 19, y + 19), 4, fill=(6, 12, 48, 255), outline=ring + (255,), width=2)
                d.rectangle((x + 4, y + 4, x + 15, y + 15), fill=c + (255,))
            d.rounded_rectangle((110 + 6 * 24, y, 110 + 6 * 24 + 19, y + 19), 4, fill=(6, 12, 48, 255),
                                outline=(40, 90, 220, 255), width=2)
            draw_text(im, 110 + 6 * 24 + 6, y + 3, "?")
            y += 26
    # bottom buttons: brick bars like the game's "Main Menu" / "Stats"
    for i, t in enumerate(["Back", "Apply"]):
        w = 96
        b = brick_bar(fe, w, cap=(i == 0))
        x = 12 if i == 0 else W - w - 12
        im.paste(b, (x, H - 30), b)
        draw_text(im, x + (16 if i == 0 else 0), H - 23, t, center_w=w - (16 if i == 0 else 0))
    big = im.resize((W * S, H * S), Image.NEAREST)
    big.save(os.path.join(OUT, fname))
    return fname


def widget_kit(fname):
    """Every widget state on one sheet, in three team-colour banks."""
    castle = sheet("UI_MainCastle", 0)
    W, H = 460, 200
    im = Image.new("RGBA", (W, H), (12, 14, 40, 255))
    for row, (bank, name) in enumerate(((1, "Blue"), (5, "Red"), (8, "Orange"))):
        fe = sheet("UI_FE", bank)
        y = 10 + row * 62
        draw_text(im, 8, y + 6, name)
        b = brick_bar(fe, 90)
        im.paste(b, (70, y), b); draw_text(im, 70, y + 7, "Button", center_w=90)
        b = brick_bar(fe, 90, dark=True)
        im.paste(b, (166, y), b); draw_text(im, 166, y + 7, "Pressed", center_w=90, fill=(150, 170, 220))
        b = brick_bar(fe, 90, cap=True)
        im.paste(b, (262, y), b); draw_text(im, 286, y + 7, "Back", center_w=66)
        up = fe.crop((0, 184, 32, 208)); dn = fe.crop((32, 184, 64, 208))
        im.paste(up, (360, y), up); im.paste(dn, (396, y), dn)
        s = slider(150, 0.3 + 0.2 * row, fe)
        im.paste(s, (70, y + 30), s)
        for i, on in enumerate((True, False)):
            t = tick_box(castle, on)
            im.paste(t, (236 + i * 22, y + 29), t)
    im.resize((W * S, H * S), Image.NEAREST).save(os.path.join(OUT, fname))
    return fname


if __name__ == "__main__":
    print(settings_mockup(1, "mock_settings_blue.png"))
    print(settings_mockup(5, "mock_settings_red.png"))
    print(widget_kit("mock_widgets.png"))

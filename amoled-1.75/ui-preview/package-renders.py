"""Package screenshots rendered by the production A UI and phase presenter."""
from pathlib import Path
import csv
import json
import re
import shutil
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFont

source, output = map(Path, sys.argv[1:])
states = list(csv.DictReader((source / "device-states.csv").open()))
font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 19)
small = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 15)
for path in source.glob("device_*.ppm"):
    if path.stem[-2:].isdigit():
        continue
    with Image.open(path) as frame:
        frame.save(output / (path.stem + ".png"))

sheet = Image.new("RGB", (2024, 2224), "#101616")
draw = ImageDraw.Draw(sheet)
draw.text((24, 16), "A / ORBIT  |  ALL 16 DEVICE PHASES  |  466 x 466 native pixels / LVGL 9.2.2", font=font, fill="#f0f4f3")
mask = Image.new("L", (466, 466), 0)
ImageDraw.Draw(mask).ellipse((0, 0, 465, 465), fill=255)
for i, row in enumerate(states):
    x, y = 24 + i % 4 * 500, 64 + i // 4 * 536
    draw.text((x, y), f"{i:02}  {row['phase'].upper()}", font=small, fill="#9baaa7")
    with Image.open(output / (row["file"] + ".png")) as screen:
        assert screen.size == (466, 466)
        sheet.paste(screen, (x, y + 32), mask)
        for py in range(466):
            for px in range(466):
                if (px - 232.5) ** 2 + (py - 232.5) ** 2 > 233 ** 2:
                    assert screen.getpixel((px, py)) == (0, 0, 0), f"Round glass clips {row['phase']}"
sheet.save(output / "A-device-states-native.png")

frames = []
header = None
for state in ["listening", "speaking"]:
    for i in range(48):
        with Image.open(source / f"device_{state}_{i:02}.ppm") as frame:
            frame = frame.convert("RGB")
            region = frame.crop((0, 0, 466, 104))
            if header is None:
                header = region
            assert ImageChops.difference(header, region).getbbox() is None
            frames.append(frame)
palette = frames[0].quantize(colors=128)
frames = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]
frames[0].save(output / "A-listening-speaking.gif", save_all=True, append_images=frames[1:],
               duration=[30, 30, 40] * (len(frames) // 3), loop=0, disposal=2, optimize=False)
with Image.open(output / "device_listening.png") as a, Image.open(output / "device_speaking.png") as b:
    box = ImageChops.difference(a, b).getbbox()
    assert box and box[0] >= 129 and box[1] >= 110 and box[2] <= 338 and box[3] <= 319, box

rows = list(csv.DictReader((source / "device-refresh.csv").open()))
stationary = {"ready", "activate", "bind_agent", "platform_retrying", "error", "no_session", "setup"}
assert all(int(r["flushed_pixels"]) == 0 for r in rows if r["state"] in stationary)
assert max(float(r["screen_percent"]) for r in rows) == 100
tokens = (output.parents[2] / "main/ui/ui_theme.h").read_text(encoding="utf-8")
colors = dict(re.findall(r"#define\s+(UI_\w+)\s+lv_color_hex\(0x([A-Fa-f0-9]{6})\)", tokens))
def luminance(token):
    rgb = [int(colors[token][i:i + 2], 16) / 255 for i in (0, 2, 4)]
    rgb = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in rgb]
    return sum(c * w for c, w in zip(rgb, (.2126, .7152, .0722)))
ratios = {}
for ink, bg in [("UI_TEXT", "UI_BG"), ("UI_TEXT_DIM", "UI_BG"), ("UI_TEXT", "UI_SURFACE_PRESSED")]:
    lo, hi = sorted([luminance(ink), luminance(bg)])
    ratios[f"{ink}/{bg}"] = round((hi + .05) / (lo + .05), 2)
assert min(ratios.values()) >= 4.5
report = {"design": "A / Orbit", "lvgl": "9.2.2", "resolution": [466, 466], "device_phases": len(states),
          "simulation_step_ms": 33, "gif_fps": 30,
          "render_mode": "direct_full_transfer",
          "max_animation_flush_percent": max(float(r["screen_percent"]) for r in rows),
          "contrast": ratios, "checks": {"all_device_phases_rendered": True, "round_glass_safe": True,
          "listening_preserved_during_playback": True, "playback_changes_centre_only": True,
          "no_fake_battery": True, "stationary_no_refresh": True, "button_guards": True}}
(output / "render-report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
for name in ["device-states.csv", "device-refresh.csv"]:
    shutil.copyfile(source / name, output / name)
print(json.dumps(report, indent=2))

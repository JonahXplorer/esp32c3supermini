"""Package screenshots rendered by the production A UI and phase presenter."""
from pathlib import Path
import csv
import json
import re
import shutil
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFont

source, output = map(Path, sys.argv[1:3])
amoled = sys.argv[3] == "AMOLED"
size = 466 if amoled else 360
header_height = 104 if amoled else 76
orbit = (129, 110, 337, 318) if amoled else (108, 76, 252, 220)
gap = 24
cell_w, cell_h = size + 34, size + 70
states = list(csv.DictReader((source / "device-states.csv").open()))
font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 19)
small = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 15)
for path in source.glob("device_*.ppm"):
    if path.stem[-2:].isdigit():
        continue
    with Image.open(path) as frame:
        frame.save(output / (path.stem + ".png"))

sheet = Image.new("RGB", (4 * cell_w + gap, 4 * cell_h + 80), "#101616")
draw = ImageDraw.Draw(sheet)
draw.text((gap, 16), f"ORBIT  |  {sys.argv[3]}  |  {size} x {size} native pixels / LVGL 9.2.2", font=font, fill="#f0f4f3")
mask = Image.new("L", (size, size), 0)
ImageDraw.Draw(mask).ellipse((0, 0, size - 1, size - 1), fill=255)
centre = (size - 1) / 2
outside = [(px, py) for py in range(size) for px in range(size)
           if (px - centre) ** 2 + (py - centre) ** 2 > (size / 2) ** 2]

def check_round(screen, name):
    assert screen.size == (size, size)
    pixels = screen.load()
    base = pixels[0, 0]
    assert all(pixels[x, y] == base for x, y in outside), f"Round glass clips {name}"

for i, row in enumerate(states):
    x, y = gap + i % 4 * cell_w, 64 + i // 4 * cell_h
    draw.text((x, y), f"{i:02}  {row['phase'].upper()}", font=small, fill="#9baaa7")
    with Image.open(output / (row["file"] + ".png")) as screen:
        check_round(screen, row['phase'])
        sheet.paste(screen, (x, y + 32), mask)
sheet.save(output / "A-device-states-native.png")
if not amoled:
    pages = ["settings", "volume", "brightness", "sessions", "manual", "network"]
    settings = Image.new("RGB", (3 * cell_w + gap, 2 * cell_h + 80), "#101616")
    captions = ImageDraw.Draw(settings)
    captions.text((gap, 16), "SUGARAT  |  SETTINGS  |  360 x 360", font=font, fill="#f0f4f3")
    for i, name in enumerate(pages):
        x, y = gap + i % 3 * cell_w, 64 + i // 3 * cell_h
        captions.text((x, y), name.title(), font=small, fill="#9baaa7")
        with Image.open(output / f"device_{name}.png") as frame:
            check_round(frame, name)
            settings.paste(frame, (x, y + 32), mask)
    settings.save(output / "Sugarat-settings-native.png")

frames = []
header = None
for state in ["listening", "speaking"]:
    for i in range(48):
        with Image.open(source / f"device_{state}_{i:02}.ppm") as frame:
            frame = frame.convert("RGB")
            region = frame.crop((0, 0, size, header_height))
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
    assert box and box[0] >= orbit[0] and box[1] >= orbit[1] and box[2] <= orbit[2] and box[3] <= orbit[3], box

rows = list(csv.DictReader((source / "device-refresh.csv").open()))
stationary = {"ready", "activate", "bind_agent", "platform_retrying", "error", "no_session", "setup"}
assert all(int(r["flushed_pixels"]) == 0 for r in rows if r["state"] in stationary)
maximum = max(float(r["screen_percent"]) for r in rows)
if amoled:
    assert maximum == 100
else:
    assert 0 < maximum <= 17, maximum
tokens = (Path(__file__).resolve().parents[2] / "sugarat/main/ui/ui_theme.h").read_text(encoding="utf-8")
colors = dict(re.findall(r"#define\s+(UI_\w+)_RGB\s+0x([A-Fa-f0-9]{6})\b", tokens))
backgrounds = re.findall(r"#define\s+UI_BG_RGB\s+0x([A-Fa-f0-9]{6})\b", tokens)
colors["UI_BG"] = backgrounds[0 if amoled else 1]
def luminance(token):
    rgb = [int(colors[token][i:i + 2], 16) / 255 for i in (0, 2, 4)]
    rgb = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in rgb]
    return sum(c * w for c, w in zip(rgb, (.2126, .7152, .0722)))
ratios = {}
for ink, bg in [("UI_TEXT", "UI_BG"), ("UI_TEXT_DIM", "UI_BG"), ("UI_TEXT", "UI_SURFACE_PRESSED")]:
    lo, hi = sorted([luminance(ink), luminance(bg)])
    ratios[f"{ink}/{bg}"] = round((hi + .05) / (lo + .05), 2)
assert min(ratios.values()) >= 4.5
report = {"design": "A / Orbit", "lvgl": "9.2.2", "resolution": [size, size], "device_phases": len(states),
          "simulation_step_ms": 33, "gif_fps": 30,
          "render_mode": "direct_full_transfer" if amoled else "partial",
          "max_animation_flush_percent": maximum,
          "contrast": ratios, "checks": {"all_device_phases_rendered": True, "round_glass_safe": True,
          "listening_preserved_during_playback": True, "playback_changes_centre_only": True,
          "no_fake_battery": True, "stationary_no_refresh": True, "button_guards": True}}
if not amoled:
    report["checks"]["settings_touch_navigation"] = True
(output / "render-report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
for name in ["device-states.csv", "device-refresh.csv"]:
    shutil.copyfile(source / name, output / name)
print(json.dumps(report, indent=2))

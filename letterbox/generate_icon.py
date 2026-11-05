#!/usr/bin/env python3
"""
Generate a modern flat icon for Letterbox app
Following Apple's design guidelines: simple, overlapping shapes, transparency
"""

from PIL import Image, ImageDraw, ImageFont
import math

# Icon dimensions (macOS requires 1024x1024)
SIZE = 1024

# Colors - Letterbox theme (modern flat design)
PRIMARY_DARK = (40, 40, 40)  # Dark gray background
SECONDARY_DARK = (60, 60, 60)  # Slightly lighter gray for depth
LETTER_COLOR = (255, 255, 255)  # White letter
ACCENT_COLOR = (58, 127, 159)  # Subtle blue accent from input field border

def create_squircle_mask(size, exponent=5.0, supersample=4):
    """Mask for Apple's continuous-corner icon shape, approximated by the
    superellipse |x|^n + |y|^n = 1. Drawn large and scaled down so the edge
    is antialiased."""
    big = size * supersample
    half = big / 2
    points = []
    steps = 2048
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        x = math.copysign(abs(c) ** (2 / exponent), c)
        y = math.copysign(abs(s) ** (2 / exponent), s)
        points.append((half + x * half, half + y * half))
    mask = Image.new('L', (big, big), 0)
    ImageDraw.Draw(mask).polygon(points, fill=255)
    return mask.resize((size, size), Image.LANCZOS)

def create_icon():
    """Generate the modern Letterbox app icon following Apple guidelines"""

    # Create main canvas with transparent background
    img = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Icon design: a dark tile on Apple's macOS icon grid (824x824 body with
    # ~100px of transparent margin). No outline: macOS draws its own edge and
    # shadow, and a drawn-in border looks wrong in the Dock and app switcher.
    margin = 100
    icon_size = SIZE - (margin * 2)
    tile = Image.new('RGBA', (icon_size, icon_size), PRIMARY_DARK + (255,))
    img.paste(tile, (margin, margin), create_squircle_mask(icon_size))
    draw = ImageDraw.Draw(img)

    # Layer 3: Letter "L" - the core visual element
    # Use Jost Bold font (should be bundled with app)
    letter_size = 500
    score_size = int(140 * 1.15)  # 15% larger for the subscript

    # Try to load Jost Bold font
    jost_paths = [
        "fonts/Jost/static/Jost-Bold.ttf",  # Relative to letterbox directory
        "./fonts/Jost/static/Jost-Bold.ttf",  # Current directory
        "../fonts/Jost/static/Jost-Bold.ttf",  # Parent directory
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",  # Fallback
    ]

    letter_font = None
    score_font = None

    for path in jost_paths:
        try:
            letter_font = ImageFont.truetype(path, letter_size)
            score_font = ImageFont.truetype(path, score_size)
            print(f"Loaded font from: {path}")
            break
        except:
            continue

    # Final fallback
    if not letter_font:
        try:
            letter_font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", letter_size)
            score_font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", score_size)
            print("Using Arial as fallback")
        except:
            letter_font = ImageFont.load_default()
            score_font = ImageFont.load_default()

    # Create text layer
    text_layer = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    text_draw = ImageDraw.Draw(text_layer)

    # Draw letter "L"
    letter = "L"
    bbox = text_draw.textbbox((0, 0), letter, font=letter_font)
    letter_width = bbox[2] - bbox[0]
    letter_height = bbox[3] - bbox[1]
    letter_offset_x = bbox[0]  # Left offset from origin
    letter_offset_y = bbox[1]  # Top offset from origin

    # Center the letter accounting for bbox offsets
    letter_x = (SIZE - letter_width) // 2 - letter_offset_x
    letter_y = (SIZE - letter_height) // 2 - letter_offset_y

    # Manual horizontal adjustment to center the visual weight of the L
    # (The font bbox has unequal left/right spacing)
    letter_x -= 14

    # Pure white, full opacity
    text_draw.text((letter_x, letter_y), letter, font=letter_font, fill=(255, 255, 255, 255))

    # Draw subscript score "1"
    score = "1"

    # Get bounding box to determine actual rendered size and offset
    score_bbox = text_draw.textbbox((0, 0), score, font=score_font)
    score_width = score_bbox[2] - score_bbox[0]
    score_height = score_bbox[3] - score_bbox[1]
    score_offset_x = score_bbox[0]  # Left offset from origin
    score_offset_y = score_bbox[1]  # Top offset from origin

    # Position subscript in bottom right corner with equal margins from edges
    corner_margin = 100  # Distance from tile edge to "1" edge
    vertical_adjustment = 40  # Move "1" up to balance with horizontal margin

    # Calculate position accounting for bbox offsets
    score_x = (SIZE - margin) - score_width - corner_margin - score_offset_x
    score_y = (SIZE - margin) - score_height - corner_margin - score_offset_y - vertical_adjustment

    # Pure white, full opacity (same as L)
    text_draw.text((score_x, score_y), score, font=score_font, fill=(255, 255, 255, 255))

    # Composite text onto icon
    img = Image.alpha_composite(img, text_layer)

    return img

if __name__ == "__main__":
    print("Generating Letterbox icon...")
    icon = create_icon()

    # Save at full resolution
    output_path = "Letterbox_Icon.png"
    icon.save(output_path, "PNG")
    print(f"Icon saved to {output_path}")

    # Generate additional sizes for macOS icon set
    sizes = [1024, 512, 256, 128, 64, 32, 16]
    for size in sizes:
        resized = icon.resize((size, size), Image.LANCZOS)
        resized.save(f"Letterbox_Icon_{size}.png", "PNG")
        print(f"Generated {size}x{size} icon")

    print("\nTo create an .icns file for macOS:")
    print("1. Create Letterbox.iconset directory")
    print("2. Place icons with names: icon_512x512@2x.png (1024), icon_512x512.png, etc.")
    print("3. Run: iconutil -c icns Letterbox.iconset")

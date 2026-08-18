#!/usr/bin/env python3
import os
import sys
from PIL import Image, ImageFont, ImageDraw, ImageFilter

font_path = "/mnt/c/Windows/Fonts/arialbd.ttf"
if not os.path.exists(font_path):
    font_path = "/mnt/c/Windows/Fonts/segoeuib.ttf"

def render_font_set(font_size, char_list, glow_radius=6.0, glow_intensity=1.8):
    font = ImageFont.truetype(font_path, font_size)
    ascent, descent = font.getmetrics()
    baseline = ascent
    
    unique_chars = list(dict.fromkeys(char_list))
    glyphs = {}
    
    for c in unique_chars:
        if c == ' ':
            adv = int(font_size * 0.35)
            glyphs[c] = {
                'w': 0, 'h': 0, 'off_x': 0, 'off_y': 0,
                'adv_x': adv, 'core': [], 'glow': []
            }
            continue
            
        adv = int(font.getlength(c))
        pad = int(glow_radius * 2.8) + 4
        
        img_w = max(adv, int(font_size * 0.8)) + pad * 2
        img_h = (ascent + descent) + pad * 2
        
        # High resolution core render
        img = Image.new("L", (img_w, img_h), 0)
        draw = ImageDraw.Draw(img)
        draw.text((pad, pad), c, font=font, fill=255)
        
        # Multi-pass Gaussian blur glow
        glow1 = img.filter(ImageFilter.GaussianBlur(radius=glow_radius * 0.5))
        glow2 = img.filter(ImageFilter.GaussianBlur(radius=glow_radius))
        glow3 = img.filter(ImageFilter.GaussianBlur(radius=glow_radius * 1.5))
        
        # Composite glow
        glow_data = []
        g1 = list(glow1.getdata())
        g2 = list(glow2.getdata())
        g3 = list(glow3.getdata())
        c_data = list(img.getdata())
        
        min_x, min_y, max_x, max_y = img_w, img_h, 0, 0
        
        for y in range(img_h):
            for x in range(img_w):
                idx = y * img_w + x
                # Combine multi-tier bloom
                val = int((g1[idx] * 0.45 + g2[idx] * 0.40 + g3[idx] * 0.35) * glow_intensity)
                if val > 255: val = 255
                glow_data.append(val)
                
                if val > 4 or c_data[idx] > 0:
                    if x < min_x: min_x = x
                    if x > max_x: max_x = x
                    if y < min_y: min_y = y
                    if y > max_y: max_y = y
                    
        if min_x > max_x:
            min_x, min_y, max_x, max_y = 0, 0, img_w - 1, img_h - 1
            
        crop_w = max_x - min_x + 1
        crop_h = max_y - min_y + 1
        
        # Extract cropped core & glow arrays
        core_crop = []
        glow_crop = []
        for y in range(min_y, max_y + 1):
            for x in range(min_x, max_x + 1):
                idx = y * img_w + x
                core_crop.append(c_data[idx])
                glow_crop.append(glow_data[idx])
                
        off_x = min_x - pad
        off_y = min_y - (pad + baseline)
        
        glyphs[c] = {
            'w': crop_w,
            'h': crop_h,
            'off_x': off_x,
            'off_y': off_y,
            'adv_x': adv,
            'core': core_crop,
            'glow': glow_crop
        }
    return glyphs

def export_c_header(filename):
    print("Rasterizing fonts with broad Gaussian bloom halo...")
    hero_glyphs = render_font_set(122, "0123456789.-", glow_radius=7.5, glow_intensity=2.0)
    unit_glyphs = render_font_set(54, "wkWMVA", glow_radius=4.8, glow_intensity=2.0)
    metric_glyphs = render_font_set(40, "0123456789.VAWk", glow_radius=3.8, glow_intensity=1.8)
    label_glyphs = render_font_set(18, "TAPOP16MVOLTAGE CURRENT ON OFF", glow_radius=2.2, glow_intensity=1.6)
    
    print(f"Writing {filename}...")
    with open(filename, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n")
        f.write("typedef struct {\n")
        f.write("    char c;\n")
        f.write("    uint8_t w;\n")
        f.write("    uint8_t h;\n")
        f.write("    int16_t off_x;\n")
        f.write("    int16_t off_y;\n")
        f.write("    uint8_t adv_x;\n")
        f.write("    const uint8_t *core_data;\n")
        f.write("    const uint8_t *glow_data;\n")
        f.write("} arial_glyph_t;\n\n")
        
        def write_set(set_name, glyphs_dict):
            for c, g in glyphs_dict.items():
                c_id = ord(c)
                arr_name = f"{set_name}_g{c_id}"
                if g['w'] > 0 and g['h'] > 0:
                    f.write(f"static const uint8_t {arr_name}_core[{g['w'] * g['h']}] = {{\n    ")
                    f.write(", ".join(str(v) for v in g['core']))
                    f.write("\n};\n")
                    f.write(f"static const uint8_t {arr_name}_glow[{g['w'] * g['h']}] = {{\n    ")
                    f.write(", ".join(str(v) for v in g['glow']))
                    f.write("\n};\n\n")
            
            f.write(f"static const arial_glyph_t {set_name}[] = {{\n")
            for c, g in glyphs_dict.items():
                c_id = ord(c)
                arr_name = f"{set_name}_g{c_id}"
                escaped_c = "\\'" if c == "'" else ("\\\\" if c == "\\" else c)
                if g['w'] > 0 and g['h'] > 0:
                    f.write(f"    {{ '{escaped_c}', {g['w']}, {g['h']}, {g['off_x']}, {g['off_y']}, {g['adv_x']}, {arr_name}_core, {arr_name}_glow }},\n")
                else:
                    f.write(f"    {{ '{escaped_c}', 0, 0, 0, 0, {g['adv_x']}, 0, 0 }},\n")
            f.write("    { 0, 0, 0, 0, 0, 0, 0, 0 }\n")
            f.write("};\n\n")
            
        write_set("font_arial_hero", hero_glyphs)
        write_set("font_arial_unit", unit_glyphs)
        write_set("font_arial_metric", metric_glyphs)
        write_set("font_arial_label", label_glyphs)

if __name__ == "__main__":
    export_c_header("/home/cmb/tapo-project/matter-display/tapo-matter-display/main/arial_font.h")
    print("Generated main/arial_font.h successfully!")

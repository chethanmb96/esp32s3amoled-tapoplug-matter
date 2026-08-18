#!/usr/bin/env python3
import os
import sys
from PIL import Image, ImageFont, ImageDraw, ImageFilter

arial_bd = "/mnt/c/Windows/Fonts/arialbd.ttf"
arial_reg = "/mnt/c/Windows/Fonts/arial.ttf"

def render_font_set(font_path, font_size, char_list, glow_radius=4.5):
    font = ImageFont.truetype(font_path, font_size)
    glyphs = {}
    unique_chars = list(dict.fromkeys(char_list))
    
    for c in unique_chars:
        if c == ' ':
            advance = int(font_size * 0.35)
            glyphs[c] = {
                'w': 0, 'h': 0, 'off_x': 0, 'off_y': 0,
                'adv_x': advance,
                'core': [], 'glow': []
            }
            continue
            
        bbox = font.getbbox(c)
        if not bbox or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            advance = int(font_size * 0.35)
            glyphs[c] = {
                'w': 0, 'h': 0, 'off_x': 0, 'off_y': 0,
                'adv_x': advance,
                'core': [], 'glow': []
            }
            continue
            
        pad = int(glow_radius * 2.5) + 3
        gx0, gy0, gx1, gy1 = bbox
        gw = gx1 - gx0
        gh = gy1 - gy0
        adv = int(font.getlength(c))
        
        img_w = gw + pad * 2
        img_h = gh + pad * 2
        img = Image.new("L", (img_w, img_h), 0)
        draw = ImageDraw.Draw(img)
        draw.text((pad - gx0, pad - gy0), c, font=font, fill=255)
        
        glow_img = img.filter(ImageFilter.GaussianBlur(radius=glow_radius))
        
        glow_pixels = list(glow_img.getdata())
        core_pixels = list(img.getdata())
        
        min_x, min_y, max_x, max_y = img_w, img_h, 0, 0
        for y in range(img_h):
            for x in range(img_w):
                idx = y * img_w + x
                if glow_pixels[idx] > 3 or core_pixels[idx] > 0:
                    if x < min_x: min_x = x
                    if x > max_x: max_x = x
                    if y < min_y: min_y = y
                    if y > max_y: max_y = y
                    
        if min_x > max_x:
            min_x, min_y, max_x, max_y = 0, 0, img_w - 1, img_h - 1
            
        crop_w = max_x - min_x + 1
        crop_h = max_y - min_y + 1
        
        core_crop = img.crop((min_x, min_y, max_x + 1, max_y + 1))
        glow_crop = glow_img.crop((min_x, min_y, max_x + 1, max_y + 1))
        
        off_x = min_x - pad + gx0
        off_y = min_y - pad + gy0
        
        glyphs[c] = {
            'w': crop_w,
            'h': crop_h,
            'off_x': off_x,
            'off_y': off_y,
            'adv_x': adv,
            'core': list(core_crop.getdata()),
            'glow': list(glow_crop.getdata())
        }
    return glyphs

def export_c_header(filename):
    print("Rasterizing Arial fonts from TTF...")
    hero_glyphs = render_font_set(arial_bd, 118, "0123456789.-", glow_radius=5.2)
    unit_glyphs = render_font_set(arial_bd, 56, "wkWMVA", glow_radius=3.8)
    metric_glyphs = render_font_set(arial_bd, 44, "0123456789.VAWk", glow_radius=3.2)
    label_glyphs = render_font_set(arial_bd, 19, "TAPOP16MVOLTAGE CURRENT ON OFF", glow_radius=2.0)
    
    print("Writing header file...")
    with open(filename, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n")
        f.write("typedef struct {\n")
        f.write("    char c;\n")
        f.write("    uint8_t w;\n")
        f.write("    uint8_t h;\n")
        f.write("    int8_t off_x;\n")
        f.write("    int8_t off_y;\n")
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

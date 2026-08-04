#!/usr/bin/env python3
"""
Convert gif1/*.gif files into C source files for embedding into firmware.
Each GIF is stored as a raw byte array + lv_image_dsc_t descriptor.
"""

import os
import sys
import glob

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
GIF_DIR = SCRIPT_DIR
OUT_DIR = os.path.join(SCRIPT_DIR, "generated")

HEADER_FILE = os.path.join(OUT_DIR, "expression_gif1_assets.h")

C_FILE_TEMPLATE = """\
/**
 * Auto-generated from {gif_name} - DO NOT EDIT
 */

#include "lvgl/lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {var_name}_data[] = {{
{hex_data}
}};

const lv_image_dsc_t {var_name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RAW,
    .header.w = 0,
    .header.h = 0,
    .data_size = {data_size},
    .data = {var_name}_data,
}};
"""

HEADER_TEMPLATE = """\
/**
 * Expression GIF assets declarations (gif1).
 * Auto-generated - DO NOT EDIT
 */

#ifndef __EXPRESSION_GIF2_ASSETS_H__
#define __EXPRESSION_GIF2_ASSETS_H__

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {{
#endif

{declarations}

#ifdef __cplusplus
}}
#endif

#endif /* __EXPRESSION_GIF2_ASSETS_H__ */
"""


def gif_to_varname(gif_filename):
    """Convert filename like 'face_calm.gif' to C variable name 'gif_face_calm'."""
    name = os.path.splitext(gif_filename)[0]
    # Replace any non-alphanumeric chars with underscore
    name = ''.join(c if c.isalnum() or c == '_' else '_' for c in name)
    return "gif_" + name


def convert_gif(gif_path, out_dir):
    """Convert a single GIF file to a C source file."""
    gif_name = os.path.basename(gif_path)
    var_name = gif_to_varname(gif_name)

    with open(gif_path, 'rb') as f:
        data = f.read()

    # Format hex data (16 bytes per line)
    hex_lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
        hex_lines.append('    ' + hex_str + ',')

    # Remove trailing comma from last line
    if hex_lines:
        hex_lines[-1] = hex_lines[-1].rstrip(',')

    hex_data = '\n'.join(hex_lines)

    c_content = C_FILE_TEMPLATE.format(
        gif_name=gif_name,
        var_name=var_name,
        hex_data=hex_data,
        data_size=len(data)
    )

    c_filename = var_name + ".c"
    c_path = os.path.join(out_dir, c_filename)

    with open(c_path, 'w') as f:
        f.write(c_content)

    return var_name, gif_name, len(data)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    gif_files = sorted(glob.glob(os.path.join(GIF_DIR, "*.gif")))
    if not gif_files:
        print(f"No GIF files found in {GIF_DIR}")
        sys.exit(1)

    print(f"Converting {len(gif_files)} GIF files...")

    assets = []
    for gif_path in gif_files:
        var_name, gif_name, size = convert_gif(gif_path, OUT_DIR)
        assets.append((var_name, gif_name, size))
        print(f"  {gif_name} -> {var_name}.c ({size} bytes)")

    # Generate header file
    declarations = '\n'.join(
        f'LV_IMAGE_DECLARE({var_name});'
        for var_name, _, _ in assets
    )

    header_content = HEADER_TEMPLATE.format(declarations=declarations)

    with open(HEADER_FILE, 'w') as f:
        f.write(header_content)

    print(f"\nGenerated {len(assets)} C files + header in {OUT_DIR}")
    total_size = sum(s for _, _, s in assets)
    print(f"Total embedded GIF data: {total_size} bytes ({total_size/1024:.1f} KB)")


if __name__ == '__main__':
    main()

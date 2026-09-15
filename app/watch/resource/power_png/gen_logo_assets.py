#!/usr/bin/env python3
"""
Convert power_png/*.gif into C source files for embedding into firmware.
Each GIF is stored as a raw byte array + lv_image_dsc_t descriptor.

Usage: python3 gen_logo_assets.py   (regenerates all assets below)
"""

import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "generated")

# (source GIF, C symbol name)
ASSETS = [
    ("expr_logo.gif",  "gif_expr_logo"),
    ("watch_logo.gif", "gif_watch_logo"),
    ("expr_anim.gif",  "gif_expr_anim"),
    ("watch_anim.gif", "gif_watch_anim"),
]

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

H_FILE_TEMPLATE = """\
/**
 * {var_name} asset declaration.
 * Auto-generated from {gif_name} - DO NOT EDIT
 */

#ifndef __{guard}__
#define __{guard}__

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {{
#endif

LV_IMAGE_DECLARE({var_name});

#ifdef __cplusplus
}}
#endif

#endif /* __{guard}__ */
"""


def gif_to_c(gif_path, var_name, out_dir):
    with open(gif_path, 'rb') as f:
        data = f.read()

    hex_lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ', '.join('0x%02x' % b for b in chunk)
        hex_lines.append('    ' + hex_str + ',')

    # Remove trailing comma from last line
    if hex_lines:
        hex_lines[-1] = hex_lines[-1].rstrip(',')

    c_content = C_FILE_TEMPLATE.format(
        gif_name=os.path.basename(gif_path),
        var_name=var_name,
        hex_data='\n'.join(hex_lines),
        data_size=len(data),
    )

    h_content = H_FILE_TEMPLATE.format(
        gif_name=os.path.basename(gif_path),
        var_name=var_name,
        guard=('__%s_H__' % var_name.upper()),
    )

    c_file = os.path.join(out_dir, '%s.c' % var_name)
    h_file = os.path.join(out_dir, '%s.h' % var_name)

    with open(c_file, 'w') as f:
        f.write(c_content)
    with open(h_file, 'w') as f:
        f.write(h_content)

    print("Generated: %s (%d bytes embedded)" % (c_file, len(data)))
    print("Generated: %s" % h_file)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    for gif_name, var_name in ASSETS:
        gif_path = os.path.join(SCRIPT_DIR, gif_name)
        if not os.path.exists(gif_path):
            print("ERROR: %s not found" % gif_path)
            sys.exit(1)
        gif_to_c(gif_path, var_name, OUT_DIR)


if __name__ == '__main__':
    main()

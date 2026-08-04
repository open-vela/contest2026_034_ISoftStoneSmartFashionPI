#!/usr/bin/env python3
"""
Convert bootlogo.gif into a C source file for embedding into firmware.
Stored as a raw byte array + lv_image_dsc_t descriptor.
"""

import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
GIF_PATH = os.path.join(SCRIPT_DIR, "bootlogo.gif")
OUT_DIR = os.path.join(SCRIPT_DIR, "generated")

C_FILE = os.path.join(OUT_DIR, "gif_bootlogo.c")
H_FILE = os.path.join(OUT_DIR, "gif_bootlogo.h")

C_FILE_TEMPLATE = """\
/**
 * Auto-generated from bootlogo.gif - DO NOT EDIT
 */

#include "lvgl/lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

static const LV_ATTRIBUTE_MEM_ALIGN uint8_t gif_bootlogo_data[] = {{
{hex_data}
}};

const lv_image_dsc_t gif_bootlogo = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RAW,
    .header.w = 0,
    .header.h = 0,
    .data_size = {data_size},
    .data = gif_bootlogo_data,
}};
"""

H_FILE_CONTENT = """\
/**
 * Bootlogo GIF asset declaration.
 * Auto-generated - DO NOT EDIT
 */

#ifndef __GIF_BOOTLOGO_H__
#define __GIF_BOOTLOGO_H__

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_IMAGE_DECLARE(gif_bootlogo);

#ifdef __cplusplus
}
#endif

#endif /* __GIF_BOOTLOGO_H__ */
"""


def main():
    if not os.path.exists(GIF_PATH):
        print(f"ERROR: {GIF_PATH} not found")
        sys.exit(1)

    os.makedirs(OUT_DIR, exist_ok=True)

    with open(GIF_PATH, 'rb') as f:
        data = f.read()

    print(f"Converting bootlogo.gif ({len(data)} bytes)...")

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
        hex_data=hex_data,
        data_size=len(data)
    )

    with open(C_FILE, 'w') as f:
        f.write(c_content)

    with open(H_FILE, 'w') as f:
        f.write(H_FILE_CONTENT)

    print(f"Generated: {C_FILE} ({len(data)} bytes embedded)")
    print(f"Generated: {H_FILE}")
    print(f"Total embedded GIF data: {len(data)} bytes ({len(data)/1024:.1f} KB)")


if __name__ == '__main__':
    main()

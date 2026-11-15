#!/usr/bin/env python3
"""Convert SMAA's lookup textures (AreaTex.png, SearchTex.png) into a C header.

Usage:
    python -I tools/png_to_header.py <AreaTex.png> <SearchTex.png> <out.h>

Only the standard library (zlib, struct) is used: a minimal PNG decoder that
handles non-interlaced images with 8-bit gray/RGB/RGBA/gray+alpha and 1/2/4/8-bit
gray/palette data, which covers both SMAA textures (AreaTex is 8-bit RGB,
SearchTex is a 2-bit palette image).

Output layout (what runtime/win/postfx.cpp uploads):
  smaa_area_tex[]   160x560, 2 bytes per texel (R, G) = D3DFMT_A8L8 memory order
                    (low byte L = R, high byte A = G), read in the shader as .ra.
  smaa_search_tex[] 64x16, 1 byte per texel (R) = D3DFMT_L8, read as .r.
"""
import struct
import sys
import zlib

SMAA_NOTICE = """\
/**
 * Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)
 * Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
 * Copyright (C) 2013 Belen Masia (bmasia@unizar.es)
 * Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)
 * Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to
 * do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software. As clarification, there
 * is no requirement that the copyright notice and permission be included in
 * binary distributions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
"""


def decode_png(path):
    """Return (width, height, rows) where rows[y][x] is an (r, g, b, a) tuple."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos = 8
    idat = b''
    palette = None
    trns = None
    hdr = None
    while pos < len(data):
        (length,) = struct.unpack('>I', data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b'IHDR':
            hdr = struct.unpack('>IIBBBBB', body)
        elif ctype == b'PLTE':
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif ctype == b'tRNS':
            trns = body
        elif ctype == b'IDAT':
            idat += body
        elif ctype == b'IEND':
            break
    if hdr is None:
        raise ValueError(f'{path}: no IHDR')
    w, h, depth, color, _comp, _filt, interlace = hdr
    if interlace:
        raise ValueError(f'{path}: interlaced PNGs are not supported')
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    if color in (2, 4, 6) and depth != 8:
        raise ValueError(f'{path}: only 8-bit RGB/RGBA/gray+alpha supported')
    bits_pp = channels * depth
    bpp = max(1, bits_pp // 8)            # filter byte distance
    stride = (w * bits_pp + 7) // 8
    raw = zlib.decompress(idat)
    if len(raw) != h * (stride + 1):
        raise ValueError(f'{path}: unexpected data size {len(raw)}')

    lines = []
    prev = bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                cur[i] = (cur[i] + a) & 255
            elif ft == 2:
                cur[i] = (cur[i] + b) & 255
            elif ft == 3:
                cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
            elif ft != 0:
                raise ValueError(f'{path}: bad filter {ft}')
        lines.append(cur)
        prev = cur

    rows = []
    for line in lines:
        row = []
        for x in range(w):
            if depth < 8:
                per = 8 // depth
                byte = line[x // per]
                shift = 8 - depth * (x % per + 1)
                v = (byte >> shift) & ((1 << depth) - 1)
            else:
                v = None
            if color == 3:
                idx = v if v is not None else line[x]
                r, g, b = palette[idx]
                a = trns[idx] if trns is not None and idx < len(trns) else 255
                row.append((r, g, b, a))
            elif color == 0:
                if v is not None:
                    v = v * 255 // ((1 << depth) - 1)
                else:
                    v = line[x]
                row.append((v, v, v, 255))
            elif color == 2:
                r, g, b = line[3 * x:3 * x + 3]
                row.append((r, g, b, 255))
            elif color == 4:
                l, a = line[2 * x:2 * x + 2]
                row.append((l, l, l, a))
            else:
                row.append(tuple(line[4 * x:4 * x + 4]))
        rows.append(row)
    return w, h, rows


def c_array(name, data, per_line=32):
    out = [f'static const unsigned char {name}[{len(data)}] = {{']
    for i in range(0, len(data), per_line):
        out.append(','.join(str(b) for b in data[i:i + per_line]) + ',')
    out.append('};')
    return '\n'.join(out)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    area_path, search_path, out_path = sys.argv[1:]
    aw, ah, arows = decode_png(area_path)
    sw, sh, srows = decode_png(search_path)
    if (aw, ah) != (160, 560) or (sw, sh) != (64, 16):
        raise SystemExit(f'unexpected sizes: AreaTex {aw}x{ah}, SearchTex {sw}x{sh}')
    area = bytearray()
    nonzero_b = 0
    for row in arows:
        for r, g, b, _a in row:
            area += bytes((r, g))
            nonzero_b += b != 0
    search = bytearray(r for row in srows for (r, _g, _b, _a) in row)
    print(f'AreaTex {aw}x{ah}: {len(area)} bytes (RG), blue nonzero texels: {nonzero_b}')
    print(f'SearchTex {sw}x{sh}: {len(search)} bytes (R), distinct values: {sorted(set(search))}')

    with open(out_path, 'w', newline='\n') as f:
        f.write('// SMAA precomputed lookup textures (AreaTex, SearchTex), generated by\n')
        f.write('// tools/png_to_header.py from the PNGs shipped with SMAA. Do not edit.\n')
        f.write('// AreaTex: 160x560 RG8 stored as (R,G) byte pairs = D3DFMT_A8L8 order (L=R, A=G).\n')
        f.write('// SearchTex: 64x16 R8 = D3DFMT_L8.\n')
        f.write(SMAA_NOTICE)
        f.write('#pragma once\n\n')
        f.write(f'#define SMAA_AREATEX_WIDTH {aw}\n#define SMAA_AREATEX_HEIGHT {ah}\n')
        f.write(f'#define SMAA_SEARCHTEX_WIDTH {sw}\n#define SMAA_SEARCHTEX_HEIGHT {sh}\n\n')
        f.write(c_array('smaa_area_tex', area) + '\n\n')
        f.write(c_array('smaa_search_tex', search) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())

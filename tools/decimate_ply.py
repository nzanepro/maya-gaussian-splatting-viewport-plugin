#!/usr/bin/env python3
"""Write a smaller 3DGS .ply by keeping every Nth splat.

Useful for separating "does it render correctly" from "is it fast enough"
when testing the OpenGL 4.1 path, where the depth sort runs on the CPU.

    python3 tools/decimate_ply.py big.ply small.ply --target 150000
"""
import argparse
import os

TYPE_SIZES = {'float': 4, 'float32': 4, 'double': 8, 'uchar': 1, 'uint8': 1,
              'char': 1, 'int8': 1, 'short': 2, 'ushort': 2,
              'int': 4, 'uint': 4, 'int32': 4, 'uint32': 4}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--target', type=int, default=150000,
                    help='approximate splat count to keep (default 150000)')
    args = ap.parse_args()

    with open(args.src, 'rb') as f:
        header = b''
        while b'end_header' not in header:
            line = f.readline()
            if not line:
                raise SystemExit('no end_header found — not a PLY?')
            header += line
        body_offset = f.tell()

    lines = header.decode('ascii').splitlines()
    if not lines or lines[0].strip() != 'ply':
        raise SystemExit('missing ply magic')
    if not any(l.startswith('format binary_little_endian') for l in lines):
        raise SystemExit('only binary_little_endian is supported')

    count, props = 0, []
    for line in lines:
        tok = line.split()
        if not tok:
            continue
        if tok[0] == 'element' and tok[1] == 'vertex':
            count = int(tok[2])
        elif tok[0] == 'property':
            if tok[1] == 'list':
                raise SystemExit('list properties are not supported')
            props.append(tok[1])
    stride = sum(TYPE_SIZES[p] for p in props)

    step = max(1, count // max(1, args.target))
    keep = len(range(0, count, step))
    out_header = '\n'.join('element vertex %d' % keep
                           if l.startswith('element vertex') else l
                           for l in lines) + '\n'

    with open(args.src, 'rb') as f, open(args.dst, 'wb') as g:
        g.write(out_header.encode('ascii'))
        for i in range(0, count, step):
            f.seek(body_offset + i * stride)
            g.write(f.read(stride))

    print('%s: %d splats, %d properties, %d-byte stride' %
          (os.path.basename(args.src), count, len(props), stride))
    print('%s: %d splats (every %dth), %.1f MB' %
          (os.path.basename(args.dst), keep, step, os.path.getsize(args.dst) / 1e6))


if __name__ == '__main__':
    main()

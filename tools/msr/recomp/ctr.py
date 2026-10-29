"""Reads the game's code from a decrypted 3DS dump (.3ds/.cci): the ExHeader and the
decompressed ExeFS .code (code.bin), as loaded at the text address.

Usage: python tools/msr/recomp/ctr.py <dump> <output-dir>
Writes code.bin, exheader.bin and layout.json. Game code: keep the output out of Git and
packages (build-windows/ is ignored).
"""
import json
import pathlib
import struct
import sys

MEDIA_UNIT = 0x200


def blz_decompress(data):
    """Nintendo's backwards LZ77 (the ExeFS .code compression)."""
    end_info, extra = struct.unpack_from('<II', data, len(data) - 8)
    header_size = end_info >> 24
    compressed_size = end_info & 0xFFFFFF
    out = bytearray(data) + bytearray(extra)
    read = len(data) - header_size
    write = len(out)
    stop = len(data) - compressed_size
    while read > stop:
        read -= 1
        flags = out[read]
        for _ in range(8):
            if read <= stop:
                break
            if flags & 0x80:
                read -= 2
                pair = out[read] | (out[read + 1] << 8)
                length = ((pair >> 12) & 0xF) + 3
                distance = (pair & 0xFFF) + 2
                for _ in range(length):
                    write -= 1
                    out[write] = out[write + distance + 1]
            else:
                read -= 1
                write -= 1
                out[write] = out[read]
            flags = (flags << 1) & 0xFF
    return bytes(out)


def read_game(path):
    with open(path, 'rb') as f:
        header = f.read(MEDIA_UNIT)
        ncch_offset = 0
        if header[0x100:0x104] == b'NCSD':
            ncch_offset = struct.unpack_from('<I', header, 0x120)[0] * MEDIA_UNIT
        f.seek(ncch_offset)
        ncch = f.read(MEDIA_UNIT)
        if ncch[0x100:0x104] != b'NCCH':
            raise ValueError('no NCCH partition')
        if not ncch[0x188 + 7] & 0x4:
            raise ValueError('the dump is encrypted')
        exheader = f.read(0x800)
        exefs_offset = struct.unpack_from('<I', ncch, 0x1A0)[0] * MEDIA_UNIT
        f.seek(ncch_offset + exefs_offset)
        exefs_header = f.read(0x200)
        code = None
        for i in range(10):
            name, offset, size = struct.unpack_from('<8sII', exefs_header, i * 16)
            if name.rstrip(b'\0') == b'.code':
                f.seek(ncch_offset + exefs_offset + 0x200 + offset)
                code = f.read(size)
        if code is None:
            raise ValueError('no .code in the ExeFS')
    if exheader[0xD] & 1:
        code = blz_decompress(code)

    def segment(offset):
        address, pages, size = struct.unpack_from('<III', exheader, offset)
        return {'address': address, 'pages': pages, 'size': size}

    layout = {
        'title': exheader[:8].rstrip(b'\0').decode('ascii', 'replace'),
        'program_id': '%016X' % struct.unpack_from('<Q', ncch, 0x118)[0],
        'text': segment(0x10),
        'rodata': segment(0x20),
        'data': segment(0x30),
        'bss_size': struct.unpack_from('<I', exheader, 0x3C)[0],
        'code_size': len(code),
    }
    return code, exheader, layout


def main(dump, out_dir):
    code, exheader, layout = read_game(dump)
    out = pathlib.Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'code.bin').write_bytes(code)
    (out / 'exheader.bin').write_bytes(exheader)
    (out / 'layout.json').write_text(json.dumps(layout, indent=2))
    print(json.dumps(layout, indent=2))


if __name__ == '__main__':
    main(*sys.argv[1:3])

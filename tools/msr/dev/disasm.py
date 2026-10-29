"""Disassembly helpers for the game's code (requires capstone: python -m pip install capstone).

  python tools/msr/dev/disasm.py dis <hex addr> <count>      ARM instructions, with literal pool
                                                             values and the strings they point to
  python tools/msr/dev/disasm.py callers <hex addr>...       BL call sites of these functions
  python tools/msr/dev/disasm.py refs <hex addr>...          instructions forming these addresses
                                                             (add/sub rX, pc, #imm or ldr =addr)

Reads code.bin from the recompiler's input (tools/msr/recomp/ctr.py extracts it from your dump):
build-windows/msr-recomp/input by default, or $MSR_CODE_DIR. The code loads at 0x100000; its
text segment (ARM) is the first TEXT_SIZE bytes in the USA release.
"""
import os
import pathlib
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs

ROOT = pathlib.Path(__file__).resolve().parents[3]
BASE = 0x100000
TEXT_SIZE = 6047816


def load():
    folder = pathlib.Path(os.environ.get('MSR_CODE_DIR', ROOT / 'build-windows/msr-recomp/input'))
    return (folder / 'code.bin').read_bytes()


def literal(code, address, op_str):
    """The word a pc-relative ldr loads, if it is inside the code."""
    lit = address + 8 + int(op_str.split('#')[1].rstrip(']'), 16)
    if BASE <= lit < BASE + len(code) - 4:
        return struct.unpack_from('<I', code, lit - BASE)[0]
    return None


def dis(code, address, count):
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.skipdata = True
    for i in md.disasm(code[address - BASE:address - BASE + 4 * count], address):
        extra = ''
        if i.mnemonic.startswith('ldr') and '[pc, #' in i.op_str and i.op_str.endswith(']'):
            value = literal(code, i.address, i.op_str)
            if value is not None:
                extra = ' =0x%X' % value
                if BASE <= value < BASE + len(code):
                    text = code[value - BASE:value - BASE + 50].split(b'\0')[0]
                    if len(text) > 3 and all(32 <= c < 127 for c in text):
                        extra += ' %r' % text
        print('0x%x: %-8s %s%s' % (i.address, i.mnemonic, i.op_str, extra))


def callers(code, targets):
    found = {t: [] for t in targets}
    for offset in range(0, TEXT_SIZE, 4):
        word = struct.unpack_from('<I', code, offset)[0]
        if (word & 0x0F000000) == 0x0B000000:  # BL, any condition
            imm = word & 0xFFFFFF
            if imm & 0x800000:
                imm -= 0x1000000
            target = BASE + offset + 8 + imm * 4
            if target in found:
                found[target].append(BASE + offset)
    for target, sites in found.items():
        print(hex(target), len(sites), ' '.join(hex(s) for s in sites[:40]))


def refs(code, targets):
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.skipdata = True
    for i in md.disasm(code[:TEXT_SIZE], BASE):
        target = None
        if i.mnemonic in ('add', 'sub') and ', pc, #' in i.op_str:
            try:
                imm = int(i.op_str.split('#')[1], 16)
            except ValueError:
                continue
            target = i.address + 8 + (imm if i.mnemonic == 'add' else -imm)
        elif i.mnemonic == 'ldr' and '[pc, #' in i.op_str:
            target = literal(code, i.address, i.op_str)
        if target in targets:
            print('0x%X %s %s -> 0x%X' % (i.address, i.mnemonic, i.op_str, target))


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('dis', 'callers', 'refs'):
        sys.exit(__doc__)
    code = load()
    args = [int(a, 16) for a in sys.argv[2:]]
    if sys.argv[1] == 'dis':
        dis(code, args[0], int(sys.argv[3]) if len(sys.argv) > 3 else 32)
    elif sys.argv[1] == 'callers':
        callers(code, args)
    else:
        refs(code, set(args))


if __name__ == '__main__':
    main()

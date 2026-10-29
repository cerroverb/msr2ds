"""Explains the code the game ran that static analysis missed (see analyze.py).

Usage: python tools/msr/recomp/diagnose.py <dir> <coverage.bin> [samples]
For each missed region: the instruction before it, whether that was decoded, whether the
region's start was a rejected pointer candidate, and its first instructions.
"""
import collections
import json
import pathlib
import sys

import capstone

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import analyze  # noqa: E402


def main(directory, coverage_path, samples='40'):
    directory = pathlib.Path(directory)
    code = (directory / 'code.bin').read_bytes()
    layout = json.loads((directory / 'layout.json').read_text())
    an = analyze.Analyzer(code, layout)
    an.run()
    words, _ = analyze.load_coverage(coverage_path)
    static_words = {h & ~3 for h in an.code_half}
    executed = [an.base + 4 * i for i, hit in enumerate(words) if hit and an.in_text(an.base + 4 * i)]
    missed = [w for w in executed if w not in static_words]
    regions = []
    for w in missed:
        if regions and w == regions[-1][1]:
            regions[-1][1] = w + 4
        else:
            regions.append([w, w + 4])
    arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    reasons = collections.Counter()
    out = []
    for start, end in regions:
        prev = an.insns.get((start - 4, False))
        rejected = an.pointer_candidates.get((start, False)) == 'rejected' or \
            an.pointer_candidates.get((start, True)) == 'rejected'
        in_data = any(w in an.data_words for w in range(start, end, 4))
        if rejected:
            reason = 'rejected pointer candidate'
        elif in_data:
            reason = 'overlaps words taken for data'
        elif prev and prev['kind'] == 'call':
            reason = 'after a call (path stopped)'
        elif prev and prev['kind'] in ('svc',):
            reason = 'after svc'
        elif prev and prev['ends']:
            reason = 'after an unconditional end (entry reached indirectly)'
        elif prev:
            reason = 'after decoded code (%s)' % prev['kind']
        else:
            reason = 'no decoded code before'
        reasons[reason] += (end - start) // 4
        if len(out) < int(samples):
            first = [f'{i.mnemonic} {i.op_str}' for i in arm.disasm(code[start - an.base:start - an.base + 12], start, 3)]
            before = f"{prev['mnemonic']} {prev['op_str']}" if prev else None
            out.append({'start': '0x%X' % start, 'words': (end - start) // 4, 'reason': reason,
                        'before': before, 'first': first})
    print(json.dumps({'missed_words': len(missed), 'regions': len(regions),
                      'words_by_reason': reasons.most_common(), 'samples': out}, indent=1))


if __name__ == '__main__':
    main(*sys.argv[1:4])

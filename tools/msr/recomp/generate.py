"""Generates the ahead-of-time recompiled Samus Returns code (C++) from code.bin.

One C++ function per region: the code reachable from a function entry without crossing calls.
Calls, returns, indirect branches and system calls leave the function for the dispatcher
(src/core/arm/msr_aot/arm_aot.cpp); a function resumes at its entry, after each call or system
call, at loop heads (where it yields when the time slice ends) and at switch targets.
Instructions the translator does not handle exit to the interpreter for that one instruction.
Instructions listed in patchable.txt, which the player rewrites in memory (the frame limit,
src/msr/frame_rate.h), first check their word and run on the interpreter when it changed.

Usage: python tools/msr/recomp/generate.py <dir from ctr.py> --out <generated dir>
Needs capstone (for the analysis). The output is game code: keep it out of Git and packages.
"""
import argparse
import collections
import hashlib
import json
import pathlib
import random
import re
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import analyze  # noqa: E402
import emit_arm  # noqa: E402
import emit_thumb  # noqa: E402
from emit_arm import Unsupported  # noqa: E402


class Region:
    def __init__(self, entry, thumb):
        self.entry, self.thumb = entry, thumb
        self.insns = {}       # address -> analyzer record
        self.resume = {entry}
        self.gotos = set()
        self.interpret = {}   # address -> reason
        self.name = 'R_%08X_%d' % (entry, thumb)


class Flow:
    def __init__(self, region, pc):
        self.region, self.pc = region, pc

    def jump(self, target, thumb, call=False):
        region = self.region
        if not call and bool(thumb) == bool(region.thumb) and target in region.insns:
            region.gotos.add(target)
            if target <= self.pc:  # loop: yield when the time slice is used up
                region.resume.add(target)
                return 'if (c.ticks >= c.budget) { c.r[15] = 0x%08Xu; return; } goto L_%08X;' % (target, target)
            return 'goto L_%08X;' % target
        return 'c.r[15] = 0x%08Xu; c.thumb = %d; return;' % (target, 1 if thumb else 0)

    def exit(self, call=False):
        return 'return;'

    def dispatch(self):
        return 'goto dispatch;'


def build_region(an, entry, thumb):
    region = Region(entry, thumb)
    work = [entry]
    while work:
        a = work.pop()
        if a in region.insns:
            continue
        rec = an.insns.get((a, thumb))
        if rec is None:
            continue
        region.insns[a] = rec
        for t, target_thumb in rec['targets']:
            if target_thumb != thumb or ((t, thumb) in an.entries and t != entry):
                continue  # mode switch or tail call: leaves the region
            work.append(t)
        nxt = a + rec['size']
        if (a, thumb) in getattr(an, 'patchable', ()):
            region.resume.add(nxt)  # back from the interpreter right after it
        if rec['kind'] in ('call', 'icall', 'svc'):
            region.resume.add(nxt)
            work.append(nxt)
        elif not rec['ends']:
            work.append(nxt)
        for t, target_thumb in an.table_targets.get((a, thumb), ()):
            if target_thumb == thumb:
                region.resume.add(t)
                work.append(t)
    return region


def instruction_body(an, region, a):
    rec = region.insns[a]
    flow = Flow(region, a)
    raw = an.word(a) if not region.thumb else struct.unpack_from('<H', an.code, a - an.base)[0]
    raw2 = None
    try:
        if region.thumb:
            if rec['size'] == 4:
                raw2 = struct.unpack_from('<H', an.code, a + 2 - an.base)[0]
            body = emit_thumb.emit(a, raw, raw2, flow)
        else:
            body = emit_arm.emit(a, raw, flow)
        if rec['kind'] in ('table_ldr', 'table_add') and not region.thumb:
            # Computed jumps into the switch: continue through the function's own dispatch.
            body = [l.replace('return;', 'goto dispatch;') if 'c.r[15] = (' in l or l.strip() == 'return;' else l
                    for l in body]
        if (a, region.thumb) in getattr(an, 'patchable', ()):
            word = raw if raw2 is None else raw | (raw2 << 16)
            read = 'R32' if rec['size'] == 4 else 'R16'
            body = ['if (%s(c, 0x%08Xu) != 0x%08Xu) { c.r[15] = 0x%08Xu; c.thumb = %d; c.exit = Exit::Patched; return; }'
                    % (read, a, word, a, region.thumb)] + body
        return body
    except Unsupported as error:
        region.interpret[a] = str(error)
        region.resume.add(a + rec['size'])
        return ['c.r[15] = 0x%08Xu; c.thumb = %d; c.exit = Exit::Interpret; return;' % (a, region.thumb)]


def render_region(an, region):
    order = sorted(region.insns)
    bodies = {a: instruction_body(an, region, a) for a in order}
    # Fallthrough into an address emitted elsewhere (or outside the region).
    tails = {}
    for i, a in enumerate(order):
        rec = region.insns[a]
        nxt = a + rec['size']
        following = order[i + 1] if i + 1 < len(order) else None
        if not rec['ends'] and following != nxt:
            tails[a] = Flow(region, a).jump(nxt, region.thumb)
    labels = region.resume | region.gotos
    body = []
    for i, a in enumerate(order):
        if a in labels:
            k = 1
            while i + k < len(order) and order[i + k] not in labels:
                k += 1
            costs = getattr(an, 'ticks', None)
            cost = sum(costs.get((order[i + j], region.thumb), 1) for j in range(k)) if costs else k
            body.append('L_%08X: c.ticks += %d;' % (a, cost))
        body += ['    ' + line for line in bodies[a]]
        if a in tails:
            body.append('    ' + tails[a])
    body = [localize(line) for line in body]
    # Registers, flags and the tick count live in locals: loaded once, stored at the one exit.
    text = '\n'.join(body)
    regs = sorted({int(m) for m in LOCAL_REGISTER.findall(text)})
    flags = [f for f in 'nzcv' if re.search(r'\bf%s\b' % f, text)]
    out = ['static void %s(Ctx& c) {' % region.name]
    if regs:
        out.append('    u32 %s;' % ', '.join('r%d = c.r[%d]' % (r, r) for r in regs))
    if flags:
        out.append('    u32 %s;' % ', '.join('f%s = c.%s' % (f, f) for f in flags))
    out.append('    u64 tk = c.ticks;')
    out += ['dispatch:', '    switch (c.r[15]) {']
    for pc in sorted(region.resume):
        if pc in region.insns:
            out.append('    case 0x%08Xu: goto L_%08X;' % (pc, pc))
    out += ['    default: goto out;', '    }']
    out += body
    stores = ['c.r[%d] = r%d;' % (r, r) for r in regs] + ['c.%s = f%s;' % (f, f) for f in flags]
    out += ['out:', '    ' + ' '.join(stores + ['c.ticks = tk;']), '}']
    return out


LOCAL_REGISTER = re.compile(r'\br(\d+)\b')
FLAG_SYNC_OUT = 'c.n = fn; c.z = fz; c.c = fc; c.v = fv; '
FLAG_SYNC_IN = ' fn = c.n; fz = c.z; fc = c.c; fv = c.v;'


def localize(line):
    """Rewrites a generated line from the context to the function's locals."""
    sync = 'Cpsr(c)' in line or 'SetCpsrFlags(c' in line
    line = line.replace('AddFlags(c, ', 'AddFlagsL(fn, fz, fc, fv, ')
    line = line.replace('Logical(c, ', 'LogicalL(fn, fz, fc, ')
    line = line.replace('NZ(c, ', 'NZL(fn, fz, ')
    line = re.sub(r'\bc\.r\[(\d+)\]', lambda m: m.group(0) if m.group(1) == '15' else 'r' + m.group(1), line)
    line = re.sub(r'\bc\.([nzcv])\b', r'f\1', line)
    line = line.replace('c.ticks', 'tk').replace('return;', 'goto out;')
    if sync:
        # MRS, MSR and FMSTAT read or write the whole CPSR: flags through the context.
        stripped = line.lstrip()
        indent = line[:len(line) - len(stripped)]
        line = indent + FLAG_SYNC_OUT + stripped + (FLAG_SYNC_IN if 'SetCpsrFlags' in stripped else '')
    return line


REPO = HERE.parents[2]


def instruction_ticks(an, work_dir):
    """Cycle cost of every instruction from the emulator's ARM11 timing model (ticks.cpp), so
    the recompiled code advances emulated time exactly like the JIT."""
    work_dir.mkdir(parents=True, exist_ok=True)
    exe = work_dir / 'ticks.exe'
    sources = [HERE / 'ticks.cpp', REPO / 'src/core/arm/dynarmic/arm_tick_counts.cpp']
    if not exe.exists() or any(src.stat().st_mtime > exe.stat().st_mtime for src in sources):
        subprocess.run(['g++', '-std=c++20', '-O1', '-I', str(REPO / 'src'), '-o', str(exe)] +
                       [str(src) for src in sources], check=True)
    keys = sorted(an.insns)
    words = bytearray()
    for addr, thumb in keys:
        if thumb:
            first = struct.unpack_from('<H', an.code, addr - an.base)[0]
            if an.insns[(addr, thumb)]['size'] == 4:
                second = struct.unpack_from('<H', an.code, addr + 2 - an.base)[0]
                word = (first << 16) | second
            else:
                word = first
        else:
            word = an.word(addr)
        words += struct.pack('<II', 1 if thumb else 0, word)
    (work_dir / 'ticks.in').write_bytes(bytes(words))
    subprocess.run([str(exe), str(work_dir / 'ticks.in'), str(work_dir / 'ticks.out')], check=True)
    costs = struct.unpack('<%dI' % len(keys), (work_dir / 'ticks.out').read_bytes())
    return dict(zip(keys, costs))


def write_if_changed(path, text):
    """Keeps unchanged shards' timestamps, so only changed ones recompile."""
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def generate(an, regions, out_dir, shard_size=6000, code_hash=None, shard_span=0):
    """shard_span > 0 groups regions by entry address (span bytes of text per shard), so a
    change to one region only rewrites its shard; otherwise shards hold shard_size instructions."""
    out_dir.mkdir(parents=True, exist_ok=True)
    # The dispatcher maps each resume PC to one function: its own entry first.
    owner = {}
    for region in regions:
        owner.setdefault((region.entry, region.thumb), region)
    rendered = [(region, render_region(an, region)) for region in regions]
    for region, _ in rendered:
        for pc in sorted(region.resume):
            if pc in region.insns:
                owner.setdefault((pc, region.thumb), region)
    shards, current, size = [], [], 0
    if shard_span:
        buckets = collections.defaultdict(list)
        for item in rendered:
            buckets[(item[0].entry - an.base) // shard_span].append(item)
        shards = [buckets[k] for k in range(max(buckets) + 1)]
    else:
        for item in rendered:
            current.append(item)
            size += len(item[0].insns)
            if size >= shard_size:
                shards.append(current)
                current, size = [], 0
        if current:
            shards.append(current)
    keep = {'aot_shard_%03d.cpp' % i for i in range(len(shards))}
    for old in out_dir.glob('aot_shard_*.cpp'):
        if old.name not in keep:
            old.unlink()
    for index, shard in enumerate(shards):
        lines = ['// Generated by tools/msr/recomp/generate.py from Samus Returns code.bin. Do not edit.',
                 '#include "aot_runtime.h"', '', 'namespace {', 'using namespace MsrAot;', '']
        entries = []
        for region, text in shard:
            lines += text
            lines.append('')
            for pc in sorted(region.resume):
                if owner.get((pc, region.thumb)) is region:
                    entries.append('    {0x%08Xu, %du, %s},' % (pc, region.thumb, region.name))
        lines += ['} // namespace', '',
                  'extern const MsrAot::Entry msr_aot_shard_%d[] = {' % index] + (entries or ['    {0u, 0u, nullptr},']) + \
                 ['};', 'extern const MsrAot::u32 msr_aot_shard_%d_size = %du;' % (index, len(entries)), '']
        write_if_changed(out_dir / ('aot_shard_%03d.cpp' % index), '\n'.join(lines))
    registry = ['// Generated by tools/msr/recomp/generate.py. Do not edit.', '#include "aot_generated.h"', '']
    for index in range(len(shards)):
        registry.append('extern const MsrAot::Entry msr_aot_shard_%d[];' % index)
        registry.append('extern const MsrAot::u32 msr_aot_shard_%d_size;' % index)
    registry += ['', 'namespace MsrAot {', 'const Shard kShards[] = {']
    registry += ['    {msr_aot_shard_%d, &msr_aot_shard_%d_size},' % (i, i) for i in range(len(shards))]
    registry += ['};', 'const u32 kShardCount = %du;' % len(shards)]
    digest = code_hash or bytes(32)
    registry.append('const u8 kCodeSha256[32] = {%s};' % ', '.join('0x%02X' % b for b in digest))
    registry.append('const u32 kTextBase = 0x%08Xu;' % an.base)
    registry.append('const u32 kTextSize = 0x%Xu;' % (an.text_end - an.base))
    patches = []
    for address, thumb in sorted(getattr(an, 'patchable', ())):
        size = an.insns[(address, thumb)]['size']
        original = struct.unpack_from('<I' if size == 4 else '<H', an.code, address - an.base)[0]
        patches.append('    {0x%08Xu, %du, 0x%08Xu},' % (address, size, original))
    registry += ['const Patchable kPatchable[] = {'] + (patches or ['    {0u, 0u, 0u},']) + ['};']
    registry.append('const u32 kPatchableCount = %du;' % len(patches))
    registry.append('} // namespace MsrAot')
    write_if_changed(out_dir / 'aot_registry.cpp', '\n'.join(registry) + '\n')

    reasons = collections.Counter()
    for region in regions:
        reasons.update(region.interpret.values())
    stats = {
        'regions': len(regions),
        'shards': len(shards),
        'instructions_emitted': sum(len(r.insns) for r in regions),
        'unique_instructions': len({(a, r.thumb) for r in regions for a in r.insns}),
        'resume_points': sum(1 for r in owner.values()),
        'interpreted_instructions': sum(len(r.interpret) for r in regions),
        'interpreted_by_reason': reasons.most_common(),
    }
    (out_dir / 'stats.json').write_text(json.dumps(stats, indent=1))
    return stats


def patchable_instructions(an):
    """patchable.txt: one address per line (odd for Thumb), # comments."""
    found = set()
    for line in (HERE / 'patchable.txt').read_text().splitlines():
        line = line.split('#')[0].strip()
        if not line:
            continue
        value = int(line, 0)
        key = (value & ~1, value & 1)
        if key not in an.insns:
            print('patchable.txt: no instruction at 0x%08X in this program' % value, file=sys.stderr)
            continue
        found.add(key)
    return found


def all_regions(an):
    return [build_region(an, entry, thumb) for (entry, thumb) in sorted(an.entries)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory')
    parser.add_argument('--out', required=True)
    parser.add_argument('--shard-size', type=int, default=6000)
    parser.add_argument('--shard-span', type=lambda v: int(v, 0), default=0x4000,
                        help='bytes of text per shard (0: by --shard-size)')
    parser.add_argument('--sample', type=int, default=0, help='only this many random regions')
    parser.add_argument('--extra', action='append', default=[],
                        help='more entry points: user/msr/recomp-jit-entries-core*.txt from a run')
    parser.add_argument('--seed', type=int, default=1)
    args = parser.parse_args()
    directory = pathlib.Path(args.directory)
    code = (directory / 'code.bin').read_bytes()
    layout = json.loads((directory / 'layout.json').read_text())
    an = analyze.Analyzer(code, layout)
    an.extra_files = args.extra
    an.run()
    an.ticks = instruction_ticks(an, pathlib.Path(args.out).parent / 'ticks')
    an.patchable = patchable_instructions(an)
    regions = all_regions(an)
    if args.sample:
        regions = sorted(random.Random(args.seed).sample(regions, args.sample), key=lambda r: (r.entry, r.thumb))
    text = code[:layout['text']['size']]
    stats = generate(an, regions, pathlib.Path(args.out), args.shard_size, hashlib.sha256(text).digest(),
                     args.shard_span)
    print(json.dumps(stats, indent=1))


if __name__ == '__main__':
    main()

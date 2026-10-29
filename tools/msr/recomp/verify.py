"""Checks generated code against Unicorn emulating the 3DS's ARM11 MPCore.

Generates a sample of regions (generate.py), compiles them into a harness, and runs each from
random states (registers, flags, VFP registers, memory full of pointers, small integers and
floats). Unicorn runs from the same state until control leaves the region: a call, return,
indirect branch, system call, an instruction left to the interpreter, or code outside it. Exit
address, registers, flags, VFP state, memory changes and faults must all match.

Usage: python tools/msr/recomp/verify.py <dir from ctr.py> [--regions N] [--cases K]
       [--seed S] [--thumb-share F]
Needs capstone, unicorn, numpy and MSYS2's g++. Works in <dir>/verify/.
"""
import argparse
import collections
import concurrent.futures
import os
import json
import pathlib
import random
import struct
import subprocess
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_INTR, \
    UC_HOOK_MEM_WRITE, UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL, UC_MEM_FETCH_UNMAPPED
from unicorn import arm_const as U

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE))
import analyze  # noqa: E402
import generate  # noqa: E402

RAM_BASE = 0x08000000
RAM_WORDS = 0x8000  # 128 KiB
RAM_SIZE = RAM_WORDS * 4
FPSCR = 0x03C00000  # what the 3DS runs applications with: flush-to-zero, default NaN
import numpy as np  # noqa: E402


def ram_image(seed):
    """Deterministic RAM, mostly pointers into RAM so code gets past its first loads; the
    harness builds the same image. Words: 5/8 pointers, then small ints, random, floats."""
    i = np.arange(RAM_WORDS, dtype=np.uint64)
    with np.errstate(over='ignore'):
        z = np.uint64(seed) * np.uint64(0x100000) + i + np.uint64(0x9E3779B97F4A7C15)
        z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
        z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
        z = z ^ (z >> np.uint64(31))
    kind = z & np.uint64(7)
    pointer = np.uint64(RAM_BASE) + ((z >> np.uint64(8)) % np.uint64(RAM_WORDS - 1024)) * np.uint64(4)
    small = (z >> np.uint64(8)) & np.uint64(0xFF)
    word = (z >> np.uint64(32)) & np.uint64(0xFFFFFFFF)
    floating = np.uint64(0x3F800000) + ((z >> np.uint64(8)) & np.uint64(0x7FFFFF))
    out = np.where(kind < 5, pointer, np.where(kind == 5, small, np.where(kind == 6, word, floating)))
    return out.astype('<u4').tobytes()

MAGIC = 0x0FFF0000
TLS = RAM_BASE + 0x100
STEP_LIMIT = 100_000
FLOW_EXITS = ('call', 'icall', 'return', 'ijump')

HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "aot_generated.h"

using namespace MsrAot;

static std::vector<u8> code_image, ram;
static u32 code_base, ram_base;
static bool faulted;
static u32 fault_address;
static Ctx* current; // a fault ends the run at the next loop check (reads return 0 after it)
static void Fault(u32 a) {
    if (!faulted) { faulted = true; fault_address = a; }
    current->budget = 0;
}

static u8* At(u32 a) {
    if (a >= code_base && a - code_base < code_image.size()) return &code_image[a - code_base];
    if (a >= ram_base && a - ram_base < ram.size()) return &ram[a - ram_base];
    return nullptr;
}
template <typename T> static T SlowRead(void*, u32 a) {
    T v = 0;
    for (u32 i = 0; i < sizeof(T); ++i) {
        u8* p = At(a + i);
        if (!p) { Fault(a); return 0; }
        v |= T(*p) << (8 * i);
    }
    return v;
}
template <typename T> static void SlowWrite(void*, u32 a, T v) {
    for (u32 i = 0; i < sizeof(T); ++i) {
        if (!At(a + i)) { Fault(a); return; }
    }
    for (u32 i = 0; i < sizeof(T); ++i) *At(a + i) = u8(v >> (8 * i));
}
static bool WriteExclusive32(void*, u32 a, u32 v, u32 expected) {
    if (SlowRead<u32>(nullptr, a) != expected) return false;
    SlowWrite<u32>(nullptr, a, v);
    return true;
}
static bool WriteExclusive64(void*, u32 a, u64 v, u64 expected) {
    const u64 current = SlowRead<u32>(nullptr, a) | (u64(SlowRead<u32>(nullptr, a + 4)) << 32);
    if (current != expected) return false;
    SlowWrite<u32>(nullptr, a, u32(v)); SlowWrite<u32>(nullptr, a + 4, u32(v >> 32));
    return true;
}
static u64 Mix(u64 x) {
    u64 z = x + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int main(int argc, char** argv) {
    // argv: image.bin cases.bin results.bin code_base ram_base ram_words
    {
        FILE* f = std::fopen(argv[1], "rb");
        u8 buf[65536]; size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) code_image.insert(code_image.end(), buf, buf + n);
        std::fclose(f);
    }
    const std::vector<u8> pristine = code_image;
    code_base = std::strtoul(argv[4], nullptr, 0);
    ram_base = std::strtoul(argv[5], nullptr, 0);
    const u32 ram_words = std::strtoul(argv[6], nullptr, 0);
    ram.resize(ram_words * 4);
    std::vector<u8*> pages(1u << 20, nullptr);
    for (u32 off = 0; off + 0x1000 <= code_image.size(); off += 0x1000) pages[(code_base + off) >> 12] = &code_image[off];
    for (u32 off = 0; off < ram.size(); off += 0x1000) pages[(ram_base + off) >> 12] = &ram[off];

    std::unordered_map<u64, Handler> handlers;
    for (u32 s = 0; s < kShardCount; ++s)
        for (u32 i = 0; i < *kShards[s].count; ++i)
            if (kShards[s].entries[i].handler)
                handlers[(u64(kShards[s].entries[i].pc) << 1) | kShards[s].entries[i].thumb] = kShards[s].entries[i].handler;

    FILE* in = std::fopen(argv[2], "rb");
    FILE* out = std::fopen(argv[3], "wb");
    u32 count = 0;
    std::fread(&count, 4, 1, in);
    for (u32 k = 0; k < count; ++k) {
        u32 pc, thumb, regs[16], cpsr, fpscr, s[32], tls; u64 seed;
        std::fread(&pc, 4, 1, in); std::fread(&thumb, 4, 1, in); std::fread(&seed, 8, 1, in);
        std::fread(regs, 4, 16, in); std::fread(&cpsr, 4, 1, in); std::fread(&fpscr, 4, 1, in);
        std::fread(s, 4, 32, in); std::fread(&tls, 4, 1, in);
        for (u32 i = 0; i < ram_words; ++i) {
            const u64 z = Mix(seed * 0x100000ULL + i);
            u32 w;
            const u32 kind = u32(z & 7);
            if (kind < 5) w = ram_base + u32(((z >> 8) % (ram_words - 1024)) * 4);
            else if (kind == 5) w = u32((z >> 8) & 0xFF);
            else if (kind == 6) w = u32(z >> 32);
            else w = 0x3F800000U + u32((z >> 8) & 0x7FFFFF);
            std::memcpy(&ram[i * 4], &w, 4);
        }
        const std::vector<u8> ram_before = ram;
        Ctx c{};
        for (int i = 0; i < 16; ++i) c.r[i] = regs[i];
        SetCpsrFlags(c, cpsr, false);
        c.thumb = thumb;
        for (int i = 0; i < 32; ++i) c.s[i] = s[i];
        SetFpscr(c, fpscr);
        c.tls_ro = tls;
        c.budget = 5000000; // well past the reference's step limit
        current = &c;
        c.pages = pages.data();
        c.mem.read8 = SlowRead<u8>; c.mem.read16 = SlowRead<u16>; c.mem.read32 = SlowRead<u32>;
        c.mem.write8 = SlowWrite<u8>; c.mem.write16 = SlowWrite<u16>; c.mem.write32 = SlowWrite<u32>;
        c.mem.write_exclusive32 = WriteExclusive32;
        c.mem.write_exclusive64 = WriteExclusive64;
        faulted = false; fault_address = 0;
        c.r[15] = pc;
        auto it = handlers.find((u64(pc) << 1) | thumb);
        u32 known = it != handlers.end();
        if (known) it->second(c);
        const u32 header[7] = {known, u32(c.exit), c.r[15], c.thumb, c.svc, faulted, fault_address};
        std::fwrite(header, 4, 7, out);
        std::fwrite(c.r, 4, 15, out);
        const u32 flags = Cpsr(c);
        std::fwrite(&flags, 4, 1, out); std::fwrite(&c.fpscr, 4, 1, out); std::fwrite(c.s, 4, 32, out);
        std::vector<std::pair<u32, u32>> changed;
        for (u32 i = 0; i < ram.size(); ++i) if (ram[i] != ram_before[i]) changed.push_back({ram_base + i, ram[i]});
        for (u32 i = 0; i < code_image.size(); ++i)
            if (code_image[i] != pristine[i]) { changed.push_back({code_base + i, code_image[i]}); code_image[i] = pristine[i]; }
        const u32 n = u32(changed.size());
        std::fwrite(&n, 4, 1, out);
        for (auto& [a, v] : changed) { std::fwrite(&a, 4, 1, out); std::fwrite(&v, 4, 1, out); }
    }
    std::fclose(out);
    return 0;
}
'''


class Reference:
    def __init__(self, image, base):
        self.base, self.image = base, image
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.ctl_set_cpu_model(U.UC_CPU_ARM_11MPCORE)
        uc.mem_map(base, (len(image) + 0xFFF) & ~0xFFF, UC_PROT_ALL)
        uc.mem_write(base, image)
        uc.mem_map(RAM_BASE, RAM_SIZE, UC_PROT_ALL)
        uc.mem_map(MAGIC, 0x1000, UC_PROT_ALL)
        uc.mem_write(MAGIC, b'\xfe\xff\xff\xea' * 1024)  # b .
        uc.reg_write(U.UC_ARM_REG_C1_C0_2, uc.reg_read(U.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
        uc.hook_add(UC_HOOK_CODE, self.on_code)
        uc.hook_add(UC_HOOK_INTR, self.on_interrupt)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.on_unmapped)

    def on_code(self, uc, address, size, data):
        thumb = (uc.reg_read(U.UC_ARM_REG_CPSR) >> 5) & 1
        if self.prev is not None:
            prev_address, prev_size, prev_kind = self.prev
            if prev_kind in FLOW_EXITS and address != prev_address + prev_size:
                self.stop('branch', address, thumb)
                return
        rec = self.region.insns.get(address) if thumb == self.region.thumb else None
        if rec is None or address in self.region.interpret:
            self.stop('interpret' if rec is not None else 'branch', address, thumb)
            return
        self.prev = (address, rec['size'], rec['kind'])
        self.steps += 1

    def on_interrupt(self, uc, number, data):
        thumb = (uc.reg_read(U.UC_ARM_REG_CPSR) >> 5) & 1
        self.stop('svc', uc.reg_read(U.UC_ARM_REG_PC), thumb)

    def stop(self, kind, pc, thumb):
        if self.result is None:
            self.result = (kind, pc, thumb)
        self.uc.emu_stop()

    def on_write(self, uc, access, address, size, value, data):
        self.writes.update(range(address, address + size))

    def on_unmapped(self, uc, access, address, size, value, data):
        if access == UC_MEM_FETCH_UNMAPPED:
            # A branch to an unmapped address: the region exited there.
            thumb = (uc.reg_read(U.UC_ARM_REG_CPSR) >> 5) & 1
            self.stop('branch', address & ~1, thumb)
            return False
        if self.fault is None:
            self.fault = address
        return False

    def run(self, region, case):
        uc = self.uc
        self.region, self.prev, self.result, self.fault = region, None, None, None
        self.steps = 0
        self.writes = set()
        ram = ram_image(case['seed'])
        uc.mem_write(RAM_BASE, ram)
        uc.reg_write(U.UC_ARM_REG_CPSR, case['cpsr'])
        for i in range(13):
            uc.reg_write(U.UC_ARM_REG_R0 + i, case['regs'][i])
        uc.reg_write(U.UC_ARM_REG_SP, case['regs'][13])
        uc.reg_write(U.UC_ARM_REG_LR, case['regs'][14])
        uc.reg_write(U.UC_ARM_REG_FPSCR, case['fpscr'])
        for i in range(32):
            uc.reg_write(U.UC_ARM_REG_S0 + i, case['vfp'][i])
        uc.reg_write(U.UC_ARM_REG_C13_C0_3, case['tls'])
        start = case['pc'] | (1 if case['thumb'] else 0)
        try:
            uc.emu_start(start, MAGIC + 0x800, count=STEP_LIMIT)
        except UcError:
            pass
        out = {'result': self.result, 'fault': self.fault, 'steps': self.steps}
        if self.result is None and self.fault is None:
            out['result'] = ('timeout', 0, 0)
        out['regs'] = [uc.reg_read(U.UC_ARM_REG_R0 + i) for i in range(13)] + \
            [uc.reg_read(U.UC_ARM_REG_SP), uc.reg_read(U.UC_ARM_REG_LR)]
        out['cpsr'] = uc.reg_read(U.UC_ARM_REG_CPSR)
        out['fpscr'] = uc.reg_read(U.UC_ARM_REG_FPSCR)
        out['vfp'] = [uc.reg_read(U.UC_ARM_REG_S0 + i) for i in range(32)]
        changed = {}
        for a in self.writes:
            if RAM_BASE <= a < RAM_BASE + RAM_SIZE:
                value = uc.mem_read(a, 1)[0]
                if value != ram[a - RAM_BASE]:
                    changed[a] = value
            elif self.base <= a < self.base + len(self.image):
                value = uc.mem_read(a, 1)[0]
                if value != self.image[a - self.base]:
                    changed[a] = value
                uc.mem_write(a, self.image[a - self.base:a - self.base + 1])
        out['changed'] = changed
        return out


def pick_regions(regions, count, thumb_share, rng):
    thumb = [r for r in regions if r.thumb]
    arm = [r for r in regions if not r.thumb]
    n_thumb = min(len(thumb), int(count * thumb_share))
    return rng.sample(thumb, n_thumb) + rng.sample(arm, min(len(arm), count - n_thumb))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory')
    parser.add_argument('--regions', type=int, default=600)
    parser.add_argument('--cases', type=int, default=8)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--thumb-share', type=float, default=0.15)
    parser.add_argument('--max-region', type=int, default=400, help='skip larger regions')
    parser.add_argument('--runtime', help='alternative runtime header directory (mutation tests)')
    args = parser.parse_args()
    directory = pathlib.Path(args.directory)
    work = directory / 'verify'
    gen_dir = work / 'gen'
    code = (directory / 'code.bin').read_bytes()
    layout = json.loads((directory / 'layout.json').read_text())
    an = analyze.Analyzer(code, layout)
    an.run()
    an.ticks = generate.instruction_ticks(an, work / 'ticks')
    rng = random.Random(args.seed)
    regions = [r for r in generate.all_regions(an) if len(r.insns) <= args.max_region]
    sample = sorted(pick_regions(regions, args.regions, args.thumb_share, rng), key=lambda r: (r.entry, r.thumb))
    stats = generate.generate(an, sample, gen_dir, shard_size=3000)
    (work / 'harness.cpp').write_text(HARNESS)
    runtime = pathlib.Path(args.runtime) if args.runtime else REPO / 'src/core/arm/msr_aot'
    exe = work / 'harness.exe'
    sources = [work / 'harness.cpp'] + sorted(gen_dir.glob('*.cpp'))
    flags = ['-std=c++20', '-O1', '-w', '-ffp-contract=off', '-fno-strict-aliasing', '-I', str(runtime)]
    objects = work / 'obj'
    objects.mkdir(exist_ok=True)

    def compile_one(source):
        obj = objects / (source.stem + '.o')
        result = subprocess.run(['g++'] + flags + ['-c', str(source), '-o', str(obj)],
                                capture_output=True, text=True)
        return obj, result
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, (os.cpu_count() or 4) - 4)) as pool:
        compiled = list(pool.map(compile_one, sources))
    for obj, result in compiled:
        if result.returncode != 0:
            sys.exit('harness build failed:\n' + result.stderr[-6000:])
    build = subprocess.run(['g++', '-o', str(exe)] + [str(obj) for obj, _ in compiled],
                           capture_output=True, text=True)
    if build.returncode != 0:
        sys.exit('harness link failed:\n' + build.stderr[-6000:])

    image = code + bytes((layout['bss_size'] + 0xFFF) & ~0xFFF)
    (work / 'image.bin').write_bytes(image)
    ref = Reference(image, an.base)
    cases, expected = [], []
    timeouts = 0
    for region in sample:
        for _ in range(args.cases):
            seed = rng.getrandbits(32)
            r = random.Random(seed)
            regs = [([RAM_BASE + r.randrange(RAM_WORDS - 1024) * 4] * 6 + [r.randrange(64), r.randrange(64),
                     r.getrandbits(32), 0])[r.randrange(10)] for _ in range(13)]
            # Start at the entry or at any other point the dispatcher can resume at.
            points = sorted(p for p in region.resume if p in region.insns)
            start = region.entry if r.random() < 0.4 else r.choice(points)
            regs += [RAM_BASE + RAM_SIZE - 0x2000, MAGIC, start]
            vfp = [struct.unpack('<I', struct.pack('<f', r.uniform(-100.0, 100.0)))[0] for _ in range(32)]
            case = {'pc': start, 'thumb': region.thumb, 'seed': seed, 'regs': regs,
                    'cpsr': (r.getrandbits(4) << 28) | 0x10, 'fpscr': FPSCR, 'vfp': vfp, 'tls': TLS,
                    'region': region}
            want = ref.run(region, case)
            if want['result'] is not None and want['result'][0] == 'timeout':
                timeouts += 1
                continue
            cases.append(case)
            expected.append(want)
    with open(work / 'cases.bin', 'wb') as f:
        f.write(struct.pack('<I', len(cases)))
        for c in cases:
            f.write(struct.pack('<IIQ', c['pc'], c['thumb'], c['seed']))
            f.write(struct.pack('<16I', *c['regs']))
            f.write(struct.pack('<II', c['cpsr'], c['fpscr']))
            f.write(struct.pack('<32I', *c['vfp']))
            f.write(struct.pack('<I', c['tls']))
    run = subprocess.run([str(exe), str(work / 'image.bin'), str(work / 'cases.bin'), str(work / 'results.bin'),
                          hex(an.base), hex(RAM_BASE), str(RAM_WORDS)], capture_output=True, text=True, timeout=1200)
    if run.returncode != 0:
        sys.exit('harness failed (%d): %s' % (run.returncode, run.stderr[-2000:]))

    data = (work / 'results.bin').read_bytes()
    off = 0
    outcomes = collections.Counter()
    endings = collections.Counter()
    steps = [w['steps'] for w in expected]
    problems_by_kind = collections.Counter()
    bad_regions = {}
    exit_names = {0: 'branch', 1: 'svc', 2: 'interpret'}
    for case, want in zip(cases, expected):
        known, exit_kind, pc, thumb, svc, faulted, fault_address = struct.unpack_from('<7I', data, off); off += 28
        regs = list(struct.unpack_from('<15I', data, off)); off += 60
        cpsr, fpscr = struct.unpack_from('<2I', data, off); off += 8
        vfp = list(struct.unpack_from('<32I', data, off)); off += 128
        (n,) = struct.unpack_from('<I', data, off); off += 4
        changed = {}
        for _ in range(n):
            a, v = struct.unpack_from('<2I', data, off); off += 8
            changed[a] = v
        problems = []
        if not known:
            problems.append('entry not registered')
        elif want['fault'] is not None or faulted:
            if not (faulted and want['fault'] == fault_address):
                problems.append('fault %s vs reference %s' % (hex(fault_address) if faulted else None,
                                                               hex(want['fault']) if want['fault'] is not None else None))
        else:
            kind, want_pc, want_thumb = want['result']
            # A BX to an ARM address with bit 1 set is unpredictable; like the emulator's JIT,
            # the translation clears both low bits.
            if not want_thumb:
                want_pc &= ~3
            if exit_names.get(exit_kind) != kind or pc != want_pc or thumb != want_thumb:
                problems.append('exit %s 0x%X/%d vs %s 0x%X/%d' % (exit_names.get(exit_kind), pc, thumb, kind, want_pc, want_thumb))
            for i in range(15):
                if regs[i] != want['regs'][i]:
                    problems.append('r%d 0x%X vs 0x%X' % (i, regs[i], want['regs'][i]))
            if (cpsr ^ want['cpsr']) & 0xF80F0000:
                problems.append('cpsr 0x%08X vs 0x%08X' % (cpsr & 0xF80F0000, want['cpsr'] & 0xF80F0000))
            if (fpscr ^ want['fpscr']) & 0xF3C00000:
                problems.append('fpscr 0x%08X vs 0x%08X' % (fpscr, want['fpscr']))
            for i in range(32):
                if vfp[i] != want['vfp'][i]:
                    problems.append('s%d 0x%08X vs 0x%08X' % (i, vfp[i], want['vfp'][i]))
            if changed != want['changed']:
                diff = sorted(set(changed.items()) ^ set(want['changed'].items()))[:3]
                problems.append('memory %s' % [(hex(a), v) for a, v in diff])
        region = case['region']
        endings['fault' if want['fault'] is not None else want['result'][0]] += 1
        outcomes['mismatch' if problems else 'match'] += 1
        if problems:
            for p in problems[:2]:
                problems_by_kind[p.split(' ')[0]] += 1
            bad_regions.setdefault(region.name, (region.thumb, problems[:4]))
    report = {
        'regions': len(sample),
        'thumb_regions': sum(1 for r in sample if r.thumb),
        'instructions': stats['instructions_emitted'],
        'interpreted': stats['interpreted_instructions'],
        'interpreted_by_reason': stats['interpreted_by_reason'][:10],
        'cases': len(cases),
        'timeouts_skipped': timeouts,
        'outcomes': dict(outcomes),
        'reference_endings': dict(endings),
        'instructions_run_per_case': {'mean': sum(steps) / max(1, len(steps)), 'max': max(steps or [0]),
                                      'total': sum(steps)},
        'problem_kinds': problems_by_kind.most_common(),
        'regions_with_mismatches': len(bad_regions),
        'samples': list(bad_regions.items())[:20],
    }
    (work / 'report.json').write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()

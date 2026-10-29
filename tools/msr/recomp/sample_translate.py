"""Recompilation sample: translates Samus Returns ARM functions to C++ with TriAevum's
true-AOT emitter (../TriAevum), and checks each against Unicorn emulating the 3DS's ARM11
MPCore on random inputs: same registers, flags, VFP registers, memory writes and faults.

Also measures, over the whole program, how many instructions TriAevum's translators accept.

Usage: python tools/msr/recomp/sample_translate.py <dir from ctr.py> [--functions N]
       [--cases K] [--triaevum PATH]
Needs capstone, unicorn and numpy, and MSYS2's g++ on PATH. Writes into <dir>/sample/.
"""
import argparse
import collections
import json
import pathlib
import random
import struct
import subprocess
import sys

import numpy as np
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_WRITE, \
    UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL
from unicorn import arm_const as U

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import analyze  # noqa: E402

RAM_BASE = 0x08000000
RAM_WORDS = 0x8000  # 128 KiB
RAM_SIZE = RAM_WORDS * 4
MAGIC_RETURN = 0x0FFFF000
FPSCR = 0x03C00000  # what the 3DS runs applications with: flush-to-zero, default NaN
STEP_LIMIT = 200_000


def ram_image(seed):
    """Deterministic RAM: a mix of pointers into RAM, small integers, random words and floats
    near 1.0. The C++ harness generates the same image."""
    i = np.arange(RAM_WORDS, dtype=np.uint64)
    with np.errstate(over='ignore'):
        z = np.uint64(seed) * np.uint64(0x100000) + i + np.uint64(0x9E3779B97F4A7C15)
        z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
        z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
        z = z ^ (z >> np.uint64(31))
    kind = z & np.uint64(3)
    pointer = np.uint64(RAM_BASE) + ((z >> np.uint64(8)) % np.uint64(RAM_WORDS)) * np.uint64(4)
    small = (z >> np.uint64(8)) & np.uint64(0xFF)
    word = (z >> np.uint64(32)) & np.uint64(0xFFFFFFFF)
    floating = np.uint64(0x3F800000) + ((z >> np.uint64(8)) & np.uint64(0x7FFFFF))
    out = np.where(kind == 0, pointer, np.where(kind == 1, small, np.where(kind == 2, word, floating)))
    return out.astype('<u4').tobytes()


HARNESS = r'''
#include "oot3d_a32_true_aot_generated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

using namespace oot3d::recomp;

static uint64_t Mix(uint64_t x) {
    uint64_t z = x + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct Bus final : a32::MemoryBus {
    std::vector<uint8_t> code, ram;
    uint32_t code_base = 0, ram_base = 0;
    std::map<uint32_t, bool> written;
    uint8_t* At(uint32_t a, uint32_t n) {
        if (a >= code_base && a - code_base + n <= code.size()) return &code[a - code_base];
        if (a >= ram_base && a - ram_base + n <= ram.size()) return &ram[a - ram_base];
        return nullptr;
    }
    template <typename T> bool Rd(uint32_t a, T* v) {
        uint8_t* p = At(a, sizeof(T)); if (!p) return false; std::memcpy(v, p, sizeof(T)); return true;
    }
    template <typename T> bool Wr(uint32_t a, T v) {
        uint8_t* p = At(a, sizeof(T)); if (!p) return false; std::memcpy(p, &v, sizeof(T));
        for (uint32_t i = 0; i < sizeof(T); ++i) written[a + i] = true;
        return true;
    }
    bool Read32(uint32_t a, uint32_t* v) override { return Rd(a, v); }
    bool Write32(uint32_t a, uint32_t v) override { return Wr(a, v); }
    bool Read8(uint32_t a, uint8_t* v) override { return Rd(a, v); }
    bool Read16(uint32_t a, uint16_t* v) override { return Rd(a, v); }
    bool Write8(uint32_t a, uint8_t v) override { return Wr(a, v); }
    bool Write16(uint32_t a, uint16_t v) override { return Wr(a, v); }
    bool Read64(uint32_t a, uint64_t* v, uint32_t* f) override {
        if (Rd(a, v)) return true; *f = a; return false;
    }
    bool Write64(uint32_t a, uint64_t v, uint32_t* f) override {
        if (Wr(a, v)) return true; *f = a; return false;
    }
};

int main(int argc, char** argv) {
    // argv: code.bin cases.bin results.bin code_base ram_base ram_words
    FILE* f = std::fopen(argv[1], "rb");
    std::vector<uint8_t> code;
    { uint8_t buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) code.insert(code.end(), buf, buf + n); }
    std::fclose(f);
    const uint32_t code_base = std::strtoul(argv[4], nullptr, 0);
    const uint32_t ram_base = std::strtoul(argv[5], nullptr, 0);
    const uint32_t ram_words = std::strtoul(argv[6], nullptr, 0);
    FILE* in = std::fopen(argv[2], "rb");
    FILE* out = std::fopen(argv[3], "wb");
    uint32_t count = 0;
    std::fread(&count, 4, 1, in);
    for (uint32_t c = 0; c < count; ++c) {
        uint32_t entry; uint64_t seed; uint32_t regs[16], cpsr, fpscr, vfp[32];
        std::fread(&entry, 4, 1, in); std::fread(&seed, 8, 1, in);
        std::fread(regs, 4, 16, in); std::fread(&cpsr, 4, 1, in); std::fread(&fpscr, 4, 1, in);
        std::fread(vfp, 4, 32, in);
        Bus bus;
        bus.code = code; bus.code_base = code_base; bus.ram_base = ram_base;
        bus.ram.resize(ram_words * 4);
        for (uint32_t i = 0; i < ram_words; ++i) {
            const uint64_t z = Mix(seed * 0x100000ULL + i);
            uint32_t w;
            switch (z & 3) {
            case 0: w = ram_base + uint32_t(((z >> 8) % ram_words) * 4); break;
            case 1: w = uint32_t((z >> 8) & 0xFF); break;
            case 2: w = uint32_t(z >> 32); break;
            default: w = 0x3F800000U + uint32_t((z >> 8) & 0x7FFFFF); break;
            }
            std::memcpy(&bus.ram[i * 4], &w, 4);
        }
        a32::GuestState state{};
        for (int i = 0; i < 16; ++i) state.r[i] = regs[i];
        state.cpsr = cpsr; state.fpscr = fpscr;
        for (int i = 0; i < 32; ++i) state.vfp[i] = vfp[i];
        a32::ExecutionResult result{};
        GeneratedTrueAotRunStats stats{};
        uint32_t pc = entry;
        bool known = true;
        // Leaf functions: the first exit is the return (or a fault).
        known = ExecuteA32GeneratedTrueAotBlock(pc, state, bus, &result, &stats);
        uint32_t header[3] = {known ? uint32_t(result.kind) : 0xFFU, result.pc, result.detail};
        std::fwrite(header, 4, 3, out);
        std::fwrite(state.r.data(), 4, 16, out);
        std::fwrite(&state.cpsr, 4, 1, out); std::fwrite(&state.fpscr, 4, 1, out);
        std::fwrite(state.vfp.data(), 4, 32, out);
        const uint32_t n = uint32_t(bus.written.size());
        std::fwrite(&n, 4, 1, out);
        for (const auto& [address, unused] : bus.written) {
            uint8_t* p = bus.At(address, 1);
            const uint32_t pair[2] = {address, p ? *p : 0U};
            std::fwrite(pair, 4, 2, out);
        }
    }
    std::fclose(out);
    return 0;
}
'''


class Reference:
    """Unicorn as the ARM11 reference, reused across cases."""

    def __init__(self, code, base):
        self.base = base
        self.code = code
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(U.UC_CPU_ARM_11MPCORE)
        size = (len(code) + 0xFFF) & ~0xFFF
        self.uc.mem_map(base, size, UC_PROT_ALL)
        self.uc.mem_write(base, code)
        self.uc.mem_map(RAM_BASE, RAM_SIZE, UC_PROT_ALL)
        self.uc.reg_write(U.UC_ARM_REG_C1_C0_2, self.uc.reg_read(U.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        self.uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
        self.writes = set()
        self.fault = None
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.on_unmapped)

    def on_write(self, uc, access, address, size, value, data):
        # Attempts on unmapped memory are reported too, before the fault: keep real writes.
        if self.base <= address < self.base + len(self.code) or RAM_BASE <= address < RAM_BASE + RAM_SIZE:
            self.writes.update(range(address, address + size))

    def on_unmapped(self, uc, access, address, size, value, data):
        self.fault = address
        return False

    def run(self, case):
        uc = self.uc
        uc.mem_write(RAM_BASE, ram_image(case['seed']))
        uc.reg_write(U.UC_ARM_REG_CPSR, case['cpsr'])
        for i, reg in enumerate(REGS):
            uc.reg_write(reg, case['regs'][i])
        uc.reg_write(U.UC_ARM_REG_FPSCR, case['fpscr'])
        for i in range(32):
            uc.reg_write(U.UC_ARM_REG_S0 + i, case['vfp'][i])
        self.writes = set()
        self.fault = None
        status = 'return'
        try:
            uc.emu_start(case['regs'][15], MAGIC_RETURN, count=STEP_LIMIT)
            if uc.reg_read(U.UC_ARM_REG_PC) != MAGIC_RETURN:
                status = 'timeout'
        except UcError:
            status = 'fault'
        result = {
            'status': status,
            'pc': uc.reg_read(U.UC_ARM_REG_PC),
            'fault': self.fault,
            'regs': [uc.reg_read(reg) for reg in REGS],
            'cpsr': uc.reg_read(U.UC_ARM_REG_CPSR),
            'fpscr': uc.reg_read(U.UC_ARM_REG_FPSCR),
            'vfp': [uc.reg_read(U.UC_ARM_REG_S0 + i) for i in range(32)],
            'writes': {a: uc.mem_read(a, 1)[0] for a in self.writes},
        }
        # Undo writes to the game's data so the next case starts clean.
        for a in self.writes:
            if self.base <= a < self.base + len(self.code):
                uc.mem_write(a, self.code[a - self.base:a - self.base + 1])
        return result


REGS = [U.UC_ARM_REG_R0 + i for i in range(13)] + [U.UC_ARM_REG_SP, U.UC_ARM_REG_LR, U.UC_ARM_REG_PC]


def leaf_functions(an):
    """ARM functions without calls, system calls or indirect branches, laid out contiguously
    without embedded literal pools: what the true-AOT emitter translates as one region."""
    out = []
    for (entry, thumb) in an.entries:
        if thumb:
            continue
        body, work, ok = set(), [entry], True
        while work and ok:
            a = work.pop()
            if a in body:
                continue
            rec = an.insns.get((a, False))
            if rec is None or rec['kind'] in ('call', 'icall', 'svc', 'ijump', 'table_ldr', 'table_add'):
                ok = False
                break
            body.add(a)
            for target, target_thumb in rec['targets']:
                if target_thumb or ((target, False) in an.entries and target != entry):
                    ok = False  # mode switch or tail call
                work.append(target)
            if not rec['ends']:
                work.append(a + 4)
            if len(body) > 600:
                ok = False
        if not ok or min(body) != entry:
            continue
        end = max(body) + 4
        if len(body) != (end - entry) // 4:
            continue  # literal pool or another function inside
        out.append((entry, end))
    return sorted(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory')
    parser.add_argument('--functions', type=int, default=400)
    parser.add_argument('--cases', type=int, default=12)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--triaevum', default=str(HERE.parents[3] / 'TriAevum'))
    args = parser.parse_args()
    directory = pathlib.Path(args.directory)
    out_dir = directory / 'sample'
    out_dir.mkdir(exist_ok=True)

    tri = pathlib.Path(args.triaevum) / 'tools/oot3d/native_a32_runtime'
    sys.path.insert(0, str(tri))
    sys.path.insert(0, str(tri / 'upstream/src'))
    import generate_true_aot as gta  # noqa: E402
    from oot3d_pack import arm_ir  # noqa: E402

    code = (directory / 'code.bin').read_bytes()
    layout = json.loads((directory / 'layout.json').read_text())
    an = analyze.Analyzer(code, layout)
    an.run()
    base = an.base

    # 1. Whole program: instructions TriAevum's translators accept.
    whole = collections.Counter()
    unsupported = collections.Counter()
    emitter_unsupported = collections.Counter()
    for (addr, thumb), rec in an.insns.items():
        if thumb:
            whole['thumb'] += 1
            continue
        raw = an.word(addr)
        mnemonic = rec['mnemonic'].split('.')[0]
        try:
            ir_ok = arm_ir.arm_core_supported(raw) or arm_ir.arm_vfp_scalar_supported(raw) or \
                arm_ir.arm_vfp_transport_supported(raw)
        except Exception:
            ir_ok = False
        branch = rec['kind'] in ('call', 'jump', 'cjump', 'return', 'icall', 'ijump', 'svc',
                                 'table_ldr', 'table_add')
        whole['arm'] += 1
        if ir_ok or branch:
            whole['ir_supported'] += 1
        else:
            unsupported[mnemonic] += 1
        try:
            gta._emit_instruction(gta.Region(addr, addr + 4, 'probe', (addr,), (raw,)), 0)
            whole['emitter_supported'] += 1
        except ValueError as error:
            if 'whole-program' in str(error):
                whole['emitter_call'] += 1
            else:
                emitter_unsupported[mnemonic] += 1

    # 2. Sample of leaf functions the emitter translates completely.
    leaves = leaf_functions(an)
    translatable, rejected = [], collections.Counter()
    for entry, end in leaves:
        words = tuple(an.word(a) for a in range(entry, end, 4))
        region = gta.Region(entry, end, 'f_%08X' % entry, (entry,), words)
        try:
            for index in range(len(words)):
                gta._emit_instruction(region, index)
        except ValueError as error:
            rejected[str(error).split(' at ')[0][:40]] += 1
            continue
        translatable.append(region)
    rng = random.Random(args.seed)
    sample = sorted(rng.sample(translatable, min(args.functions, len(translatable))), key=lambda r: r.pc)

    (out_dir / gta.HEADER_NAME).write_text(gta._render_header())
    (out_dir / gta.SOURCE_NAME).write_text(gta._render_source(sample))
    (out_dir / 'harness.cpp').write_text(HARNESS)
    runtime = tri / 'upstream'
    sources = [str(out_dir / gta.SOURCE_NAME), str(out_dir / 'harness.cpp')] + \
        [str(p) for p in sorted((runtime / 'recomp').glob('*.cpp'))]
    exe = out_dir / 'harness.exe'
    build = subprocess.run(['g++', '-std=c++20', '-O1', '-w', '-I', str(out_dir), '-I', str(runtime),
                            '-I', str(runtime / 'recomp'), '-o', str(exe)] + sources,
                           capture_output=True, text=True)
    if build.returncode != 0:
        sys.exit('harness build failed:\n' + build.stderr[-4000:])

    # 3. Cases: Unicorn first; only cases that return or fault there go to the C++.
    image = code + bytes(((layout['bss_size'] + 0xFFF) & ~0xFFF))
    ref = Reference(image, base)
    cases, expected = [], []
    timeouts = 0
    for region in sample:
        for k in range(args.cases):
            seed = rng.getrandbits(32)
            r = random.Random(seed)
            regs = []
            for i in range(13):
                choice = r.randrange(4)
                regs.append([RAM_BASE + r.randrange(RAM_WORDS) * 4, r.randrange(64),
                             r.getrandbits(32), 0][choice])
            regs += [RAM_BASE + RAM_SIZE - 0x2000, MAGIC_RETURN, region.pc]
            vfp = [struct.unpack('<I', struct.pack('<f', r.uniform(-100.0, 100.0)))[0] for _ in range(32)]
            case = {'seed': seed, 'regs': regs, 'cpsr': (r.getrandbits(4) << 28) | 0x10,
                    'fpscr': FPSCR, 'vfp': vfp, 'function': region.pc}
            result = ref.run(case)
            if result['status'] == 'timeout':
                timeouts += 1
                continue
            cases.append(case)
            expected.append(result)
    with open(out_dir / 'cases.bin', 'wb') as f:
        f.write(struct.pack('<I', len(cases)))
        for c in cases:
            f.write(struct.pack('<IQ', c['regs'][15], c['seed']))
            f.write(struct.pack('<16I', *c['regs']))
            f.write(struct.pack('<II', c['cpsr'], c['fpscr']))
            f.write(struct.pack('<32I', *c['vfp']))
    image_path = out_dir / 'image.bin'
    image_path.write_bytes(image)
    run = subprocess.run([str(exe), str(image_path), str(out_dir / 'cases.bin'),
                          str(out_dir / 'results.bin'), hex(base), hex(RAM_BASE), str(RAM_WORDS)],
                         capture_output=True, text=True, timeout=1800)
    if run.returncode != 0:
        sys.exit('harness failed: %s' % run.stderr[-2000:])

    # 4. Compare.
    data = (out_dir / 'results.bin').read_bytes()
    off = 0
    outcome = collections.Counter()
    activity = collections.Counter()
    mismatches = []
    bad_functions = set()
    for case, want in zip(cases, expected):
        kind, pc, detail = struct.unpack_from('<3I', data, off); off += 12
        regs = list(struct.unpack_from('<16I', data, off)); off += 64
        cpsr, fpscr = struct.unpack_from('<2I', data, off); off += 8
        vfp = list(struct.unpack_from('<32I', data, off)); off += 128
        (n,) = struct.unpack_from('<I', data, off); off += 4
        writes = {}
        for _ in range(n):
            a, v = struct.unpack_from('<2I', data, off); off += 8
            writes[a] = v
        problems = []
        if want['status'] == 'return':
            if not (kind == 1 and pc == MAGIC_RETURN):
                problems.append('exit %d at 0x%X, expected return' % (kind, pc))
        else:
            if kind != 4:
                problems.append('exit %d, expected fault at 0x%X' % (kind, want['pc']))
            elif detail != want['fault'] or pc != want['pc']:
                problems.append('fault 0x%X at 0x%X, expected 0x%X at 0x%X' % (detail, pc, want['fault'], want['pc']))
        if not problems:
            for i in range(15):
                if regs[i] != want['regs'][i]:
                    problems.append('r%d 0x%X != 0x%X' % (i, regs[i], want['regs'][i]))
            if (cpsr ^ want['cpsr']) & 0xF8000000:
                problems.append('flags 0x%X != 0x%X' % (cpsr >> 27, want['cpsr'] >> 27))
            for i in range(32):
                if vfp[i] != want['vfp'][i]:
                    problems.append('s%d 0x%08X != 0x%08X' % (i, vfp[i], want['vfp'][i]))
            if writes != want['writes']:
                diff = sorted(set(writes.items()) ^ set(want['writes'].items()))[:3]
                problems.append('memory differs (%d vs %d bytes written; %s)' % (
                    len(writes), len(want['writes']), [(hex(a), v) for a, v in diff]))
        outcome[('match' if not problems else 'mismatch', want['status'])] += 1
        if want['regs'][:15] != case['regs'][:15]:
            activity['changed_registers'] += 1
        if want['writes']:
            activity['wrote_memory'] += 1
        if want['vfp'] != case['vfp']:
            activity['changed_vfp_registers'] += 1
        if (want['cpsr'] ^ case['cpsr']) & 0xF0000000:
            activity['changed_flags'] += 1
        if problems:
            bad_functions.add(case['function'])
            if len(mismatches) < 25:
                mismatches.append({'function': '0x%X' % case['function'], 'problems': problems[:4]})

    report = {
        'whole_program': {
            'arm_instructions': whole['arm'],
            'thumb_instructions': whole['thumb'],
            'triaevum_ir_or_control_flow_supported': whole['ir_supported'],
            'triaevum_true_aot_emitter_supported': whole['emitter_supported'],
            'true_aot_emitter_calls_need_whole_program': whole['emitter_call'],
            'ir_unsupported_top': unsupported.most_common(25),
            'emitter_unsupported_top': emitter_unsupported.most_common(25),
        },
        'leaf_functions': len(leaves),
        'leaf_functions_emitter_translates': len(translatable),
        'leaf_rejections': rejected.most_common(10),
        'sampled_functions': len(sample),
        'cases_run': len(cases),
        'cases_timed_out_in_reference': timeouts,
        'outcomes': {'%s/%s' % k: v for k, v in outcome.items()},
        'case_activity': dict(activity),
        'sampled_functions_using_vfp': sum(1 for r in sample if any((w & 0x0E000E00) == 0x0C000A00 for w in r.words)),
        'sampled_instructions': sum(len(r.words) for r in sample),
        'functions_with_mismatches': len(bad_functions),
        'mismatch_samples': mismatches,
    }
    (out_dir / 'report.json').write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()

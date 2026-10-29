"""Static analysis of Samus Returns' code.bin for ahead-of-time recompilation.

Finds the code by recursive descent from the entry point, from every direct call and branch,
from code pointers in literal pools, rodata and data (vtables, callbacks), and through jump
tables, then classifies what a static recompiler has to handle: indirect calls and jumps,
returns, system calls, instruction set use. Optionally compares the result with the code the
emulator really ran (coverage.bin from MSR_DEV=1, see src/msr/code_coverage.h).

Usage: python tools/msr/recomp/analyze.py <dir from ctr.py> [coverage.bin]
Needs capstone. Writes functions.csv and report.json into the directory.
"""
import collections
import csv
import json
import pathlib
import struct
import sys

import capstone
from capstone import arm as A

# ARMv6K (the 3DS's ARM11) lacks these. Capstone decodes ARMv7 and Thumb-2, so an instruction
# from this set means the bytes are not ARM11 code.
NOT_ARMV6K = {
    'movw', 'movt', 'ubfx', 'sbfx', 'bfi', 'bfc', 'rbit', 'mls', 'dmb', 'dsb', 'isb', 'sdiv',
    'udiv', 'tbb', 'tbh', 'cbz', 'cbnz', 'it', 'ite', 'itt', 'itte', 'ittt', 'iteet', 'ldrht',
    'strht', 'ldrsbt', 'ldrsht', 'pldw', 'smc', 'hvc', 'eret', 'lda', 'stl', 'crc32b', 'aese',
    'vrinta', 'vsel', 'vmaxnm', 'vcvta', 'hlt', 'sevl', 'dbg', 'udf',
}
LOAD_MULTIPLE = {getattr(A, name) for name in dir(A)
                 if name.startswith('ARM_INS_LDM') or name == 'ARM_INS_POP'}


class Analyzer:
    def __init__(self, code, layout):
        self.code = code
        self.base = layout['text']['address']
        self.text_end = self.base + layout['text']['size']
        # Whole pages: the init array runs past rodata's declared size into its last page.
        self.ro = (layout['rodata']['address'], layout['rodata']['address'] + layout['rodata']['pages'] * 0x1000)
        self.data = (layout['data']['address'], layout['data']['address'] + layout['data']['size'])
        self.entry = self.base
        self.cs = {}
        for thumb in (False, True):
            md = capstone.Cs(capstone.CS_ARCH_ARM,
                             capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
            md.detail = True
            self.cs[thumb] = md
        self.insns = {}         # (addr, thumb) -> record
        self.code_half = set()  # halfword addresses decoded as code
        self.data_words = set() # word addresses known to be data (literal pools, tables)
        self.entries = {}       # (addr, thumb) -> how it was found
        self.rejected = collections.Counter()
        self.stats = collections.Counter()
        self.indirect = collections.Counter()
        self.svcs = collections.Counter()
        self.mnemonics = {False: collections.Counter(), True: collections.Counter()}
        self.jump_tables = []
        self.table_targets = {}  # (addr, thumb) -> resolved switch targets
        self.unresolved_jumps = []
        self.pointer_candidates = {}

    # -- memory -------------------------------------------------------------------------------
    def in_text(self, addr):
        return self.base <= addr < self.text_end

    def word(self, addr):
        off = addr - self.base
        if 0 <= off <= len(self.code) - 4:
            return struct.unpack_from('<I', self.code, off)[0]
        return None

    # -- decoding -----------------------------------------------------------------------------
    def decode(self, addr, thumb):
        """Decodes one instruction to a record, or None if the bytes are not ARM11 code."""
        key = (addr, thumb)
        if key in self.insns:
            return self.insns[key]
        off = addr - self.base
        if not self.in_text(addr) or (addr & (1 if thumb else 3)):
            return None
        insn = next(self.cs[thumb].disasm(self.code[off:off + 4], addr, 1), None)
        if insn is None:
            return None
        mnem = insn.mnemonic.split('.')[0]
        if mnem in NOT_ARMV6K or (thumb and insn.size == 4 and mnem not in ('bl', 'blx')):
            return None
        if not self.plausible(insn, mnem):
            return None
        rec = self.classify(insn, thumb)
        return rec

    @staticmethod
    def plausible(insn, mnem):
        """Rejects encodings that are valid ARM but never appear in 3DS application code, which
        is how text decoded as instructions gives itself away."""
        if mnem.startswith('svc'):
            ops = insn.operands
            return bool(ops) and 0x01 <= ops[0].imm <= 0x7D
        if mnem.startswith(('cdp', 'ldc', 'stc', 'mcrr', 'mrrc')):
            return False
        if mnem.startswith(('mrc', 'mcr')):
            return insn.op_str.startswith('p15,')
        return True

    def classify(self, insn, thumb):
        """Control flow of one instruction: kind, direct targets, literal and table data."""
        addr, size = insn.address, insn.size
        cond_always = insn.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID)
        ops = insn.operands
        reads, writes = insn.regs_access()
        writes_pc = A.ARM_REG_PC in writes
        kind = 'seq'
        targets = []
        calls = []
        literals = []
        table = None
        iid = insn.id
        pc_value = (addr + 4) & ~3 if thumb else addr + 8

        # PC-relative literal loads: data in the text section, possibly code pointers.
        wide = insn.mnemonic.startswith('ldrd') or (
            insn.mnemonic.startswith('vldr') and insn.op_str.startswith('d'))
        for op in ops:
            if op.type == A.ARM_OP_MEM and op.mem.base == A.ARM_REG_PC and op.mem.index == 0:
                disp = -op.mem.disp if getattr(op, 'subtracted', False) else op.mem.disp
                literals.append((pc_value + disp, insn.mnemonic, 8 if wide else 4))

        # Registers set to a constant address: `add rN, pc, #imm` / `adr` and literal loads.
        # Lets `bx rN` / `blx rN` in veneers and mode switches resolve.
        const = None
        if ops and ops[0].type == A.ARM_OP_REG and cond_always:
            if iid in (A.ARM_INS_ADD, A.ARM_INS_ADR) and len(ops) == 3 and ops[1].type == A.ARM_OP_REG \
                    and ops[1].reg == A.ARM_REG_PC and ops[2].type == A.ARM_OP_IMM:
                const = (ops[0].reg, pc_value + ops[2].imm)
            elif iid == A.ARM_INS_ADR and len(ops) == 2 and ops[1].type == A.ARM_OP_IMM:
                const = (ops[0].reg, ops[1].imm if ops[1].imm >= self.base else pc_value + ops[1].imm)
            elif iid == A.ARM_INS_LDR and literals and len(ops) == 2:
                value = self.word(literals[0][0] & ~3)
                if value is not None:
                    const = (ops[0].reg, value)

        if iid in (A.ARM_INS_BL, A.ARM_INS_BLX) and ops and ops[0].type == A.ARM_OP_IMM:
            target_thumb = thumb if iid == A.ARM_INS_BL else not thumb
            calls.append((ops[0].imm, target_thumb))
            kind = 'call'
        elif iid == A.ARM_INS_B and ops and ops[0].type == A.ARM_OP_IMM:
            targets.append((ops[0].imm, thumb))
            kind = 'jump' if cond_always else 'cjump'
        elif iid == A.ARM_INS_BLX:
            kind = 'icall'
        elif iid == A.ARM_INS_BX:
            reg = ops[0].reg if ops else 0
            if reg == A.ARM_REG_LR:
                kind = 'return'
            elif reg == A.ARM_REG_PC and thumb:
                # Thumb `bx pc`: continue in ARM mode at the next word.
                targets.append(((addr + 4) & ~3, False))
                kind = 'jump' if cond_always else 'cjump'
            else:
                kind = 'ijump'
        elif writes_pc:
            if iid in LOAD_MULTIPLE:
                base = ops[0].reg if iid != A.ARM_INS_POP else A.ARM_REG_SP
                kind = 'return' if iid == A.ARM_INS_POP or base == A.ARM_REG_SP else 'ijump'
            elif iid == A.ARM_INS_LDR and len(ops) >= 2 and ops[1].type == A.ARM_OP_MEM:
                mem = ops[1].mem
                if mem.base == A.ARM_REG_SP:
                    kind = 'return'
                elif mem.base == A.ARM_REG_PC and mem.index != 0:
                    kind = 'table_ldr'
                    table = pc_value
                else:
                    kind = 'ijump'
            elif iid in (A.ARM_INS_MOV,) and len(ops) >= 2 and ops[1].type == A.ARM_OP_REG:
                kind = 'return' if ops[1].reg == A.ARM_REG_LR else 'ijump'
            elif (iid == A.ARM_INS_ADD and len(ops) >= 3 and ops[1].type == A.ARM_OP_REG
                  and ops[1].reg == A.ARM_REG_PC and ops[2].type == A.ARM_OP_REG):
                kind = 'table_add'
                table = pc_value
            else:
                kind = 'ijump'
        elif iid == A.ARM_INS_SVC:
            kind = 'svc'

        ends = kind in ('jump', 'return', 'ijump', 'table_ldr', 'table_add') and cond_always
        rec = {
            'size': size, 'kind': kind, 'ends': ends, 'targets': targets, 'calls': calls,
            'literals': literals, 'table': table, 'mnemonic': insn.mnemonic, 'op_str': insn.op_str,
            'svc': ops[0].imm if kind == 'svc' and ops else None,
            'cmp': (ops[0].reg, ops[1].imm) if insn.mnemonic.startswith('cmp') and len(ops) == 2
            and ops[1].type == A.ARM_OP_IMM else None,
            'index_reg': None,
            'const': const,
            'reg': ops[0].reg if iid in (A.ARM_INS_BX, A.ARM_INS_BLX) and ops
            and ops[0].type == A.ARM_OP_REG else None,
            'writes': tuple(writes),
        }
        if kind == 'table_ldr':
            rec['index_reg'] = ops[1].mem.index
        elif kind == 'table_add':
            rec['index_reg'] = ops[2].reg
        return rec

    # -- exploration --------------------------------------------------------------------------
    def explore(self, start, thumb, origin, speculative=True):
        """Decodes everything reachable from an entry. For a pointer candidate (speculative), if
        a reachable instruction is not ARM11 code or collides with known data, nothing is kept.
        The path after a call or a system call is soft: the callee may never return (asserts,
        panics), with data after the call, so a bad decode there only ends that path."""
        new = {}
        new_entries = {(start, thumb): origin}
        new_data = set()
        work = [(start, thumb, False)]
        tables = []
        unresolved = []
        while work:
            addr, th, soft = work.pop()
            key = (addr, th)
            if key in self.insns or key in new:
                continue
            bad = False
            halves = self.halves(addr, 2 if th else 4)
            if any(h in self.code_half for h in halves):
                bad = True  # overlaps code decoded at another alignment or in the other mode
            elif (addr & ~3) in self.data_words or (addr & ~3) in new_data:
                bad = True
            rec = None if bad else self.decode(addr, th)
            if rec is None:
                if speculative and not soft:
                    return None
                self.stats['stopped_paths'] += 1
                continue
            new[key] = rec
            for lit_addr, mnem, size in rec['literals']:
                new_data.update(range(lit_addr & ~3, lit_addr + size, 4))
            if rec['reg'] is not None and rec['kind'] in ('ijump', 'icall'):
                target = self.register_constant(addr, th, rec['reg'], new)
                if target is not None:
                    resolved = (target & ~1, bool(target & 1))
                    self.stats['resolved_register_branches'] += 1
                    if rec['kind'] == 'icall':
                        rec = dict(rec, kind='call', calls=[resolved])
                    else:
                        rec = dict(rec, kind='jump' if rec['ends'] else 'cjump', targets=[resolved])
                        new_entries.setdefault(resolved, 'veneer')
                    new[key] = rec
            for target in rec['targets']:
                work.append((target[0], target[1], soft))
            for target in rec['calls']:
                new_entries.setdefault(target, 'call')
                work.append((target[0], target[1], soft))
            if rec['kind'] in ('svc', 'call'):
                work.append((addr + rec['size'], th, True))
            elif not rec['ends']:
                work.append((addr + rec['size'], th, soft))
            if rec['table']:
                entries = self.table_entries(addr, th, rec, new)
                if entries is None:
                    unresolved.append((addr, th, rec['mnemonic'], rec['op_str']))
                else:
                    tables.append((addr, rec['kind'], entries))
                    new_data.update(entries['data'])
                    for target in entries['targets']:
                        work.append((target[0], target[1], soft))
        self.unresolved_jumps.extend(unresolved)
        # Commit.
        for key, rec in new.items():
            self.insns[key] = rec
            self.code_half.update(self.halves(key[0], rec['size']))
            for lit_addr, _, size in rec['literals']:
                self.data_words.update(range(lit_addr & ~3, lit_addr + size, 4))
        for addr, kind, entries in tables:
            self.jump_tables.append((addr, kind, len(entries['targets'])))
            self.table_targets[(addr, False)] = list(entries['targets'])
            self.data_words.update(entries['data'])
        for key, how in new_entries.items():
            self.entries.setdefault(key, how)
        return new

    def register_constant(self, addr, thumb, reg, pending):
        """The constant a register holds at a branch, if set just before it."""
        step = 2 if thumb else 4
        for back in range(1, 4):
            prev = self.insns.get((addr - step * back, thumb)) or pending.get((addr - step * back, thumb))
            if prev is None:
                return None
            if prev['const'] and prev['const'][0] == reg:
                value = prev['const'][1]
                return value if self.in_text(value & ~1) else None
            if reg in prev['writes'] or prev['kind'] != 'seq':
                return None
        return None

    def halves(self, addr, size):
        return range(addr & ~1, addr + size, 2)

    def table_entries(self, addr, thumb, rec, pending):
        """Targets of an ARM switch: `cmp rN, #n` then `ldrls pc, [pc, rN, lsl #2]` (a table
        of addresses) or `addls pc, pc, rN, lsl #2` (a table of branches)."""
        # The bound: `ls`/`hi` admit index n (n + 1 cases), `lo`/`cc`/`hs`/`cs` stop before it.
        # The condition is on the table instruction, or on the guarding branch before it.
        inclusive = None
        mnemonic = rec['mnemonic']
        if mnemonic.endswith(('ls',)):
            inclusive = True
        elif mnemonic.endswith(('lo', 'cc')):
            inclusive = False
        count = None
        for back in range(1, 24):
            prev = self.insns.get((addr - 4 * back, thumb)) or pending.get((addr - 4 * back, thumb))
            if prev is None or prev['kind'] not in ('seq', 'cjump'):
                break
            if inclusive is None and prev['mnemonic'] in ('bhi', 'bhs', 'bcs'):
                inclusive = prev['mnemonic'] == 'bhi'
            if prev['cmp'] and prev['cmp'][0] == rec['index_reg']:
                if inclusive is not None:
                    count = prev['cmp'][1] + (1 if inclusive else 0)
                break
            if rec['index_reg'] in prev['writes']:
                break  # the index changes after any bound check: give up
        if count is None or count > 4096:
            return None
        start = rec['table']
        if rec['kind'] == 'table_ldr':
            targets, data = [], []
            for i in range(count):
                value = self.word(start + 4 * i)
                if value is None or not self.in_text(value & ~1):
                    return None
                targets.append((value & ~1, bool(value & 1)))
                data.append(start + 4 * i)
            return {'targets': targets, 'data': data}
        # Branch table: each slot is a `b` instruction, decoded as code.
        return {'targets': [(start + 4 * i, False) for i in range(count)], 'data': []}

    # -- pointers -----------------------------------------------------------------------------
    def pointer_seeds(self):
        """Words in rodata, data and literal pools that point into the text section."""
        seeds = {}
        for lo, hi, where in ((self.ro[0], self.ro[1], 'rodata'), (self.data[0], self.data[1], 'data')):
            for addr in range(lo, hi - 3, 4):
                value = self.word(addr)
                if value is not None and self.in_text(value & ~1) and value != 0:
                    seeds.setdefault((value & ~1, bool(value & 1)), where)
        return seeds

    def init_array_seeds(self):
        """C++ static constructors: ARMCC's init array holds offsets relative to each entry.
        Found as runs of 16+ consecutive words in rodata/data whose relative target is code."""
        seeds = {}
        self.init_arrays = []
        for lo, hi in (self.ro, self.data):
            run_start = None
            for addr in range(lo, hi + 4, 4):
                value = self.word(addr) if addr <= hi - 4 else None
                ok = value is not None and self.in_text((addr + value) & 0xFFFFFFFF & ~1)
                if ok and run_start is None:
                    run_start = addr
                elif not ok and run_start is not None:
                    if (addr - run_start) // 4 >= 16:
                        self.init_arrays.append((run_start, (addr - run_start) // 4))
                        for a in range(run_start, addr, 4):
                            target = (a + self.word(a)) & 0xFFFFFFFF
                            seeds.setdefault((target & ~1, bool(target & 1)), 'init_array')
                    run_start = None
        return seeds

    def literal_seeds(self):
        seeds = {}
        for rec in list(self.insns.values()):
            for lit_addr, mnem, _ in rec['literals']:
                if not mnem.startswith('ldr') or mnem.startswith(('ldrd', 'ldrb', 'ldrh', 'ldrs')):
                    continue
                value = self.word(lit_addr & ~3)
                if value is not None and self.in_text(value & ~1):
                    seeds.setdefault((value & ~1, bool(value & 1)), 'literal')
        return seeds

    def vtables(self):
        """Runs of 3+ consecutive code entries in rodata/data: C++ vtables and callback tables."""
        runs, count, total = [], 0, 0
        for lo, hi in (self.ro, self.data):
            run = 0
            for addr in range(lo, hi - 3, 4):
                value = self.word(addr)
                ok = value is not None and (value & ~1, bool(value & 1)) in self.entries
                if ok:
                    run += 1
                else:
                    if run >= 3:
                        count += 1
                        total += run
                    run = 0
        return count, total

    extra_files = []  # more entry lists, e.g. user/msr/recomp-jit-entries-core0.txt

    def extra_entries(self):
        """Entry points found at run time: extra_entries.txt beside this script, and any
        extra_files. One address per line (first word); an odd address is Thumb."""
        out = []
        for path in [pathlib.Path(__file__).with_name('extra_entries.txt')] + list(self.extra_files):
            path = pathlib.Path(path)
            if not path.exists():
                continue
            for line in path.read_text().splitlines():
                line = line.split('#')[0].strip()
                if line:
                    value = int(line.split()[0], 0)
                    out.append((value & ~1, bool(value & 1)))
        return out

    def run(self):
        self.explore(self.entry, False, 'entry', speculative=False)
        for addr, thumb in self.extra_entries():
            self.explore(addr, thumb, 'runtime', speculative=False)
        rounds = 0
        while True:
            rounds += 1
            before = len(self.insns)
            seeds = {}
            seeds.update(self.init_array_seeds())
            seeds.update(self.pointer_seeds())
            seeds.update(self.literal_seeds())
            for key, where in seeds.items():
                if key in self.insns:
                    self.entries.setdefault(key, where)
                    continue
                if key in self.pointer_candidates and self.pointer_candidates[key] == 'rejected':
                    continue
                if self.explore(key[0], key[1], where) is None:
                    self.pointer_candidates[key] = 'rejected'
                    self.rejected[where] += 1
                else:
                    self.pointer_candidates[key] = 'accepted'
            if len(self.insns) == before or rounds > 8:
                break
        return rounds

    # -- report -------------------------------------------------------------------------------
    def report(self, coverage=None):
        text_bytes = self.text_end - self.base
        code_bytes = sum(r['size'] for r in self.insns.values())
        data_bytes = 4 * len([w for w in self.data_words if self.in_text(w)])
        kinds = collections.Counter(r['kind'] for r in self.insns.values())
        thumb_insns = sum(1 for (a, t) in self.insns if t)
        for (addr, thumb), rec in self.insns.items():
            self.mnemonics[thumb][rec['mnemonic'].split('.')[0]] += 1
            if rec['svc'] is not None:
                self.svcs['0x%02X' % rec['svc']] += 1
        # Unexplained gaps in the text section, and prologues inside them.
        known = set(self.code_half)
        for w in self.data_words:
            known.update((w, w + 2))
        gap_bytes, gap_prologues, gaps = 0, 0, []
        addr = self.base
        while addr < self.text_end:
            if addr in known:
                addr += 2
                continue
            start = addr
            while addr < self.text_end and addr not in known:
                addr += 2
            size = addr - start
            zero = all(b == 0 for b in self.code[start - self.base:addr - self.base])
            if not zero:
                gap_bytes += size
                gaps.append((start, size))
                for a in range((start + 3) & ~3, addr - 3, 4):
                    w = self.word(a)
                    # push {..., lr} / stmdb sp!, {..., lr}
                    if w is not None and (w & 0x0FFF4000) == 0x092D4000:
                        gap_prologues += 1
        report = {
            'text_bytes': text_bytes,
            'code_bytes': code_bytes,
            'text_data_bytes': data_bytes,
            'unexplained_bytes': gap_bytes,
            'unexplained_regions': len(gaps),
            'prologues_in_unexplained': gap_prologues,
            'largest_unexplained': [('0x%X' % a, s) for a, s in sorted(gaps, key=lambda g: -g[1])[:12]],
            'instructions': len(self.insns),
            'thumb_instructions': thumb_insns,
            'functions': len(self.entries),
            'functions_by_source': collections.Counter(self.entries.values()),
            'pointer_candidates_rejected': dict(self.rejected),
            'control_flow': dict(kinds),
            'init_arrays': [('0x%X' % a, n) for a, n in getattr(self, 'init_arrays', [])],
            'jump_tables': len(self.jump_tables),
            'jump_table_cases': sum(n for _, _, n in self.jump_tables),
            'analysis_stats': dict(self.stats),
            'unresolved_indirect_jumps': len(self.unresolved_jumps),
            'unresolved_jump_samples': [('0x%X' % a, t, m, o) for a, t, m, o in self.unresolved_jumps[:20]],
            'vtables': self.vtables(),
            'svc': dict(self.svcs.most_common()),
            'arm_mnemonics': len(self.mnemonics[False]),
            'thumb_mnemonics': len(self.mnemonics[True]),
            'top_arm': self.mnemonics[False].most_common(25),
            'top_thumb': self.mnemonics[True].most_common(15),
            'vfp_instructions': sum(n for m, n in self.mnemonics[False].items() if m.startswith('v')),
            'exclusive_instructions': sum(n for m, n in self.mnemonics[False].items()
                                          if m.startswith(('ldrex', 'strex', 'clrex'))),
            'coprocessor': {m: n for m, n in self.mnemonics[False].items()
                            if m.startswith(('mrc', 'mcr', 'mrrc', 'mcrr', 'cdp', 'ldc', 'stc'))},
            'system': {m: n for m, n in self.mnemonics[False].items()
                       if m.startswith(('mrs', 'msr', 'cps', 'srs', 'rfe', 'wfi', 'wfe', 'yield', 'sev', 'bkpt', 'swp'))},
        }
        if coverage is not None:
            report['runtime'] = self.compare(coverage)
        return report

    def compare(self, coverage):
        words, outside = coverage
        executed = [self.base + 4 * i for i, hit in enumerate(words)
                    if hit and self.in_text(self.base + 4 * i)]
        static_words = {h & ~3 for h in self.code_half}
        missed = [w for w in executed if w not in static_words]
        # Group the misses into contiguous regions.
        regions = []
        for w in missed:
            if regions and w == regions[-1][1]:
                regions[-1][1] = w + 4
            else:
                regions.append([w, w + 4])
        return {
            'executed_words': len(executed),
            'missed_words': len(missed),
            'executed_found_statically': 1 - len(missed) / max(1, len(executed)),
            'missed_regions': len(regions),
            'missed_samples': [('0x%X' % a, b - a) for a, b in regions[:25]],
            'executed_outside_text_pages': ['0x%X' % p for p in outside[:40]],
            'executed_outside_text_page_count': len(outside),
            'static_code_executed_fraction': len(executed) / max(1, len(static_words)),
        }

    def write_functions(self, path):
        with open(path, 'w', newline='') as f:
            out = csv.writer(f)
            out.writerow(['address', 'mode', 'found_by'])
            for (addr, thumb), how in sorted(self.entries.items()):
                out.writerow(['0x%08X' % addr, 'thumb' if thumb else 'arm', how])


def load_coverage(path):
    data = pathlib.Path(path).read_bytes()
    assert data[:8] == b'MSRCOV1\0'
    base, count = struct.unpack_from('<II', data, 8)
    words = data[16:16 + count]
    (n,) = struct.unpack_from('<I', data, 16 + count)
    outside = list(struct.unpack_from('<%dI' % n, data, 20 + count))
    return words, outside


def main(directory, coverage_path=None):
    directory = pathlib.Path(directory)
    code = (directory / 'code.bin').read_bytes()
    layout = json.loads((directory / 'layout.json').read_text())
    analyzer = Analyzer(code, layout)
    rounds = analyzer.run()
    coverage = load_coverage(coverage_path) if coverage_path else None
    report = analyzer.report(coverage)
    report['seed_rounds'] = rounds
    (directory / 'report.json').write_text(json.dumps(report, indent=1, default=str))
    analyzer.write_functions(directory / 'functions.csv')
    print(json.dumps(report, indent=1, default=str))


if __name__ == '__main__':
    main(*sys.argv[1:3])

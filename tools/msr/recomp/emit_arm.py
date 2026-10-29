"""ARM (A32, ARMv6K + VFPv2) instruction translation to C++ for the AOT runtime
(src/core/arm/msr_aot/aot_runtime.h). Decodes raw encodings.

emit(pc, raw, flow) returns C++ statements. `flow` supplies control transfers:
    flow.jump(target, thumb)  -> statement for a direct branch (goto or exit)
    flow.exit()               -> statement leaving the function (r[15] and thumb already set)
    flow.dispatch()           -> statement continuing at the computed r[15] inside the function
Raises Unsupported for encodings left to the interpreter.
"""


class Unsupported(Exception):
    pass


COND = ['c.z', '!c.z', 'c.c', '!c.c', 'c.n', '!c.n', 'c.v', '!c.v', '(c.c && !c.z)',
        '(!c.c || c.z)', '(c.n == c.v)', '(c.n != c.v)', '(!c.z && c.n == c.v)',
        '(c.z || c.n != c.v)', None, None]


def bits(raw, hi, lo):
    return (raw >> lo) & ((1 << (hi - lo + 1)) - 1)


def bit(raw, n):
    return (raw >> n) & 1


def ror32(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


def hexu(v):
    return '0x%08Xu' % (v & 0xFFFFFFFF)


def reg(n, pc, extra=8):
    """A register read; PC reads as the instruction address + 8 (+12 for register shifts)."""
    return hexu(pc + extra) if n == 15 else 'c.r[%d]' % n


def wrap(cond, lines):
    if cond == 14:
        return lines
    if cond == 15:
        raise Unsupported('unconditional space')
    return ['if (%s) {' % COND[cond]] + ['    ' + l for l in lines] + ['}']


def shift_imm(v, typ, amount):
    """Immediate shift of expression v: (value, carry) expressions."""
    if typ == 0:
        if amount == 0:
            return v, 'c.c'
        return '(%s << %d)' % (v, amount), '((%s >> %d) & 1u)' % (v, 32 - amount)
    if typ == 1:
        if amount == 0:
            return '0u', '(%s >> 31)' % v
        return '(%s >> %d)' % (v, amount), '((%s >> %d) & 1u)' % (v, amount - 1)
    if typ == 2:
        if amount == 0:
            return '(u32)((s32)%s >> 31)' % v, '(%s >> 31)' % v
        return '(u32)((s32)%s >> %d)' % (v, amount), '((%s >> %d) & 1u)' % (v, amount - 1)
    if amount == 0:  # RRX
        return '((c.c << 31) | (%s >> 1))' % v, '(%s & 1u)' % v
    return 'Ror(%s, %d)' % (v, amount), '((%s >> %d) & 1u)' % (v, amount - 1)


class Arm:
    def __init__(self, pc, raw, flow):
        self.pc, self.raw, self.flow = pc, raw, flow

    def emit(self):
        raw = self.raw
        cond = raw >> 28
        if cond == 15:
            return self.unconditional()
        op = bits(raw, 27, 25)
        if op == 0b101:
            return self.branch(cond)
        if op == 0b100:
            return wrap(cond, self.block_transfer())
        if op in (0b010, 0b011):
            if op == 0b011 and bit(raw, 4):
                return wrap(cond, self.media())
            return wrap(cond, self.single_transfer())
        if op == 0b111:
            if bit(raw, 24):
                return wrap(cond, self.svc())
            return wrap(cond, self.coprocessor())
        if op == 0b110:
            return wrap(cond, self.coprocessor_transfer())
        if op == 0b001:
            if bits(raw, 24, 23) == 0b10 and not bit(raw, 20):
                if bit(raw, 21):
                    return wrap(cond, self.msr(imm=True))
                raise Unsupported('movw/movt')
            return wrap(cond, self.data_processing())
        # op == 000
        if bits(raw, 7, 4) == 0b1001:
            if bits(raw, 24, 22) == 0b000 or bits(raw, 24, 23) == 0b01:
                return wrap(cond, self.multiply())
            if bits(raw, 24, 23) == 0b10 and bits(raw, 21, 20) == 0b00:
                raise Unsupported('swp')
            if bits(raw, 24, 23) == 0b11:
                return wrap(cond, self.exclusive())
            if bits(raw, 24, 20) == 0b00100:
                return wrap(cond, self.umaal())
            raise Unsupported('multiply space')
        if bit(raw, 7) and bit(raw, 4):
            return wrap(cond, self.extra_transfer())
        if bits(raw, 24, 23) == 0b10 and not bit(raw, 20):
            return wrap(cond, self.miscellaneous())
        return wrap(cond, self.data_processing())

    # -- branches ------------------------------------------------------------------------------
    def branch(self, cond):
        offset = bits(self.raw, 23, 0)
        if offset & 0x800000:
            offset -= 0x1000000
        target = (self.pc + 8 + offset * 4) & 0xFFFFFFFF
        if bit(self.raw, 24):
            lines = ['c.r[14] = %s;' % hexu(self.pc + 4), self.flow.jump(target, False, call=True)]
        else:
            lines = [self.flow.jump(target, False)]
        return wrap(cond, lines)

    def unconditional(self):
        raw = self.raw
        if bits(raw, 27, 25) == 0b101:  # BLX immediate
            offset = bits(raw, 23, 0)
            if offset & 0x800000:
                offset -= 0x1000000
            target = (self.pc + 8 + offset * 4 + (bit(raw, 24) << 1)) & 0xFFFFFFFF
            return ['c.r[14] = %s;' % hexu(self.pc + 4), self.flow.jump(target, True, call=True)]
        if (raw & 0xFD70F000) == 0xF550F000:  # PLD
            return []
        if raw == 0xF57FF01F:  # CLREX
            return ['c.excl_valid = 0;']
        raise Unsupported('unconditional')

    def svc(self):
        return ['c.r[15] = %s; c.exit = Exit::Svc; c.svc = 0x%Xu;' % (hexu(self.pc + 4), bits(self.raw, 23, 0)),
                self.flow.exit()]

    # -- data processing -----------------------------------------------------------------------
    def operand2(self, need_carry):
        """(prelude lines, value expression, carry expression or None)."""
        raw, pc = self.raw, self.pc
        if bit(raw, 25):
            rot = bits(raw, 11, 8) * 2
            value = ror32(bits(raw, 7, 0), rot)
            carry = ('c.c' if rot == 0 else str(value >> 31) + 'u') if need_carry else None
            return [], hexu(value), carry
        rm = bits(raw, 3, 0)
        typ = bits(raw, 6, 5)
        if bit(raw, 4):
            if bit(raw, 7):
                raise Unsupported('not data processing')
            rs = bits(raw, 11, 8)
            if rs == 15:
                raise Unsupported('shift by pc')
            pre = ['u32 sc; const u32 sv = ShiftReg(%d, %s, c.r[%d], c.c, &sc);' % (typ, reg(rm, pc, 12), rs)]
            return pre, 'sv', 'sc' if need_carry else None
        amount = bits(raw, 11, 7)
        if rm == 15:
            pre = ['const u32 m = %s;' % hexu(pc + 8)]
        else:
            pre = ['const u32 m = c.r[%d];' % rm]
        value, carry = shift_imm('m', typ, amount)
        return pre, value, carry if need_carry else None

    def data_processing(self):
        raw, pc = self.raw, self.pc
        opcode = bits(raw, 24, 21)
        s = bit(raw, 20)
        rn = bits(raw, 19, 16)
        rd = bits(raw, 15, 12)
        logical = opcode in (0, 1, 8, 9, 12, 13, 14, 15)
        pre, op2, carry = self.operand2(need_carry=s and logical)
        a = reg(rn, pc, 12 if (not bit(raw, 25) and bit(raw, 4)) else 8)
        lines = list(pre)
        lines.append('const u32 b = %s;' % op2)
        if opcode not in (13, 15):
            lines.append('const u32 a = %s;' % a)
        if s:
            exprs = {
                0: 'Logical(c, a & b, %s)', 1: 'Logical(c, a ^ b, %s)', 2: 'AddFlags(c, a, ~b, 1)',
                3: 'AddFlags(c, b, ~a, 1)', 4: 'AddFlags(c, a, b, 0)', 5: 'AddFlags(c, a, b, c.c)',
                6: 'AddFlags(c, a, ~b, c.c)', 7: 'AddFlags(c, b, ~a, c.c)', 8: 'Logical(c, a & b, %s)',
                9: 'Logical(c, a ^ b, %s)', 10: 'AddFlags(c, a, ~b, 1)', 11: 'AddFlags(c, a, b, 0)',
                12: 'Logical(c, a | b, %s)', 13: 'Logical(c, b, %s)', 14: 'Logical(c, a & ~b, %s)',
                15: 'Logical(c, ~b, %s)',
            }
            expr = exprs[opcode]
            if '%s' in expr:
                expr = expr % carry
        else:
            expr = {0: 'a & b', 1: 'a ^ b', 2: 'a - b', 3: 'b - a', 4: 'a + b', 5: 'a + b + c.c',
                    6: 'a + ~b + c.c', 7: 'b + ~a + c.c', 12: 'a | b', 13: 'b', 14: 'a & ~b',
                    15: '~b'}.get(opcode)
            if expr is None:
                raise Unsupported('test without S')
        if opcode in (8, 9, 10, 11):
            lines.append('(void)%s;' % expr)
            return ['{'] + ['    ' + l for l in lines] + ['}']
        if rd == 15:
            if s:
                raise Unsupported('exception return')
            lines.append('c.r[15] = (%s) & ~3u; c.thumb = 0;' % expr)
            lines.append(self.flow.exit())
        else:
            lines.append('c.r[%d] = %s;' % (rd, expr))
        return ['{'] + ['    ' + l for l in lines] + ['}']

    def msr(self, imm):
        raw = self.raw
        if bit(raw, 22):
            raise Unsupported('msr spsr')
        mask = bits(raw, 19, 16)
        value = hexu(ror32(bits(raw, 7, 0), bits(raw, 11, 8) * 2)) if imm else 'c.r[%d]' % bits(raw, 3, 0)
        if mask & 0b1000:
            return ['SetCpsrFlags(c, %s, false);' % value]
        return []

    def miscellaneous(self):
        raw, pc = self.raw, self.pc
        op = bits(raw, 22, 21)
        op2 = bits(raw, 7, 4)
        if op2 == 0b0000:
            if op & 1:
                return self.msr(imm=False)
            if bit(raw, 22):
                raise Unsupported('mrs spsr')
            return ['c.r[%d] = Cpsr(c);' % bits(raw, 15, 12)]
        if op2 == 0b0001 and op == 0b01:  # BX
            return ['BranchX(c, c.r[%d]);' % bits(raw, 3, 0), self.flow.exit()]
        if op2 == 0b0011 and op == 0b01:  # BLX register
            return ['{ const u32 t = c.r[%d]; c.r[14] = %s; BranchX(c, t); }' % (bits(raw, 3, 0), hexu(pc + 4)),
                    self.flow.exit(call=True)]
        if op2 == 0b0001 and op == 0b11:  # CLZ
            return ['c.r[%d] = Clz(c.r[%d]);' % (bits(raw, 15, 12), bits(raw, 3, 0))]
        if op2 == 0b0101:  # QADD, QSUB, QDADD, QDSUB
            rd, rn, rm = bits(raw, 15, 12), bits(raw, 19, 16), bits(raw, 3, 0)
            n = '(s64)(s32)c.r[%d]' % rn
            if op & 2:
                n = '(s64)(s32)SignedSat(c, 2 * (s64)(s32)c.r[%d], 32)' % rn
            sign = '-' if op & 1 else '+'
            return ['c.r[%d] = SignedSat(c, (s64)(s32)c.r[%d] %s %s, 32);' % (rd, rm, sign, n)]
        if bit(raw, 7) and not bit(raw, 4):
            return self.halfword_multiply()
        if op2 == 0b0111:
            raise Unsupported('bkpt')
        raise Unsupported('miscellaneous')

    # -- multiplies ----------------------------------------------------------------------------
    def multiply(self):
        raw = self.raw
        s = bit(raw, 20)
        rm, rs = bits(raw, 3, 0), bits(raw, 11, 8)
        if bit(raw, 23):  # long
            signed, accumulate = bit(raw, 22), bit(raw, 21)
            hi, lo = bits(raw, 19, 16), bits(raw, 15, 12)
            if signed:
                product = '(u64)((s64)(s32)c.r[%d] * (s64)(s32)c.r[%d])' % (rm, rs)
            else:
                product = '((u64)c.r[%d] * (u64)c.r[%d])' % (rm, rs)
            lines = ['u64 p = %s;' % product]
            if accumulate:
                lines.append('p += ((u64)c.r[%d] << 32) | c.r[%d];' % (hi, lo))
            lines.append('c.r[%d] = (u32)p; c.r[%d] = (u32)(p >> 32);' % (lo, hi))
            if s:
                lines.append('c.n = (u32)(p >> 63); c.z = p == 0;')
            return ['{'] + ['    ' + l for l in lines] + ['}']
        rd, rn = bits(raw, 19, 16), bits(raw, 15, 12)
        expr = 'c.r[%d] * c.r[%d]' % (rm, rs)
        if bit(raw, 21):
            expr += ' + c.r[%d]' % rn
        if s:
            return ['c.r[%d] = NZ(c, %s);' % (rd, expr)]
        return ['c.r[%d] = %s;' % (rd, expr)]

    def umaal(self):
        raw = self.raw
        hi, lo, rs, rm = bits(raw, 19, 16), bits(raw, 15, 12), bits(raw, 11, 8), bits(raw, 3, 0)
        return ['{ const u64 p = (u64)c.r[%d] * c.r[%d] + c.r[%d] + c.r[%d]; c.r[%d] = (u32)p; c.r[%d] = (u32)(p >> 32); }'
                % (rm, rs, hi, lo, lo, hi)]

    def halfword_multiply(self):
        raw = self.raw
        op = bits(raw, 22, 21)
        rd, rn, rs, rm = bits(raw, 19, 16), bits(raw, 15, 12), bits(raw, 11, 8), bits(raw, 3, 0)
        x, y = bit(raw, 5), bit(raw, 6)

        def half(r, top):
            return '(s32)(s16)(c.r[%d] >> 16)' % r if top else '(s32)(s16)c.r[%d]' % r
        if op == 0b00:  # SMLAxy
            return ['c.r[%d] = AddQ(c, (s64)(%s * %s), (s64)(s32)c.r[%d]);' % (rd, half(rm, x), half(rs, y), rn)]
        if op == 0b01:
            product = '(u32)(((s64)(s32)c.r[%d] * %s) >> 16)' % (rm, half(rs, y))
            if x:  # SMULWy
                return ['c.r[%d] = %s;' % (rd, product)]
            return ['c.r[%d] = AddQ(c, (s64)(s32)%s, (s64)(s32)c.r[%d]);' % (rd, product, rn)]
        if op == 0b10:  # SMLALxy (rd = hi, rn = lo)
            return ['{ u64 p = ((u64)c.r[%d] << 32) | c.r[%d]; p += (u64)(s64)(%s * %s); c.r[%d] = (u32)p; c.r[%d] = (u32)(p >> 32); }'
                    % (rd, rn, half(rm, x), half(rs, y), rn, rd)]
        return ['c.r[%d] = (u32)(%s * %s);' % (rd, half(rm, x), half(rs, y))]

    # -- loads and stores ----------------------------------------------------------------------
    def address(self, base, offset_expr, p, u, w):
        """(lines, address variable) for pre/post indexing with writeback."""
        rn = base
        sign = '+' if u else '-'
        lines = ['const u32 base = %s;' % reg(rn, self.pc)]
        lines.append('const u32 off = %s;' % offset_expr)
        if p:
            lines.append('const u32 addr = base %s off;' % sign)
            writeback = w
        else:
            lines.append('const u32 addr = base;')
            writeback = True
        if writeback and rn == 15:
            raise Unsupported('writeback to pc')
        wb = 'c.r[%d] = base %s off;' % (rn, sign) if writeback else None
        return lines, wb

    def single_transfer(self):
        raw, pc = self.raw, self.pc
        i, p, u, b, w, l = bit(raw, 25), bit(raw, 24), bit(raw, 23), bit(raw, 22), bit(raw, 21), bit(raw, 20)
        rn, rt = bits(raw, 19, 16), bits(raw, 15, 12)
        if i:
            rm = bits(raw, 3, 0)
            if rm == 15:
                raise Unsupported('pc offset register')
            value, _ = shift_imm('c.r[%d]' % rm, bits(raw, 6, 5), bits(raw, 11, 7))
            offset = value
        else:
            offset = '0x%Xu' % bits(raw, 11, 0)
        lines, wb = self.address(rn, offset, p, u, w)
        if l:
            lines.append('const u32 v = %s(c, addr);' % ('R8' if b else 'R32'))
            if wb:
                lines.append(wb)
            if rt == 15:
                if b:
                    raise Unsupported('ldrb pc')
                lines.append('BranchX(c, v);')
                lines.append(self.flow.exit())
            else:
                lines.append('c.r[%d] = v;' % rt)
        else:
            value = hexu(pc + 8) if rt == 15 else 'c.r[%d]' % rt
            lines.append('const u32 v = %s;' % value)
            lines.append('%s(c, addr, v);' % ('W8' if b else 'W32'))
            if wb:
                lines.append(wb)
        return ['{'] + ['    ' + x for x in lines] + ['}']

    def extra_transfer(self):
        raw, pc = self.raw, self.pc
        p, u, imm, w, l = bit(raw, 24), bit(raw, 23), bit(raw, 22), bit(raw, 21), bit(raw, 20)
        rn, rt = bits(raw, 19, 16), bits(raw, 15, 12)
        sh = bits(raw, 6, 5)
        if imm:
            offset = '0x%Xu' % ((bits(raw, 11, 8) << 4) | bits(raw, 3, 0))
        else:
            if bits(raw, 3, 0) == 15:
                raise Unsupported('pc offset register')
            offset = 'c.r[%d]' % bits(raw, 3, 0)
        if rt == 15:
            raise Unsupported('extra transfer with pc')
        lines, wb = self.address(rn, offset, p, u, w)
        if sh == 0b01:  # LDRH / STRH
            if l:
                lines.append('const u32 v = R16(c, addr);')
                lines += [wb] if wb else []
                lines.append('c.r[%d] = v;' % rt)
            else:
                lines.append('W16(c, addr, c.r[%d]);' % rt)
                lines += [wb] if wb else []
        elif l:  # LDRSB / LDRSH
            fn = '(u32)(s32)(s8)R8(c, addr)' if sh == 0b10 else '(u32)(s32)(s16)R16(c, addr)'
            lines.append('const u32 v = %s;' % fn)
            lines += [wb] if wb else []
            lines.append('c.r[%d] = v;' % rt)
        else:  # LDRD (10) / STRD (11)
            if rt & 1 or rt == 14:
                raise Unsupported('odd ldrd register')
            if sh == 0b10:
                lines.append('const u32 v0 = R32(c, addr), v1 = R32(c, addr + 4);')
                lines += [wb] if wb else []
                lines.append('c.r[%d] = v0; c.r[%d] = v1;' % (rt, rt + 1))
            else:
                lines.append('W32(c, addr, c.r[%d]); W32(c, addr + 4, c.r[%d]);' % (rt, rt + 1))
                lines += [wb] if wb else []
        return ['{'] + ['    ' + x for x in lines] + ['}']

    def exclusive(self):
        raw = self.raw
        size = bits(raw, 22, 21)
        if size == 0b01:  # LDREXD / STREXD: an even register pair
            rn = bits(raw, 19, 16)
            if bit(raw, 20):
                rt = bits(raw, 15, 12)
                if rt & 1 or rt == 14:
                    raise Unsupported('ldrexd pair')
                return ['{ const u32 a = c.r[%d]; const u32 lo = R32(c, a), hi = R32(c, a + 4); '
                        'LoadExclusive64(c, a, lo, hi); c.r[%d] = lo; c.r[%d] = hi; }' % (rn, rt, rt + 1)]
            rd, rt = bits(raw, 15, 12), bits(raw, 3, 0)
            if rt & 1 or rt == 14:
                raise Unsupported('strexd pair')
            return ['c.r[%d] = StoreExclusive64(c, c.r[%d], c.r[%d], c.r[%d]);' % (rd, rn, rt, rt + 1)]
        load = {0: 'R32', 2: 'R8', 3: 'R16'}[size]
        nbytes = {0: 4, 2: 1, 3: 2}[size]
        rn = bits(raw, 19, 16)
        if bit(raw, 20):
            rt = bits(raw, 15, 12)
            return ['{ const u32 a = c.r[%d]; c.r[%d] = LoadExclusive(c, a, %s(c, a)); }' % (rn, rt, load)]
        rd, rt = bits(raw, 15, 12), bits(raw, 3, 0)
        return ['c.r[%d] = StoreExclusive<%d>(c, c.r[%d], c.r[%d]);' % (rd, nbytes, rn, rt)]

    def block_transfer(self):
        raw, pc = self.raw, self.pc
        p, u, s, w, l = bit(raw, 24), bit(raw, 23), bit(raw, 22), bit(raw, 21), bit(raw, 20)
        rn = bits(raw, 19, 16)
        regs = [i for i in range(16) if bit(raw, i)]
        if s or not regs or rn == 15:
            raise Unsupported('ldm/stm form')
        n = len(regs)
        if u:
            start = '4u' if p else '0u'
            start_expr = 'c.r[%d] + %s' % (rn, start)
            final = 'c.r[%d] + %du' % (rn, 4 * n)
        else:
            start_expr = 'c.r[%d] - %du' % (rn, 4 * n - (0 if p else 4))
            final = 'c.r[%d] - %du' % (rn, 4 * n)
        lines = ['u32 a = %s;' % start_expr]
        if w:
            lines.append('const u32 wb = %s;' % final)
        if l:
            loads = []
            for r in regs:
                loads.append('const u32 v%d = R32(c, a); a += 4;' % r)
            lines += loads
            if w:
                lines.append('c.r[%d] = wb;' % rn)
            for r in regs:
                if r != 15:
                    lines.append('c.r[%d] = v%d;' % (r, r))
            if 15 in regs:
                lines.append('BranchX(c, v15);')
                lines.append(self.flow.exit())
        else:
            for r in regs:
                value = hexu(pc + 8) if r == 15 else 'c.r[%d]' % r
                lines.append('W32(c, a, %s); a += 4;' % value)
            if w:
                lines.append('c.r[%d] = wb;' % rn)
        return ['{'] + ['    ' + x for x in lines] + ['}']

    # -- media (ARMv6) -------------------------------------------------------------------------
    def media(self):
        raw = self.raw
        op1 = bits(raw, 24, 20)
        op2 = bits(raw, 7, 5)
        rd, rn, rm = bits(raw, 15, 12), bits(raw, 19, 16), bits(raw, 3, 0)
        if bits(raw, 24, 23) == 0b00:  # parallel add/subtract
            kind = bits(raw, 22, 20)
            ops = {0b000: 0, 0b001: 1, 0b010: 2, 0b011: 3, 0b100: 4, 0b111: 7}
            if kind in (0, 4) or op2 not in ops:
                raise Unsupported('parallel')
            return ['c.r[%d] = Parallel(c, %d, %d, c.r[%d], c.r[%d]);' % (rd, kind - 1, ops[op2], rn, rm)]
        if op1 == 0b01000 and not bit(raw, 5):  # PKHBT / PKHTB
            sh = bits(raw, 11, 7)
            if bit(raw, 6):
                shifted = '(u32)((s32)c.r[%d] >> %d)' % (rm, sh if sh else 31)
                return ['c.r[%d] = (c.r[%d] & 0xFFFF0000u) | (%s & 0xFFFFu);' % (rd, rn, shifted)]
            return ['c.r[%d] = (c.r[%d] & 0xFFFFu) | ((c.r[%d] << %d) & 0xFFFF0000u);' % (rd, rn, rm, sh)]
        if op1 == 0b01000 and op2 == 0b101:  # SEL
            return ['{ u32 m = 0; for (u32 i = 0; i < 4; ++i) if (c.ge & (1u << i)) m |= 0xFFu << (i * 8); '
                    'c.r[%d] = (c.r[%d] & m) | (c.r[%d] & ~m); }' % (rd, rn, rm)]
        if bits(raw, 24, 23) == 0b01 and op2 == 0b011:  # extends
            rot = bits(raw, 11, 10) * 8
            src = 'Ror(c.r[%d], %d)' % (rm, rot)
            kind = bits(raw, 22, 20)
            forms = {0b010: '(u32)(s32)(s8)%s', 0b011: '(u32)(s32)(s16)%s', 0b110: '(u32)(u8)%s',
                     0b111: '(u32)(u16)%s'}
            if kind in forms:
                value = forms[kind] % src
                if rn != 15:
                    value = 'c.r[%d] + %s' % (rn, value)
                return ['c.r[%d] = %s;' % (rd, value)]
            if kind in (0b000, 0b100):  # SXTB16 / UXTB16 (+ accumulate)
                ext = '(u32)(u16)(s16)(s8)' if kind == 0 else '(u32)(u8)'
                lines = ['const u32 s = %s;' % src,
                         'u32 lo = %s(s & 0xFF), hi = %s((s >> 16) & 0xFF);' % (ext, ext)]
                if rn != 15:
                    lines.append('lo += c.r[%d]; hi += c.r[%d] >> 16;' % (rn, rn))
                lines.append('c.r[%d] = (lo & 0xFFFFu) | (hi << 16);' % rd)
                return ['{'] + ['    ' + x for x in lines] + ['}']
            raise Unsupported('extend')
        if op1 in (0b01010, 0b01011) and not bit(raw, 5) or op1 in (0b01110, 0b01111) and not bit(raw, 5):
            # SSAT / USAT
            sat = bits(raw, 20, 16)
            sh = bits(raw, 11, 7)
            if bit(raw, 6):
                value = '(s64)((s32)c.r[%d] >> %d)' % (rm, sh if sh else 31)
            else:
                value = '(s64)(s32)(c.r[%d] << %d)' % (rm, sh)
            if bit(raw, 22):
                return ['c.r[%d] = UnsignedSat(c, %s, %d);' % (rd, value, sat)]
            return ['c.r[%d] = SignedSat(c, %s, %d);' % (rd, value, sat + 1)]
        if op1 == 0b01011 and op2 == 0b001:  # REV
            return ['c.r[%d] = __builtin_bswap32(c.r[%d]);' % (rd, rm)]
        if op1 == 0b01011 and op2 == 0b101:  # REV16
            return ['{ const u32 v = c.r[%d]; c.r[%d] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }' % (rm, rd)]
        if op1 == 0b01111 and op2 == 0b101:  # REVSH
            return ['{ const u32 v = c.r[%d]; c.r[%d] = (u32)(s32)(s16)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); }' % (rm, rd)]
        if bits(raw, 24, 20) == 0b10000 or bits(raw, 24, 20) == 0b10100 or bits(raw, 24, 20) == 0b10101:
            return self.dual_multiply()
        if bits(raw, 24, 20) == 0b11000 and op2 == 0b000:  # USAD8 / USADA8
            rd2, ra, rs = bits(raw, 19, 16), bits(raw, 15, 12), bits(raw, 11, 8)
            acc = '' if ra == 15 else ' + c.r[%d]' % ra
            return ['{ u32 t = 0; for (u32 i = 0; i < 32; i += 8) { const s32 d = (s32)((c.r[%d] >> i) & 0xFF) - (s32)((c.r[%d] >> i) & 0xFF); t += (u32)(d < 0 ? -d : d); } c.r[%d] = t%s; }'
                    % (rm, rs, rd2, acc)]
        raise Unsupported('media')

    def dual_multiply(self):
        raw = self.raw
        op1 = bits(raw, 24, 20)
        rd, ra, rs, rm = bits(raw, 19, 16), bits(raw, 15, 12), bits(raw, 11, 8), bits(raw, 3, 0)
        swap = bit(raw, 5)
        sub = bit(raw, 6)
        if op1 == 0b10101:  # SMMUL / SMMLA / SMMLS
            r = bit(raw, 5)
            prod = '(s64)(s32)c.r[%d] * (s64)(s32)c.r[%d]' % (rm, rs)
            if bits(raw, 7, 6) == 0b00:
                acc = '' if ra == 15 else '((s64)(s32)c.r[%d] << 32) + ' % ra
                expr = '(u32)((u64)(%s%s%s) >> 32)' % (acc, prod, ' + 0x80000000ll' if r else '')
            elif bits(raw, 7, 6) == 0b11:
                expr = '(u32)((u64)(((s64)(s32)c.r[%d] << 32) - %s%s) >> 32)' % (ra, prod, ' + 0x80000000ll' if r else '')
            else:
                raise Unsupported('smm')
            return ['c.r[%d] = %s;' % (rd, expr)]
        mval = 'Ror(c.r[%d], 16)' % rs if swap else 'c.r[%d]' % rs
        lines = ['const u32 m = %s, n = c.r[%d];' % (mval, rm),
                 'const s64 p1 = (s64)(s16)n * (s16)m, p2 = (s64)(s16)(n >> 16) * (s16)(m >> 16);',
                 'const s64 sum = p1 %s p2;' % ('-' if sub else '+')]
        if op1 == 0b10000:  # SMLAD / SMLSD / SMUAD / SMUSD
            if ra == 15:
                lines.append('c.r[%d] = AddQ(c, sum, 0);' % rd)
            else:
                lines.append('c.r[%d] = AddQ(c, sum, (s64)(s32)c.r[%d]);' % (rd, ra))
        else:  # SMLALD / SMLSLD: rd = hi, ra = lo
            lines.append('u64 acc = ((u64)c.r[%d] << 32) | c.r[%d]; acc += (u64)sum;' % (rd, ra))
            lines.append('c.r[%d] = (u32)acc; c.r[%d] = (u32)(acc >> 32);' % (ra, rd))
        return ['{'] + ['    ' + x for x in lines] + ['}']

    # -- coprocessors (VFP and CP15) -----------------------------------------------------------
    def coprocessor(self):
        raw = self.raw
        cp = bits(raw, 11, 8)
        if cp == 15:
            return self.cp15()
        if cp not in (10, 11):
            raise Unsupported('coprocessor')
        if bit(raw, 4):
            return self.vfp_register_transfer()
        return self.vfp_data()

    def cp15(self):
        raw = self.raw
        if not bit(raw, 4):
            raise Unsupported('cdp p15')
        opc1, crn, rt, opc2, crm = bits(raw, 23, 21), bits(raw, 19, 16), bits(raw, 15, 12), bits(raw, 7, 5), bits(raw, 3, 0)
        load = bit(raw, 20)
        if opc1 == 0 and crn == 13 and crm == 0 and opc2 in (2, 3):
            field = 'tls_ro' if opc2 == 3 else 'tls_rw'
            if load:
                if rt == 15:
                    raise Unsupported('mrc to pc')
                return ['c.r[%d] = c.%s;' % (rt, field)]
            if opc2 == 2:
                return ['c.tls_rw = c.r[%d];' % rt]
        if not load and opc1 == 0 and crn == 7:  # barriers and cache maintenance
            return []
        raise Unsupported('cp15')

    def vfp_register_transfer(self):
        raw = self.raw
        cp = bits(raw, 11, 8)
        opc1 = bits(raw, 23, 21)
        load = bit(raw, 20)
        rt = bits(raw, 15, 12)
        fn = bits(raw, 19, 16)
        if opc1 == 0b111:  # FMRX / FMXR
            if cp != 10:
                raise Unsupported('vmrs cp11')
            if fn == 0b0001:
                if load:
                    if rt == 15:  # FMSTAT
                        return ['SetCpsrFlags(c, (c.fpscr & 0xF0000000u) | (Cpsr(c) & 0x0FFFFFFFu), false);']
                    return ['c.r[%d] = c.fpscr;' % rt]
                return ['SetFpscr(c, c.r[%d]);' % rt]
            if fn == 0b1000:  # FPEXC
                return ['c.r[%d] = 0x40000000u;' % rt] if load else []
            if fn == 0b0000 and load:  # FPSID: ARM11 VFP11
                return ['c.r[%d] = 0x410120B4u;' % rt]
            raise Unsupported('vfp system register')
        if rt == 15:
            raise Unsupported('vmov pc')
        if cp == 10 and opc1 == 0:
            sn = (fn << 1) | bit(raw, 7)
            return ['c.r[%d] = c.s[%d];' % (rt, sn)] if load else ['c.s[%d] = c.r[%d];' % (sn, rt)]
        if cp == 11 and opc1 in (0, 1):
            if bit(raw, 7):
                raise Unsupported('d16+')
            index = fn * 2 + opc1
            return ['c.r[%d] = c.s[%d];' % (rt, index)] if load else ['c.s[%d] = c.r[%d];' % (index, rt)]
        raise Unsupported('vfp transfer')

    def vfp_data(self):
        raw = self.raw
        double = bits(raw, 11, 8) == 11
        p, q, r, s = bit(raw, 23), bit(raw, 21), bit(raw, 20), bit(raw, 6)
        D, N, M = bit(raw, 22), bit(raw, 7), bit(raw, 5)
        fd, fn, fm = bits(raw, 15, 12), bits(raw, 19, 16), bits(raw, 3, 0)
        single_d, single_n, single_m = (fd << 1) | D, (fn << 1) | N, (fm << 1) | M
        if double:
            get, put, nan, T = 'D', 'SetD', 'NanD', 'double'
        else:
            get, put, nan, T = 'F', 'SetF', 'NanF', 'float'

        def g(i):
            return '%s(c, %d)' % (get, i)

        def regs(dn=True):
            """Destination and operands in this precision (VFPv2: D0-D15 only)."""
            if double:
                if D or M or (dn and N):
                    raise Unsupported('d16+')
                return fd, fn, fm
            return single_d, single_n, single_m
        opc = (p << 3) | (q << 2) | (r << 1) | s
        if opc != 0b1111:
            d, n, m = regs()
            prod = '%s(c, %s * %s)' % (nan, g(n), g(m))
            exprs = {
                0b0000: '%s + %s' % (g(d), prod),          # FMAC (vmla)
                0b0001: '%s + (-%s)' % (g(d), prod),       # FNMAC (vmls)
                0b0010: '(-%s) + %s' % (g(d), prod),       # FMSC (vnmls)
                0b0011: '(-%s) + (-%s)' % (g(d), prod),    # FNMSC (vnmla)
                0b0100: '%s * %s' % (g(n), g(m)),
                0b0110: '%s + %s' % (g(n), g(m)),
                0b0111: '%s - %s' % (g(n), g(m)),
                0b1000: '%s / %s' % (g(n), g(m)),
            }
            if opc == 0b0101:  # FNMUL: negate the rounded product
                return ['%s(c, %d, -%s);' % (put, d, prod)]
            if opc not in exprs:
                raise Unsupported('vfp op')
            return ['%s(c, %d, %s(c, %s));' % (put, d, nan, exprs[opc])]
        ext = (fn << 1) | N
        if ext <= 0b00011 or 0b01000 <= ext <= 0b01011:
            d, _, m = regs(dn=False)
            if ext == 0b00000:  # FCPY
                return ['SetD(c, %d, D(c, %d));' % (d, m)] if double else ['c.s[%d] = c.s[%d];' % (d, m)]
            if ext in (0b00001, 0b00010):  # FABS / FNEG: the sign bit only
                fn_bits = 'AbsBits' if ext == 0b00001 else 'NegBits'
                if double:
                    return ['c.s[%d] = c.s[%d]; c.s[%d] = %s(c.s[%d]);' % (2 * d, 2 * m, 2 * d + 1, fn_bits, 2 * m + 1)]
                return ['c.s[%d] = %s(c.s[%d]);' % (d, fn_bits, m)]
            if ext == 0b00011:  # FSQRT
                return ['%s(c, %d, %s(c, Sqrt%s(%s)));' % (put, d, nan, 'D' if double else 'F', g(m))]
            if ext in (0b01000, 0b01001):  # FCMP / FCMPE
                return ['Compare<%s>(c, %s, %s);' % (T, g(d), g(m))]
            return ['Compare<%s>(c, %s, (%s)0);' % (T, g(d), T)]  # FCMPZ / FCMPEZ
        if ext == 0b01111:  # FCVTDS (cp10: double from single) / FCVTSD (cp11: single from double)
            if double:
                if M:
                    raise Unsupported('d16+')
                return ['SetF(c, %d, NanF(c, (float)D(c, %d)));' % (single_d, fm)]
            if D:
                raise Unsupported('d16+')
            return ['SetD(c, %d, NanD(c, (double)F(c, %d)));' % (fd, single_m)]
        if ext in (0b10000, 0b10001):  # FUITO / FSITO: the integer is in a single register
            if double and D:
                raise Unsupported('d16+')
            dest = fd if double else single_d
            conv = '(%s)(s32)c.s[%d]' if ext == 0b10001 else '(%s)(s64)(u64)c.s[%d]'
            return ['%s(c, %d, %s);' % (put, dest, conv % (T, single_m))]
        if ext in (0b11000, 0b11001, 0b11010, 0b11011):  # FTOUI(Z) / FTOSI(Z): into a single register
            if double and M:
                raise Unsupported('d16+')
            src = fm if double else single_m
            fn_name = 'ToU32' if ext in (0b11000, 0b11001) else 'ToS32'
            toward_zero = 'true' if ext & 1 else 'false'
            return ['c.s[%d] = %s((double)%s, %s);' % (single_d, fn_name, g(src), toward_zero)]
        raise Unsupported('vfp extension')

    def coprocessor_transfer(self):
        raw = self.raw
        cp = bits(raw, 11, 8)
        if cp not in (10, 11):
            raise Unsupported('ldc/stc')
        double = cp == 11
        if bits(raw, 24, 21) == 0b0010:  # MCRR / MRRC
            load = bit(raw, 20)
            rt, rt2, fm, M = bits(raw, 15, 12), bits(raw, 19, 16), bits(raw, 3, 0), bit(raw, 5)
            if rt == 15 or rt2 == 15:
                raise Unsupported('mrrc pc')
            first = fm * 2 if double else (fm << 1) | M
            if double and M:
                raise Unsupported('d16+')
            if load:
                return ['c.r[%d] = c.s[%d]; c.r[%d] = c.s[%d];' % (rt, first, rt2, first + 1)]
            return ['c.s[%d] = c.r[%d]; c.s[%d] = c.r[%d];' % (first, rt, first + 1, rt2)]
        p, u, D, w, load = bit(raw, 24), bit(raw, 23), bit(raw, 22), bit(raw, 21), bit(raw, 20)
        rn, fd, imm8 = bits(raw, 19, 16), bits(raw, 15, 12), bits(raw, 7, 0)
        if double and D:
            raise Unsupported('d16+')
        first = fd * 2 if double else (fd << 1) | D
        base = hexu((self.pc + 8) & ~3) if rn == 15 else 'c.r[%d]' % rn
        if p and not w:  # FLDS/FSTS/FLDD/FSTD
            addr = '%s %s 0x%Xu' % (base, '+' if u else '-', imm8 * 4)
            words = 2 if double else 1
            lines = ['const u32 a = %s;' % addr]
            for k in range(words):
                if load:
                    lines.append('c.s[%d] = R32(c, a + %d);' % (first + k, 4 * k))
                else:
                    lines.append('W32(c, a + %d, c.s[%d]);' % (4 * k, first + k))
            return ['{'] + ['    ' + x for x in lines] + ['}']
        # Multiple: IA (P=0, U=1) or DB with writeback (P=1, U=0, W=1).
        if p == u or rn == 15:
            raise Unsupported('vldm form')
        count = imm8 if not double else imm8 // 2 * 2  # words; FLDMX's odd word is skipped
        if count == 0 or first + count > 32:
            raise Unsupported('vldm count')
        lines = ['u32 a = c.r[%d]%s;' % (rn, ' - 0x%Xu' % (imm8 * 4) if not u else '')]
        for k in range(count):
            if load:
                lines.append('c.s[%d] = R32(c, a + %d);' % (first + k, 4 * k))
            else:
                lines.append('W32(c, a + %d, c.s[%d]);' % (4 * k, first + k))
        if w:
            lines.append('c.r[%d] %s= 0x%Xu;' % (rn, '+' if u else '-', imm8 * 4))
        return ['{'] + ['    ' + x for x in lines] + ['}']


def emit(pc, raw, flow):
    return Arm(pc, raw, flow).emit()

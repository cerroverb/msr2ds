"""Thumb (ARMv6, 16-bit plus the BL/BLX pair) instruction translation to C++ for the AOT
runtime. Same contract as emit_arm.emit; `raw2` is the following halfword (for BL/BLX)."""

from emit_arm import COND, Unsupported, bit, bits, hexu


def block(lines):
    return ['{'] + ['    ' + x for x in lines] + ['}']


def sext(value, width):
    return value - (1 << width) if value & (1 << (width - 1)) else value


class Thumb:
    def __init__(self, pc, raw, raw2, flow):
        self.pc, self.raw, self.raw2, self.flow = pc, raw, raw2, flow

    def read(self, r):
        return hexu(self.pc + 4) if r == 15 else 'c.r[%d]' % r

    def emit(self):
        raw = self.raw
        top5 = raw >> 11
        if top5 <= 0b00010:
            return self.shift_immediate()
        if top5 == 0b00011:
            return self.add_subtract()
        if raw >> 13 == 0b001:
            return self.immediate()
        if raw >> 10 == 0b010000:
            return self.alu()
        if raw >> 10 == 0b010001:
            return self.hi_register()
        if top5 == 0b01001:
            rd = bits(raw, 10, 8)
            address = ((self.pc + 4) & ~3) + bits(raw, 7, 0) * 4
            return ['c.r[%d] = R32(c, %s);' % (rd, hexu(address))]
        if raw >> 12 == 0b0101:
            return self.register_offset()
        if raw >> 13 == 0b011 or raw >> 12 == 0b1000:
            return self.immediate_offset()
        if raw >> 12 == 0b1001:
            rd = bits(raw, 10, 8)
            addr = 'c.r[13] + 0x%Xu' % (bits(raw, 7, 0) * 4)
            if bit(raw, 11):
                return ['c.r[%d] = R32(c, %s);' % (rd, addr)]
            return ['W32(c, %s, c.r[%d]);' % (addr, rd)]
        if raw >> 12 == 0b1010:
            rd = bits(raw, 10, 8)
            imm = bits(raw, 7, 0) * 4
            if bit(raw, 11):
                return ['c.r[%d] = c.r[13] + 0x%Xu;' % (rd, imm)]
            return ['c.r[%d] = %s;' % (rd, hexu(((self.pc + 4) & ~3) + imm))]
        if raw >> 12 == 0b1011:
            return self.miscellaneous()
        if raw >> 12 == 0b1100:
            return self.multiple()
        if raw >> 12 == 0b1101:
            cond = bits(raw, 11, 8)
            if cond == 0b1111:
                return ['c.r[15] = %s; c.thumb = 1; c.exit = Exit::Svc; c.svc = 0x%Xu;' % (hexu(self.pc + 2), bits(raw, 7, 0)),
                        self.flow.exit()]
            if cond == 0b1110:
                raise Unsupported('undefined')
            target = (self.pc + 4 + sext(bits(raw, 7, 0), 8) * 2) & 0xFFFFFFFF
            return ['if (%s) {' % COND[cond], '    ' + self.flow.jump(target, True), '}']
        if top5 == 0b11100:
            target = (self.pc + 4 + sext(bits(raw, 10, 0), 11) * 2) & 0xFFFFFFFF
            return [self.flow.jump(target, True)]
        if top5 == 0b11110:
            raw2 = self.raw2
            if raw2 is None or raw2 >> 11 not in (0b11111, 0b11101):
                raise Unsupported('bl prefix alone')
            offset = (sext(bits(raw, 10, 0), 11) << 12) + (bits(raw2, 10, 0) << 1)
            target = (self.pc + 4 + offset) & 0xFFFFFFFF
            lines = ['c.r[14] = %s;' % hexu(self.pc + 4 + 1)]
            if raw2 >> 11 == 0b11101:  # BLX: to ARM
                return lines + [self.flow.jump(target & ~3, False, call=True)]
            return lines + [self.flow.jump(target, True, call=True)]
        raise Unsupported('thumb')

    def shift_immediate(self):
        raw = self.raw
        op, imm, rm, rd = bits(raw, 12, 11), bits(raw, 10, 6), bits(raw, 5, 3), bits(raw, 2, 0)
        if op == 0 and imm == 0:  # MOVS
            return ['c.r[%d] = NZ(c, c.r[%d]);' % (rd, rm)]
        from emit_arm import shift_imm
        value, carry = shift_imm('m', op, imm)
        return block(['const u32 m = c.r[%d];' % rm, 'c.r[%d] = Logical(c, %s, %s);' % (rd, value, carry)])

    def add_subtract(self):
        raw = self.raw
        immediate, sub = bit(raw, 10), bit(raw, 9)
        field, rn, rd = bits(raw, 8, 6), bits(raw, 5, 3), bits(raw, 2, 0)
        b = '%du' % field if immediate else 'c.r[%d]' % field
        if sub:
            return ['c.r[%d] = AddFlags(c, c.r[%d], ~%s, 1);' % (rd, rn, b)]
        return ['c.r[%d] = AddFlags(c, c.r[%d], %s, 0);' % (rd, rn, b)]

    def immediate(self):
        raw = self.raw
        op, rd, imm = bits(raw, 12, 11), bits(raw, 10, 8), bits(raw, 7, 0)
        if op == 0:
            return ['c.r[%d] = NZ(c, %du);' % (rd, imm)]
        if op == 1:
            return ['(void)AddFlags(c, c.r[%d], ~%du, 1);' % (rd, imm)]
        if op == 2:
            return ['c.r[%d] = AddFlags(c, c.r[%d], %du, 0);' % (rd, rd, imm)]
        return ['c.r[%d] = AddFlags(c, c.r[%d], ~%du, 1);' % (rd, rd, imm)]

    def alu(self):
        raw = self.raw
        op, rm, rd = bits(raw, 9, 6), bits(raw, 5, 3), bits(raw, 2, 0)
        a, b = 'c.r[%d]' % rd, 'c.r[%d]' % rm
        if op in (2, 3, 4, 7):  # shifts by register
            typ = {2: 0, 3: 1, 4: 2, 7: 3}[op]
            return block(['u32 sc; const u32 sv = ShiftReg(%d, %s, %s, c.c, &sc);' % (typ, a, b),
                          'c.r[%d] = Logical(c, sv, sc);' % rd])
        forms = {
            0: 'c.r[%d] = NZ(c, %s & %s);', 1: 'c.r[%d] = NZ(c, %s ^ %s);',
            5: 'c.r[%d] = AddFlags(c, %s, %s, c.c);', 6: 'c.r[%d] = AddFlags(c, %s, ~%s, c.c);',
            12: 'c.r[%d] = NZ(c, %s | %s);', 13: 'c.r[%d] = NZ(c, %s * %s);',
            14: 'c.r[%d] = NZ(c, %s & ~%s);',
        }
        if op in forms:
            return [forms[op] % (rd, a, b)]
        if op == 8:
            return ['(void)NZ(c, %s & %s);' % (a, b)]
        if op == 9:  # NEG (RSBS #0)
            return ['c.r[%d] = AddFlags(c, 0u, ~%s, 1);' % (rd, b)]
        if op == 10:
            return ['(void)AddFlags(c, %s, ~%s, 1);' % (a, b)]
        if op == 11:
            return ['(void)AddFlags(c, %s, %s, 0);' % (a, b)]
        return ['c.r[%d] = NZ(c, ~%s);' % (rd, b)]  # MVN

    def hi_register(self):
        raw = self.raw
        op = bits(raw, 9, 8)
        rm = bits(raw, 6, 3)
        rd = (bit(raw, 7) << 3) | bits(raw, 2, 0)
        if op == 0b11:  # BX / BLX
            if bit(raw, 7):
                return ['{ const u32 t = %s; c.r[14] = %s; BranchX(c, t); }' % (self.read(rm), hexu(self.pc + 2 + 1)),
                        self.flow.exit(call=True)]
            if rm == 15:
                return [self.flow.jump((self.pc + 4) & ~3, False)]
            return ['BranchX(c, c.r[%d]);' % rm, self.flow.exit()]
        if op == 0b01:
            return ['(void)AddFlags(c, %s, ~%s, 1);' % (self.read(rd), self.read(rm))]
        value = '%s + %s' % (self.read(rd), self.read(rm)) if op == 0 else self.read(rm)
        if rd == 15:
            return ['c.r[15] = (%s) & ~1u; c.thumb = 1;' % value, self.flow.exit()]
        return ['c.r[%d] = %s;' % (rd, value)]

    def register_offset(self):
        raw = self.raw
        op, rm, rn, rd = bits(raw, 11, 9), bits(raw, 8, 6), bits(raw, 5, 3), bits(raw, 2, 0)
        addr = 'c.r[%d] + c.r[%d]' % (rn, rm)
        forms = {
            0: 'W32(c, a, c.r[%d]);', 1: 'W16(c, a, c.r[%d]);', 2: 'W8(c, a, c.r[%d]);',
            3: 'c.r[%d] = (u32)(s32)(s8)R8(c, a);', 4: 'c.r[%d] = R32(c, a);',
            5: 'c.r[%d] = R16(c, a);', 6: 'c.r[%d] = R8(c, a);', 7: 'c.r[%d] = (u32)(s32)(s16)R16(c, a);',
        }
        return block(['const u32 a = %s;' % addr, forms[op] % rd])

    def immediate_offset(self):
        raw = self.raw
        imm, rn, rd = bits(raw, 10, 6), bits(raw, 5, 3), bits(raw, 2, 0)
        load = bit(raw, 11)
        if raw >> 12 == 0b1000:  # halfword
            addr = 'c.r[%d] + %du' % (rn, imm * 2)
            return ['c.r[%d] = R16(c, %s);' % (rd, addr)] if load else ['W16(c, %s, c.r[%d]);' % (addr, rd)]
        byte = bit(raw, 12)
        addr = 'c.r[%d] + %du' % (rn, imm if byte else imm * 4)
        if load:
            return ['c.r[%d] = %s(c, %s);' % (rd, 'R8' if byte else 'R32', addr)]
        return ['%s(c, %s, c.r[%d]);' % ('W8' if byte else 'W32', addr, rd)]

    def miscellaneous(self):
        raw = self.raw
        if raw >> 8 == 0b10110000:  # ADD/SUB SP
            imm = bits(raw, 6, 0) * 4
            return ['c.r[13] %s= 0x%Xu;' % ('-' if bit(raw, 7) else '+', imm)]
        if raw >> 8 == 0b10110010:
            op, rm, rd = bits(raw, 7, 6), bits(raw, 5, 3), bits(raw, 2, 0)
            form = {0: '(u32)(s32)(s16)', 1: '(u32)(s32)(s8)', 2: '(u32)(u16)', 3: '(u32)(u8)'}[op]
            return ['c.r[%d] = %sc.r[%d];' % (rd, form, rm)]
        if raw >> 8 == 0b10111010:
            op, rm, rd = bits(raw, 7, 6), bits(raw, 5, 3), bits(raw, 2, 0)
            if op == 0:
                return ['c.r[%d] = __builtin_bswap32(c.r[%d]);' % (rd, rm)]
            if op == 1:
                return ['{ const u32 v = c.r[%d]; c.r[%d] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }' % (rm, rd)]
            if op == 3:
                return ['{ const u32 v = c.r[%d]; c.r[%d] = (u32)(s32)(s16)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); }' % (rm, rd)]
            raise Unsupported('rev')
        if (raw >> 9) == 0b1011010:  # PUSH
            regs = [i for i in range(8) if bit(raw, i)] + ([14] if bit(raw, 8) else [])
            lines = ['u32 a = c.r[13] - %du;' % (4 * len(regs))]
            for r in regs:
                lines.append('W32(c, a, c.r[%d]); a += 4;' % r)
            lines.append('c.r[13] -= %du;' % (4 * len(regs)))
            return block(lines)
        if (raw >> 9) == 0b1011110:  # POP
            regs = [i for i in range(8) if bit(raw, i)] + ([15] if bit(raw, 8) else [])
            lines = ['u32 a = c.r[13];']
            for r in regs:
                lines.append('const u32 v%d = R32(c, a); a += 4;' % r)
            lines.append('c.r[13] += %du;' % (4 * len(regs)))
            for r in regs:
                if r != 15:
                    lines.append('c.r[%d] = v%d;' % (r, r))
            if 15 in regs:
                lines += ['BranchX(c, v15);', self.flow.exit()]
            return block(lines)
        raise Unsupported('thumb misc')

    def multiple(self):
        raw = self.raw
        rn = bits(raw, 10, 8)
        regs = [i for i in range(8) if bit(raw, i)]
        if not regs:
            raise Unsupported('empty list')
        lines = ['u32 a = c.r[%d];' % rn]
        if bit(raw, 11):  # LDMIA: writeback unless the base is loaded
            for r in regs:
                lines.append('const u32 v%d = R32(c, a); a += 4;' % r)
            if rn not in regs:
                lines.append('c.r[%d] = a;' % rn)
            for r in regs:
                lines.append('c.r[%d] = v%d;' % (r, r))
        else:
            for r in regs:
                lines.append('W32(c, a, c.r[%d]); a += 4;' % r)
            lines.append('c.r[%d] = a;' % rn)
        return block(lines)


def emit(pc, raw, raw2, flow):
    return Thumb(pc, raw, raw2, flow).emit()

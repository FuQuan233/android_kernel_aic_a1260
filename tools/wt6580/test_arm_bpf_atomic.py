#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute atomic code emitted by the real ARM32 BPF JIT under Unicorn.

Requires a host C compiler and the Python unicorn package. The harness extracts
kernel emitter functions verbatim, rather than implementing a second emitter.
It checks word/doubleword operations, register aliases, stack slots, signed
offsets, overflow, cmpxchg success/failure and a simulated exclusive-store retry.
"""
import ctypes
from pathlib import Path
import random
import struct
import subprocess
import tempfile

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP, UC_ARM_REG_R11, UC_ARM_REG_PC, UC_ARM_REG_LR

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'arch/arm/net/bpf_jit_32.c').read_text()


def function(name):
    import re
    match = re.search(r'^static [^\n]*\b' + name + r'\(', SOURCE, re.M)
    assert match, name
    begin = SOURCE.index('{', match.start())
    depth = 1
    end = begin + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[match.start():end] + '\n'


def segment(begin, end):
    return SOURCE[SOURCE.index(begin):SOURCE.index(end)]


def harness(frame):
    prefix = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <linux/bpf.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
#define __LINUX_ARM_ARCH__ 7
#define CPU_ARCH_ARMv5TE 5
#define BPF_REG_FP BPF_REG_10
#define BPF_REG_AX MAX_BPF_REG
#define MAX_BPF_JIT_REG (MAX_BPF_REG + 1)
#define BIT(x) (1U << (x))
#define hweight16(x) __builtin_popcount(x)
#define __opcode_to_mem_arm(x) (x)
static u32 rol32(u32 x, unsigned n) { n &= 31; return (x << n) | (x >> ((-n) & 31)); }
static u32 ror32(u32 x, unsigned n) { n &= 31; return (x >> n) | (x << ((-n) & 31)); }
#include "bpf_jit_32.h"
'''
    if frame:
        prefix += '#define CONFIG_FRAME_POINTER 1\n'
    return (prefix + segment('#define CALLEE_MASK', '/*\n * JIT Context:') +
            'struct jit_ctx { unsigned idx, cpu_architecture; u32 *target; };\n' +
            function('_emit') + function('emit') +
            segment('#define imm12val', '/*\n * Initializes the JIT space') +
            function('emit_mov_i_no8m') + function('emit_mov_i') +
            function('is_stacked') + function('arm_bpf_get_reg32') +
            function('arm_bpf_get_reg64') +
            function('arm_bpf_put_reg64') + function('emit_atomic') + r'''
int generate(u32 *out, int width, int op, int dst, int src, int off) {
    struct bpf_insn insn = {.code = BPF_STX | BPF_ATOMIC | (width == 8 ? BPF_DW : BPF_W),
                           .dst_reg = dst, .src_reg = src, .off = off, .imm = op};
    struct jit_ctx count = {.target = NULL};
    struct jit_ctx real = {.target = out};
    int ret = emit_atomic(&insn, &count);
    if (ret) return ret;
    ret = emit_atomic(&insn, &real);
    if (ret) return ret;
    return count.idx == real.idx ? real.idx : -999;
}
int mapping(int reg, int half) { return bpf2a32[reg][half]; }
int frame_offset(int slot) { return EBPF_SCRATCH_TO_ARM_FP(slot); }
''')


CODE, STACK, MEMORY = 0x10000, 0x20000, 0x40000
REGS = [UC_ARM_REG_R0 + n for n in range(13)]


def case(lib, width, op, dst, src, offset, old, value, match, retry):
    out = (ctypes.c_uint32 * 256)()
    size = lib.generate(out, width, op, dst, src, offset)
    assert size > 0, size
    code = bytes(out)[:size * 4]
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    for base in (CODE, STACK, MEMORY):
        uc.mem_map(base, 0x10000)
    uc.mem_write(CODE, code)
    sp, fp, address = STACK + 0x8000, STACK + 0x9000, MEMORY + 0x8000
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.reg_write(UC_ARM_REG_R11, fp)
    mask = (1 << (width * 8)) - 1
    old &= mask
    values = [0x1122334455667700 + n for n in range(11)]
    values[src] = value
    values[dst] = address - offset
    if op == 0xf1:
        # CMPXCHG uses R0 as expected; do not overwrite an aliased pointer/source.
        expected = old if match else old ^ (1 << (width * 8 - 1))
        if dst != 0 and src != 0:
            values[0] = expected
    expected = values[0] & mask
    operand = values[src] & mask
    for reg, val in enumerate(values):
        for half, word in ((1, val & 0xffffffff), (0, val >> 32)):
            armreg = lib.mapping(reg, half)
            if armreg < 0:
                uc.mem_write(fp + lib.frame_offset(armreg), struct.pack('<I', word))
            else:
                uc.reg_write(REGS[armreg], word)
    uc.mem_write(address, old.to_bytes(width, 'little'))
    skipped = []
    if retry:
        def store_failure(vm, pc, length, data):
            word = struct.unpack('<I', vm.mem_read(pc, 4))[0]
            if word & 0x0ff00ff0 in (0x01800f90, 0x01a00f90) and not skipped:
                # Skip one store-exclusive, report failure, then execute the retry.
                # Change memory as a competing CPU would, to check the fresh load.
                if op == 0xf1 and old != expected:
                    return
                skipped.append(pc)
                vm.mem_write(address, ((old + 7) & mask).to_bytes(width, 'little'))
                vm.reg_write(UC_ARM_REG_LR, 1)
                vm.reg_write(UC_ARM_REG_PC, pc + 4)
        uc.hook_add(UC_HOOK_CODE, store_failure)
    uc.emu_start(CODE, CODE + len(code), count=1000)
    assert uc.reg_read(UC_ARM_REG_PC) == CODE + len(code), 'retry did not terminate'
    assert uc.reg_read(UC_ARM_REG_SP) == sp, 'stack imbalance'
    observed = int.from_bytes(uc.mem_read(address, width), 'little')
    before = ((old + 7) & mask) if skipped else old
    operation = op & ~1
    if operation == 0:
        after = (before + operand) & mask
    elif operation == 0x40:
        after = before | operand
    elif operation == 0x50:
        after = before & operand
    elif operation == 0xa0:
        after = before ^ operand
    elif op == 0xe1:
        after = operand
    else:
        after = operand if before == expected else before
    assert observed == after, (width, hex(op), dst, src, old, operand, observed, after)
    if op & 1:
        values[0 if op == 0xf1 else src] = before
    for reg, val in enumerate(values):
        parts = []
        for half in (1, 0):
            armreg = lib.mapping(reg, half)
            if armreg < 0:
                word = struct.unpack('<I', uc.mem_read(fp + lib.frame_offset(armreg), 4))[0]
            else:
                word = uc.reg_read(REGS[armreg])
            parts.append(word)
        assert parts[0] | parts[1] << 32 == val, (width, hex(op), dst, src, reg, hex(val), parts)
    return bool(skipped)


def main():
    rng = random.Random(6580)
    total = retries = 0
    with tempfile.TemporaryDirectory(prefix='arm-bpf-atomic-') as temp:
        temp = Path(temp)
        for frame in (False, True):
            # dlopen caches a loaded path, so each frame layout needs its own
            # library name to execute the newly compiled emitter.
            source = temp / f'emitter-{int(frame)}.c'
            binary = temp / f'emitter-{int(frame)}.so'
            source.write_text(harness(frame))
            subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Werror',
                            '-I' + str(ROOT / 'arch/arm/net'), str(source), '-o', str(binary)], check=True)
            lib = ctypes.CDLL(str(binary))
            for width in (4, 8):
                for op in (0, 1, 0x40, 0x41, 0x50, 0x51, 0xa0, 0xa1, 0xe1, 0xf1):
                    for dst in range(11):
                        for src in range(10):
                            for match, retry in ((True, False), (False, False), (True, True)):
                                offset = rng.choice((0, 8, -8, 4096, -4096, -32768, 32760))
                                old = rng.choice((0, 0xffffffff, 0xffffffffffffffff, rng.getrandbits(64)))
                                retries += case(lib, width, op, dst, src, offset, old,
                                                rng.getrandbits(64), match, retry)
                                total += 1
            code = (ctypes.c_uint32 * 256)()
            assert lib.generate(code, 8, 0x20, 1, 2, 0) == -95
    print(f'PASS: {total} emitted ARM atomic executions, {retries} forced store retries; both frame layouts')


if __name__ == '__main__':
    main()

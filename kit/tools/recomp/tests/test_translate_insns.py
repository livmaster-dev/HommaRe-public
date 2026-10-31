"""Instruction forms the first corpus never used, checked against Unicorn.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_translate_insns.py

Each case is a synthetic listing in the export grammar plus the machine code
it describes.  The listing goes through the translator exactly as a game
function does, the emitted C is compiled with tools/recomp/tests/harness.c
into one shared library, and the same initial registers and memory are run
through the native code and through Unicorn.  Registers, the defined
arithmetic flags and the scratch memory both sides wrote must agree.

No game is needed: the cases are their own machine code.  A translator that
cannot emit a case fails that case's test with the translator's error, and
the remaining cases still build and run.  clang must be on PATH (a skip
otherwise)."""

import ctypes as C
import os
import platform
import random
import shutil
import struct
import subprocess
import sys
import tempfile

import pytest
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_INTR, UcError
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
    UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
    UC_X86_REG_EFLAGS, UC_X86_REG_FPCW, UC_X86_REG_FPSW, UC_X86_REG_FPTAG,
    UC_X86_REG_MXCSR)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
import translate as T  # noqa: E402

# Guest layout shared by both engines.  The code lives in its own page range,
# the stack and scratch are zeroed before every run, and the cave holds the
# return address a RET lands on, which is where emulation stops.
CODE_BASE, CODE_SIZE = 0x0D010000, 0x00020000
STACK_BASE, STACK_SIZE = 0x0EF00000, 0x00100000
ESP_INIT = 0x0EFFFF00
SCRATCH, SCRATCH_SIZE = 0x0E100000, 0x00010000
CAVE, CAVE_SIZE = 0x0DEAC000, 0x1000
MAGIC_RET = CAVE

EFLAGS_COMPARED = 0x0CC5   # CF PF ZF SF DF OF
EFLAGS_MISC_INIT = 0x202
FPU_CW_INIT = 0x037F
FPU_TAG_INIT = 0xFFFF
MXCSR_INIT = 0x1F80
CS_SELECTOR = 0x1B          # the flat user-mode code selector Windows hands a process

UC_REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
           UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
REG_NAMES = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"]


class X86(C.Structure):
    _fields_ = [
        ("r", C.c_uint32 * 8), ("eip", C.c_uint32),
        ("eflags_cf", C.c_uint32), ("eflags_zf", C.c_uint32),
        ("eflags_sf", C.c_uint32), ("eflags_of", C.c_uint32),
        ("eflags_pf", C.c_uint32), ("eflags_af", C.c_uint32),
        ("eflags_df", C.c_uint32), ("eflags_misc", C.c_uint32),
        ("st", C.c_double * 8), ("st_bits", C.c_uint64 * 8),
        ("st_exact", C.c_uint8 * 8), ("fpu_top", C.c_uint32),
        ("fpu_cw", C.c_uint16), ("fpu_sw", C.c_uint16),
        ("fpu_tag", C.c_uint16),
        ("fs_base", C.c_uint32),
        ("xmm", (C.c_uint32 * 4) * 8),
        ("mm", C.c_uint64 * 8),
        ("ret_expect", C.c_uint32),
        ("mxcsr", C.c_uint32),
    ]


# ------------------------------------------------------------------ cases --

class Case(object):
    """One synthetic function.

    `lines` are (offset, listing text) pairs; `code` is the hex of the same
    bytes.  `setup(rng)` returns a dict with optional `regs` (name -> value),
    `mem` (list of (address, bytes)) and `args` (dwords pushed above the
    return address).  `ignore` lists (address, length) ranges of scratch the
    comparison skips; `check(native, emu)` adds case-specific assertions where
    exact bytes are the wrong measure (x87 results)."""

    def __init__(self, name, addr, lines, code, setup=None, ignore=(), check=None,
                 expect_intr=None, entries=()):
        self.name = name
        self.addr = addr
        self.lines = lines
        self.code = bytes.fromhex(code.replace(" ", ""))
        self.setup = setup or (lambda rng: {})
        self.ignore = ignore
        self.check = check
        self.expect_intr = expect_intr
        self.entries = entries

    def listing(self):
        return "\n".join("%08x  %s" % (self.addr + off, text) for off, text in self.lines) + "\n"


def rand_regs(rng, **fixed):
    regs = {name: rng.getrandbits(32) for name in REG_NAMES if name != "ESP"}
    regs.update(fixed)
    return regs


def cmpxchg_setup(rng):
    """Exercise both the matching and nonmatching accumulator paths."""
    mem = rng.getrandbits(32)
    eax = mem if rng.random() < 0.5 else rng.getrandbits(32)
    return {"regs": rand_regs(rng, ECX=SCRATCH + 0x40, EAX=eax),
            "mem": [(SCRATCH + 0x40, struct.pack("<I", mem))]}


def cmpxchg8b_setup(rng):
    """Compare equal qwords and mismatches confined to either dword."""
    mem = rng.getrandbits(64)
    expected = mem
    if rng.random() < 0.5:
        expected ^= 1 << rng.randrange(64)
    return {"regs": rand_regs(rng, EDI=SCRATCH + 0x40,
                              EAX=expected & 0xffffffff, EDX=expected >> 32),
            "mem": [(SCRATCH + 0x40, struct.pack("<Q", mem))]}


def loop_setup(rng):
    # ECX = 0 would spin 2^32 times; the CRT never enters a LOOP that way.
    return {"regs": rand_regs(rng, ECX=rng.randint(1, 40))}


def cmps_setup(rng):
    """Two dword arrays that agree for a random prefix, ECX covering them."""
    count = rng.randint(1, 12)
    same = rng.randint(0, count)          # == count: the arrays are equal
    left = [rng.getrandbits(32) for _ in range(count)]
    right = list(left)
    if same < count:
        right[same] ^= rng.randint(1, 0xFFFFFFFF)
    a, b = SCRATCH + 0x100, SCRATCH + 0x800
    return {"regs": rand_regs(rng, ECX=count, ESI=a, EDI=b),
            "mem": [(a, struct.pack("<%dI" % count, *left)),
                    (b, struct.pack("<%dI" % count, *right))]}


def cmpsw_setup(rng):
    count = rng.randint(1, 12)
    same = rng.randint(0, count)
    left = [rng.getrandbits(16) for _ in range(count)]
    right = list(left)
    if same < count:
        right[same] ^= rng.randint(1, 0xFFFF)
    a, b = SCRATCH + 0x100, SCRATCH + 0x800
    return {"regs": rand_regs(rng, ECX=count, ESI=a, EDI=b),
            "mem": [(a, struct.pack("<%dH" % count, *left)),
                    (b, struct.pack("<%dH" % count, *right))]}


def scas_setup(rng, width):
    count = rng.randint(1, 12)
    needle = rng.getrandbits(width * 8)
    hay = [rng.getrandbits(width * 8) for _ in range(count)]
    if rng.random() < 0.7:
        hay[rng.randrange(count)] = needle
    fmt = {2: "H", 4: "I"}[width]
    a = SCRATCH + 0x100
    return {"regs": rand_regs(rng, ECX=count, EDI=a, EAX=needle | (rng.getrandbits(32) << (width * 8))),
            "mem": [(a, struct.pack("<%d%s" % (count, fmt), *hay))]}


def seg_store_setup(rng):
    return {"regs": rand_regs(rng, ECX=SCRATCH + 0x40)}


def check_seg_store(native, emu):
    # Unicorn has no GDT, so its CS is not Windows's; the translator stores
    # the selector a 32-bit Windows process sees, which is what a CONTEXT
    # record written by the CRT's exception path expects.
    assert struct.unpack("<H", bytes(native[SCRATCH + 0x40:SCRATCH + 0x42]))[0] == CS_SELECTOR


def one_double(rng, lo, hi):
    return {"args": list(struct.unpack("<II", struct.pack("<d", rng.uniform(lo, hi))))}


def fptan_setup(rng):
    return one_double(rng, -1.5, 1.5)


def fsave_setup(rng):
    # ECX names the save area; the argument is the value on the x87 stack.
    d = one_double(rng, -1e6, 1e6)
    d["regs"] = rand_regs(rng, ECX=SCRATCH + 0x1000)
    return d


def rd_double(mem, addr):
    return struct.unpack("<d", bytes(mem[addr:addr + 8]))[0]


def check_fptan(native, emu):
    esp = ESP_INIT
    for who, mem in (("native", native), ("unicorn", emu)):
        assert rd_double(mem, esp + 0xc) == 1.0, "%s: FPTAN did not push 1.0" % who
    n, u = rd_double(native, esp + 0x14), rd_double(emu, esp + 0x14)
    assert abs(n - u) <= 1e-12 * max(1.0, abs(u)), "tan: native %r unicorn %r" % (n, u)


def check_fsave(native, emu):
    area = SCRATCH + 0x1000 + 8
    # Control word, status word and tag word, then the eight registers as
    # 80-bit values.  The exception pointers in between (FIP, FCS, FDP, FDS)
    # are compared by the generic scratch check only outside the ignored
    # range: the translator does not track them.
    for lo, hi, what in ((0, 12, "environment"), (28, 108, "registers")):
        assert bytes(native[area + lo:area + hi]) == bytes(emu[area + lo:area + hi]), (
            "FSAVE %s image differs: native %s unicorn %s"
            % (what, bytes(native[area + lo:area + hi]).hex(), bytes(emu[area + lo:area + hi]).hex()))


def check_x87_constants(native, emu):
    # FLDL2E is pushed last, so it is the first value FSTP stores.
    for mem in (native, emu):
        l2e = rd_double(mem, SCRATCH + 0x100)
        ln2 = rd_double(mem, SCRATCH + 0x108)
        assert abs(l2e - 1.4426950408889634) < 1e-15, l2e
        assert abs(ln2 - 0.6931471805599453) < 1e-15, ln2


def fbstp_setup(rng):
    value = rng.randint(-999999999, 999999999)
    return {"regs": rand_regs(rng, ECX=SCRATCH + 0x200),
            "mem": [(SCRATCH + 0x210, struct.pack("<i", value))]}


def mxcsr_rounding_setup(rng):
    """Operands whose results are inexact, so that the rounding mode decides
    the last bit, and an MXCSR with a random rounding field to run them in."""
    def single():
        return struct.unpack("<f", struct.pack("<f", rng.uniform(-1e3, 1e3)))[0]
    mxcsr = 0x1F80 | (rng.randrange(4) << 13)
    return {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100,
                         EDX=rng.randrange(0x1000001, 0x7FFFFFFF) * rng.choice([1, -1]) & 0xFFFFFFFF),
            "mem": [(SCRATCH + 0x300, struct.pack("<fff4xIIdd", single(), single(), single() / 3e5,
                                                  mxcsr, MXCSR_INIT, rng.uniform(-1e6, 1e6),
                                                  rng.choice([3.0, 7.0, -11.0, rng.uniform(1.0, 1e3)])))]}


def sse_scalar_setup(rng):
    """Two doubles worth dividing and comparing, and an integer to convert.
    The second is never zero, and both are plain values: this checks the
    translation, not what the host does with a NaN."""
    a = rng.uniform(-1e6, 1e6)
    b = rng.choice([rng.uniform(1.0, 1e3), -rng.uniform(1.0, 1e3), a])
    return {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100,
                         EDX=rng.randrange(-10000, 10000) & 0xFFFFFFFF),
            "mem": [(SCRATCH + 0x300, struct.pack("<dd", a, b)),
                    (SCRATCH + 0x100, b"\x00" * 0x100)]}


CASES = [
    # A listing that misdecodes data as code can produce the port string
    # forms. A user-mode guest never reaches one, so what matters is that the
    # build survives them and that they move the pointer and count the way
    # every other string form does: the harness's port shim reads as zero.
    # A Delphi runtime's FillChar and Move use SSE2 unconditionally: it
    # predates every CPU the compiler supports, so there is no feature test
    # to fail and no scalar path to fall back to. Only data movement is
    # modelled, so memory is where the result has to agree.
    Case("SSE2 moves a vector through memory and broadcasts a dword", 0x0D02E000,
         [(0, "MOVUPS XMM0,xmmword ptr [ESI]"),
          (3, "MOVUPS xmmword ptr [EDI],XMM0"),
          (6, "MOVD XMM1,ECX"),
          (10, "PSHUFD XMM1,XMM1,0x0"),
          (15, "MOVUPS xmmword ptr [EDI + 0x10],XMM1"),
          (19, "MOVQ XMM2,qword ptr [ESI + 0x8]"),
          (24, "MOVQ qword ptr [EDI + 0x20],XMM2"),
          (29, "RET")],
         "0f 10 06 0f 11 07 66 0f 6e c9 66 0f 70 c9 00 0f 11 4f 10 f3 0f 7e 56 08 "
         "66 0f d6 57 20 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x200,
                                   EDI=SCRATCH + 0x100, ECX=0xa5b6c7d8),
                      "mem": [(SCRATCH + 0x200, bytes(range(0x40, 0x50))),
                              (SCRATCH + 0x100, b"\x00" * 0x30)]}),
    # The RTL's Move and FillChar address their vectors through a base and an
    # index, and copy register to register between loads, so those forms carry
    # the same weight as the simple ones.
    Case("SSE2 through indexed addressing and between registers", 0x0D02F000,
         [(0, "MOVUPS XMM1,xmmword ptr [ESI]"),
          (3, "MOVUPS xmmword ptr [ECX + EDX*0x1],XMM1"),
          (7, "MOVAPS XMM0,XMM1"),
          (10, "MOVD XMM0,EDX"),
          (14, "PSHUFD XMM2,XMM0,0x55"),
          (19, "MOVAPS xmmword ptr [EDI + EDX*0x1 + 0x10],XMM2"),
          (24, "MOVQ XMM1,qword ptr [ESI + 0x8]"),
          (29, "MOVQ qword ptr [EDI + EDX*0x1 + 0x20],XMM0"),
          (35, "RET")],
         "0f 10 0e 0f 11 0c 11 0f 28 c1 66 0f 6e c2 66 0f 70 d0 55 0f 29 54 17 10 "
         "f3 0f 7e 4e 08 66 0f d6 44 17 20 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300,
                                   ECX=SCRATCH + 0x100, EDI=SCRATCH + 0x200,
                                   EDX=0x10),
                      "mem": [(SCRATCH + 0x300, bytes(range(0x10, 0x20))),
                              (SCRATCH + 0x100, b"\x00" * 0x180)]}),
    # Wine's own i386 DLLs, and every MSVC runtime since about 2005, use SSE2
    # for things that have nothing to do with floating point: memset builds
    # its fill pattern with PUNPCK and stores it with MOVUPS, and locale code
    # moves a pair of dwords through MOVD/PUNPCKLDQ. None of it asks CPUID
    # first. The lanes land in memory, which is where both engines are read.
    Case("SSE2 lane, unpack and shuffle forms", 0x0D02F800,
         [(0, "MOVUPS XMM0,xmmword ptr [ESI]"),
          (3, "MOVUPS XMM1,xmmword ptr [ESI + 0x10]"),
          (7, "MOVAPD XMM2,XMM0"),
          (11, "PUNPCKLDQ XMM2,XMM1"),
          (15, "MOVUPS xmmword ptr [EDI],XMM2"),
          (18, "MOVAPD XMM3,XMM0"),
          (22, "PUNPCKLQDQ XMM3,XMM1"),
          (26, "MOVUPS xmmword ptr [EDI + 0x10],XMM3"),
          (30, "MOVAPD XMM4,XMM0"),
          (34, "PXOR XMM4,XMM1"),
          (38, "MOVUPS xmmword ptr [EDI + 0x20],XMM4"),
          (42, "MOVAPD XMM5,XMM0"),
          (46, "PCMPEQD XMM5,XMM1"),
          (50, "MOVUPS xmmword ptr [EDI + 0x30],XMM5"),
          (54, "MOVDDUP XMM6,XMM1"),
          (58, "MOVUPS xmmword ptr [EDI + 0x40],XMM6"),
          (62, "MOVAPD XMM7,XMM0"),
          (66, "SHUFPS XMM7,XMM1,0x93"),
          (70, "MOVUPS xmmword ptr [EDI + 0x50],XMM7"),
          (74, "MOVAPD XMM2,XMM0"),
          (78, "MOVHPD XMM2,qword ptr [ESI + 0x8]"),
          (83, "MOVUPS xmmword ptr [EDI + 0x60],XMM2"),
          (87, "MOVLPD qword ptr [EDI + 0x70],XMM1"),
          (92, "RET")],
         "0f 10 06 0f 10 4e 10 66 0f 28 d0 66 0f 62 d1 0f 11 17 66 0f 28 d8 66 0f 6c d9 "
         "0f 11 5f 10 66 0f 28 e0 66 0f ef e1 0f 11 67 20 66 0f 28 e8 66 0f 76 e9 "
         "0f 11 6f 30 f2 0f 12 f1 0f 11 77 40 66 0f 28 f8 0f c6 f9 93 0f 11 7f 50 "
         "66 0f 28 d0 66 0f 16 56 08 0f 11 57 60 66 0f 13 4f 70 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
                      "mem": [(SCRATCH + 0x300, bytes(rng.randrange(256) for _ in range(0x20))),
                              (SCRATCH + 0x100, b"\x00" * 0x100)]}),
    # The scalar double forms the same runtimes use for ordinary arithmetic,
    # and the one SSE form that writes the guest's flags.
    Case("SSE2 scalar double arithmetic, conversions and COMISD", 0x0D02FC00,
         [(0, "MOVSD XMM0,qword ptr [ESI]"),
          (4, "MOVSD XMM1,qword ptr [ESI + 0x8]"),
          (9, "MOVAPD XMM2,XMM0"),
          (13, "ADDSD XMM2,XMM1"),
          (17, "MOVSD qword ptr [EDI],XMM2"),
          (21, "MOVAPD XMM2,XMM0"),
          (25, "SUBSD XMM2,XMM1"),
          (29, "MOVSD qword ptr [EDI + 0x8],XMM2"),
          (34, "MOVAPD XMM2,XMM0"),
          (38, "MULSD XMM2,XMM1"),
          (42, "MOVSD qword ptr [EDI + 0x10],XMM2"),
          (47, "MOVAPD XMM2,XMM0"),
          (51, "DIVSD XMM2,XMM1"),
          (55, "MOVSD qword ptr [EDI + 0x18],XMM2"),
          (60, "SQRTSD XMM3,XMM1"),
          (64, "MOVSD qword ptr [EDI + 0x20],XMM3"),
          (69, "CVTSI2SD XMM4,EDX"),
          (73, "MOVSD qword ptr [EDI + 0x28],XMM4"),
          (78, "CVTTSD2SI EAX,XMM0"),
          (82, "CVTSD2SS XMM5,XMM0"),
          (86, "MOVSS dword ptr [EDI + 0x30],XMM5"),
          (91, "CVTSS2SD XMM6,XMM5"),
          (95, "MOVSD qword ptr [EDI + 0x38],XMM6"),
          (100, "COMISD XMM0,XMM1"),
          (104, "RET")],
         "f2 0f 10 06 f2 0f 10 4e 08 66 0f 28 d0 f2 0f 58 d1 f2 0f 11 17 66 0f 28 d0 "
         "f2 0f 5c d1 f2 0f 11 57 08 66 0f 28 d0 f2 0f 59 d1 f2 0f 11 57 10 66 0f 28 d0 "
         "f2 0f 5e d1 f2 0f 11 57 18 f2 0f 51 d9 f2 0f 11 5f 20 f2 0f 2a e2 f2 0f 11 67 28 "
         "f2 0f 2c c0 f2 0f 5a e8 f3 0f 11 6f 30 f3 0f 5a f5 f2 0f 11 77 38 66 0f 2f c1 c3",
         sse_scalar_setup),
    # A conversion's other operand is as often memory or a general register
    # as it is an XMM one, CWDE turns up in the same runtimes, and a guest
    # that reads MXCSR before setting it gets the reset value.
    Case("SSE conversions through memory, CWDE and MXCSR", 0x0D02F000 + 0x600,
         [(0, "CVTTSD2SI EAX,qword ptr [ESI]"),
          (4, "MOV dword ptr [EDI],EAX"),
          (6, "CVTSI2SD XMM0,dword ptr [ESI + 0x10]"),
          (11, "MOVSD qword ptr [EDI + 0x8],XMM0"),
          (16, "MOVSX ECX,AX"),
          (19, "CWDE"),
          (20, "MOV dword ptr [EDI + 0x10],EAX"),
          (23, "STMXCSR dword ptr [EDI + 0x18]"),
          (27, "RET")],
         "f2 0f 2c 06 89 07 f2 0f 2a 46 10 f2 0f 11 47 08 0f bf c8 98 89 47 10 0f ae 5f 18 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
                      "mem": [(SCRATCH + 0x300,
                               struct.pack("<d", rng.uniform(-1e6, 1e6))
                               + struct.pack("<q", 0)
                               + struct.pack("<i", rng.randrange(-100000, 100000))),
                              (SCRATCH + 0x100, b"\x00" * 0x40)]}),
    # CVTTSS2SI reads a single, from memory or a register: a dword beside
    # another float read as a double is a huge number. Out of range is the
    # integer indefinite, not the host's saturation.
    Case("CVTTSS2SI reads a single and gives the integer indefinite", 0x0D02F000 + 0x700,
         [(0, "CVTTSS2SI EAX,dword ptr [ESI]"),
          (4, "MOV dword ptr [EDI],EAX"),
          (6, "MOVSS XMM0,dword ptr [ESI + 0x4]"),
          (11, "CVTTSS2SI ECX,XMM0"),
          (15, "MOV dword ptr [EDI + 0x4],ECX"),
          (18, "CVTTSS2SI EDX,dword ptr [ESI + 0x8]"),
          (23, "MOV dword ptr [EDI + 0x8],EDX"),
          (26, "RET")],
         "f3 0f 2c 06 89 07 f3 0f 10 46 04 f3 0f 2c c8 89 4f 04 f3 0f 2c 56 08 89 57 08 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
                      "mem": [(SCRATCH + 0x300,
                               struct.pack("<fff", rng.uniform(-5000.0, 5000.0),
                                           rng.uniform(-5000.0, 5000.0),
                                           rng.choice((3e9, -3e9, float("nan"))))
                               + struct.pack("<f", 960.0)),
                              (SCRATCH + 0x100, b"\x00" * 0x40)]}),
    # MIN is dst < src ? dst : src and MAX the same with >; a NaN or two
    # zeros give the second operand.
    Case("MINSS, MAXSS, MINSD and MAXSD keep the smaller and the larger", 0x0D02F000 + 0x780,
         [(0, "MOVSS XMM0,dword ptr [ESI]"),
          (4, "MOVSS XMM1,dword ptr [ESI + 0x4]"),
          (9, "MOVAPS XMM2,XMM0"),
          (12, "MINSS XMM2,XMM1"),
          (16, "MOVSS dword ptr [EDI],XMM2"),
          (20, "MOVAPS XMM3,XMM0"),
          (23, "MAXSS XMM3,XMM1"),
          (27, "MOVSS dword ptr [EDI + 0x4],XMM3"),
          (32, "MOVSD XMM4,qword ptr [ESI + 0x8]"),
          (37, "MOVSD XMM5,qword ptr [ESI + 0x10]"),
          (42, "MOVAPS XMM6,XMM4"),
          (45, "MINSD XMM6,XMM5"),
          (49, "MOVSD qword ptr [EDI + 0x8],XMM6"),
          (54, "MAXSD XMM4,XMM5"),
          (58, "MOVSD qword ptr [EDI + 0x10],XMM4"),
          (63, "RET")],
         "f3 0f 10 06 f3 0f 10 4e 04 0f 28 d0 f3 0f 5d d1 f3 0f 11 17 0f 28 d8 f3 0f 5f d9 "
         "f3 0f 11 5f 04 f2 0f 10 66 08 f2 0f 10 6e 10 0f 28 f4 f2 0f 5d f5 f2 0f 11 77 08 "
         "f2 0f 5f e5 f2 0f 11 67 10 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
                      "mem": [(SCRATCH + 0x300,
                               struct.pack("<ff", rng.uniform(-500.0, 500.0),
                                           rng.choice((rng.uniform(-500.0, 500.0), 0.0,
                                                       float("nan"))))
                               + struct.pack("<dd", rng.uniform(-1e6, 1e6),
                                             rng.uniform(-1e6, 1e6))),
                              (SCRATCH + 0x100, b"\x00" * 0x40)]}),
    Case("Port string forms store the port read and advance", 0x0D02D000,
         [(0, "INSD ES:EDI,DX"), (1, "INSD.REP ES:EDI,DX"), (3, "OUTSD ESI,DX"),
          (4, "RET")],
         "6d f3 6d 6f c3",
         lambda rng: {"regs": dict(rand_regs(rng), EDI=SCRATCH + 0x100, ESI=SCRATCH + 0x200,
                                   ECX=2, EDX=0x3f8),
                      "mem": [(SCRATCH + 0x100, b"\xaa" * 32),
                              (SCRATCH + 0x200, struct.pack("<I", 0x11223344))]}),
    Case("Vtable adapter RET enters the method with Self and the caller return", 0x0D029000,
         [(0, "ADD EAX,-0x8"), (3, "PUSH EAX"), (4, "MOV EAX,dword ptr [EAX]"),
          (6, "MOV EAX,dword ptr [EAX + 0x8]"), (9, "XCHG dword ptr [ESP],EAX"),
          (12, "RET"), (13, "MOV EAX,0x12345678"), (18, "RET")],
         "83 c0 f8 50 8b 00 8b 40 08 87 04 24 c3 b8 78 56 34 12 c3",
         lambda rng: {"regs": rand_regs(rng, EAX=SCRATCH + 0x108),
                      "mem": [(SCRATCH + 0x100, struct.pack("<I", SCRATCH + 0x200)),
                              (SCRATCH + 0x208, struct.pack("<I", 0x0D02900D))]},
         entries=(0x0D02900D,)),
    Case("Cleanup RET follows a nonadjacent pushed continuation", 0x0D01EC00,
         [(0, "PUSH EBP"), (1, "MOV EBP,ESP"), (3, "PUSH 0x0d01ec11"),
          (8, "LEA EAX,[EBP]"), (11, "MOV EDX,0x3"),
          (16, "RET"), (17, "POP EBP"), (18, "RET")],
         "55 89 e5 68 11 ec 01 0d 8d 45 00 ba 03 00 00 00 c3 5d c3",
         lambda rng: {"regs": rand_regs(rng)}),
    Case("Cleanup RET immediate adjusts ESP before the continuation", 0x0D01ED00,
         [(0, "PUSH 0x1234"), (5, "PUSH 0x0d01ed0e"), (10, "NOP"),
          (11, "RET 0x4"), (14, "INC EAX"), (15, "RET 0x8")],
         "68 34 12 00 00 68 0e ed 01 0d 90 c2 04 00 40 c2 08 00",
         lambda rng: {"regs": rand_regs(rng), "args": [11, 22]}),
    Case("Variable argument cleanup returns through a popped address", 0x0D01EA00,
         [(0, "POP EAX"), (1, "LEA ESP,[ESP + EDX*0x4]"), (4, "JMP EAX")],
         "58 8d 24 94 ff e0",
         lambda rng: {"regs": rand_regs(rng, EDX=3), "args": [11, 22, 33]}),
    Case("Popped EDX returns after removing two arguments", 0x0D01EB00,
         [(0, "POP EDX"), (1, "ADD ESP,0x8"), (4, "JMP EDX")],
         "5a 83 c4 08 ff e2",
         lambda rng: {"regs": rand_regs(rng), "args": [11, 22]}),
    Case("PUSH immediate RET reaches the epilogue before returning", 0x0D01E800,
         [(0, "PUSH EBP"), (1, "MOV EBP,ESP"),
          (3, "PUSH 0x0d01e809"), (8, "RET"), (9, "POP EBP"), (10, "RET")],
         "55 89 e5 68 09 e8 01 0d c3 5d c3"),
    Case("A branch to the shared RET returns to its own caller", 0x0D01E900,
         [(0, "JMP 0x0d01e90a"), (5, "PUSH 0x0d01e90b"),
          (10, "RET"), (11, "INC EAX"), (12, "RET")],
         "e9 05 00 00 00 68 0b e9 01 0d c3 40 c3"),
    Case("LOOP counts ECX down and branches while it is not zero", 0x0D010000,
         [(0x0, "XOR EAX,EAX"), (0x2, "INC EAX"), (0x3, "LOOP 0x0d010002"), (0x5, "RET")],
         "31 C0  40  E2 FD  C3", loop_setup),
    Case("INT3 is a breakpoint the runtime notes and steps over", 0x0D011000,
         [(0x0, "INT3"), (0x1, "MOV EAX,0x1234"), (0x6, "RET")],
         "CC  B8 34 12 00 00  C3", expect_intr=3),
    Case("STC and CLC set and clear the carry", 0x0D012000,
         [(0x0, "STC"), (0x1, "SETC AL"), (0x4, "CLC"), (0x5, "SETC BL"), (0x8, "RET")],
         "F9  0F 92 C0  F8  0F 92 C3  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("REPE CMPSD compares dwords until they differ", 0x0D013000,
         [(0x0, "CMPSD.REPE ES:EDI,ESI"), (0x2, "RET")],
         "F3 A7  C3", cmps_setup),
    Case("REPNE CMPSD compares dwords until they match", 0x0D014000,
         [(0x0, "CMPSD.REPNE ES:EDI,ESI"), (0x2, "RET")],
         "F2 A7  C3", cmps_setup),
    Case("CMPSW compares one word", 0x0D015000,
         [(0x0, "CMPSW ES:EDI,ESI"), (0x2, "RET")],
         "66 A7  C3", cmpsw_setup),
    Case("REPE CMPSW compares words until they differ", 0x0D016000,
         [(0x0, "CMPSW.REPE ES:EDI,ESI"), (0x3, "RET")],
         "F3 66 A7  C3", cmpsw_setup),
    Case("REPNE SCASD scans dwords for EAX", 0x0D017000,
         [(0x0, "SCASD.REPNE ES:EDI,EAX"), (0x2, "RET")],
         "F2 AF  C3", lambda rng: scas_setup(rng, 4)),
    Case("REPE SCASW scans words while they equal AX", 0x0D018000,
         [(0x0, "SCASW.REPE ES:EDI,AX"), (0x3, "RET")],
         "F3 66 AF  C3", lambda rng: scas_setup(rng, 2)),
    Case("PUSH and POP of 16-bit registers move ESP by two", 0x0D019000,
         [(0x0, "PUSH AX"), (0x2, "PUSH CX"), (0x4, "POP DX"), (0x6, "POP BX"), (0x8, "RET")],
         "66 50  66 51  66 5A  66 5B  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("MOV to memory from CS stores the code selector", 0x0D01A000,
         [(0x0, "MOV word ptr [ECX],CS"), (0x2, "RET")],
         "8C 09  C3", seg_store_setup, ignore=((SCRATCH + 0x40, 2),), check=check_seg_store),
    Case("FPTAN replaces ST(0) with its tangent and pushes 1.0", 0x0D01B000,
         [(0x0, "FLD double ptr [ESP + 0x4]"), (0x4, "FPTAN"),
          (0x6, "FSTP double ptr [ESP + 0xc]"), (0xa, "FSTP double ptr [ESP + 0x14]"),
          (0xe, "RET")],
         "DD 44 24 04  D9 F2  DD 5C 24 0C  DD 5C 24 14  C3", fptan_setup,
         # The tangent is the bundled libm's (third_party/musl-libm), within
         # an ulp of the x87's, not bit for bit: check_fptan compares it.
         ignore=((ESP_INIT + 0x14, 8),), check=check_fptan),
    Case("FSAVE writes the 108-byte state and reinitialises; FRSTOR brings it back", 0x0D01C000,
         [(0x0, "FLD1"), (0x2, "FLD double ptr [ESP + 0x4]"), (0x6, "FSAVE [ECX + 0x8]"),
          (0xa, "FLD1"), (0xc, "FSTP double ptr [ESP + 0xc]"),
          (0x10, "FRSTOR [ECX + 0x8]"), (0x13, "FSTP double ptr [ESP + 0x14]"),
          (0x17, "FSTP double ptr [ESP + 0x1c]"), (0x1b, "RET")],
         "D9 E8  DD 44 24 04  9B DD 71 08  D9 E8  DD 5C 24 0C  DD 61 08  DD 5C 24 14  DD 5C 24 1C  C3",
         fsave_setup, ignore=((SCRATCH + 0x1000 + 8 + 12, 16),), check=check_fsave),
    Case("PUSHF and POPF move the low 16 flag bits through the stack", 0x0D01E000,
         [(0x0, "STC"), (0x1, "PUSHF"), (0x3, "POP AX"), (0x5, "CLC"), (0x6, "PUSH AX"),
          (0x8, "POPF"), (0xa, "SETC BL"), (0xd, "RET")],
         "F9  66 9C  66 58  F8  66 50  66 9D  0F 92 C3  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("FNINIT empties the stack and resets the control word", 0x0D01D000,
         [(0x0, "FLD1"), (0x2, "FNINIT"), (0x4, "FNSTCW word ptr [ESP + 0x4]"),
          (0x8, "FLD1"), (0xa, "FSTP double ptr [ESP + 0xc]"), (0xe, "RET")],
         "D9 E8  DB E3  D9 7C 24 04  D9 E8  DD 5C 24 0C  C3",
         lambda rng: {"args": [0, 0, 0, 0, 0, 0]}),
    Case("Computed address enters an unrolled fill", 0x0D01EE00,
         [(0, "MOV CH,CL"), (2, "TEST EDX,EDX"), (4, "JLE 0xd01ee28"),
          (6, "MOV byte ptr [EDX + EAX*0x1 + -0x1],CL"), (10, "AND EDX,0xfffffffe"),
          (13, "NEG EDX"), (15, "LEA EDX,[EDX*0x2 + 0xd01ee28]"),
          (22, "JMP EDX"), (24, "MOV word ptr [EAX + 0x6],CX"),
          (28, "MOV word ptr [EAX + 0x4],CX"), (32, "MOV word ptr [EAX + 0x2],CX"),
          (36, "MOV word ptr [EAX],CX"), (39, "RET"), (40, "RET")],
         "88cd 85d2 7e22 884c02ff 83e2fe f7da 8d1455 28ee010d "
         "ffe2 66894806 66894804 66894802 668908 c3 c3",
         lambda rng: {"regs": rand_regs(rng, EAX=SCRATCH, ECX=rng.randrange(256),
                                        EDX=rng.randrange(9))}),
    Case("Scaled LEA selects each unrolled store entry", 0x0D01EF00,
         [(0, "LEA EAX,[ECX*0x4 + 0xd01ef09]"), (7, "JMP EAX"),
          (9, "MOV word ptr [EDI + 0x6],DX"), (13, "MOV word ptr [EDI + 0x4],DX"),
          (17, "MOV word ptr [EDI + 0x2],DX"), (21, "MOV word ptr [EDI],DX"), (25, "RET")],
         "8d048d 09ef010d ffe0 66895706 66895704 66895702 66895700 c3",
         lambda rng: {"regs": rand_regs(rng, EDI=SCRATCH, ECX=rng.randrange(4))}),
    Case("CMPXCHG stores when EAX equals the destination and loads EAX otherwise", 0x0D01F000,
         [(0x0, "CMPXCHG.LOCK dword ptr [ECX],EDX"), (0x4, "SETZ BL"), (0x7, "RET")],
         "F0 0F B1 11  0F 94 C3  C3", cmpxchg_setup),
    Case("XADD exchanges and adds", 0x0D020000,
         [(0x0, "XADD.LOCK dword ptr [ECX],EDX"), (0x4, "RET")],
         "F0 0F C1 11  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x40),
                      "mem": [(SCRATCH + 0x40, struct.pack("<I", rng.getrandbits(32)))]}),
    Case("PAUSE is a hint and changes nothing", 0x0D021000,
         [(0x0, "PAUSE"), (0x2, "RET")],
         "F3 90  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("CMC complements the carry", 0x0D022000,
         [(0x0, "STC"), (0x1, "CMC"), (0x2, "SETC BL"), (0x5, "CMC"),
          (0x6, "SETC CL"), (0x9, "RET")],
         "F9  F5  0F 92 C3  F5  0F 92 C1  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("STMXCSR stores the default MXCSR", 0x0D023000,
         [(0x0, "STMXCSR dword ptr [ECX]"), (0x3, "RET")],
         "0F AE 19  C3", lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x80)}),
    # MXCSR's rounding field decides the last bit of SSE arithmetic and of the
    # rounding conversions: Heroes V parses its game database in round toward
    # zero (its _control87 sets MXCSR), and checksums what it parsed. LDMXCSR
    # loads a random mode, STMXCSR reads it back, the default goes back at the
    # end (the harness runs in this process).
    Case("LDMXCSR's rounding mode rounds SSE arithmetic and conversions", 0x0D02FE00,
         [(0x00, "LDMXCSR dword ptr [ESI + 0x10]"),
          (0x04, "STMXCSR dword ptr [EDI + 0x18]"),
          (0x08, "MOVSS XMM0,dword ptr [ESI]"),
          (0x0c, "MULSS XMM0,dword ptr [ESI + 0x4]"),
          (0x11, "MOVSS dword ptr [EDI],XMM0"),
          (0x15, "MOVSS XMM1,dword ptr [ESI]"),
          (0x19, "ADDSS XMM1,dword ptr [ESI + 0x8]"),
          (0x1e, "MOVSS dword ptr [EDI + 0x4],XMM1"),
          (0x23, "MOVSD XMM2,qword ptr [ESI + 0x18]"),
          (0x28, "CVTSD2SS XMM3,XMM2"),
          (0x2c, "MOVSS dword ptr [EDI + 0x8],XMM3"),
          (0x31, "DIVSD XMM2,qword ptr [ESI + 0x20]"),
          (0x36, "MOVSD qword ptr [EDI + 0x10],XMM2"),
          (0x3b, "CVTSI2SS XMM4,EDX"),
          (0x3f, "MOVSS dword ptr [EDI + 0xc],XMM4"),
          (0x44, "LDMXCSR dword ptr [ESI + 0x14]"),
          (0x48, "RET")],
         "0f ae 56 10 0f ae 5f 18 f3 0f 10 06 f3 0f 59 46 04 f3 0f 11 07 f3 0f 10 0e "
         "f3 0f 58 4e 08 f3 0f 11 4f 04 f2 0f 10 56 18 f2 0f 5a da f3 0f 11 5f 08 "
         "f2 0f 5e 56 20 f2 0f 11 57 10 f3 0f 2a e2 f3 0f 11 67 0c 0f ae 56 14 c3",
         mxcsr_rounding_setup),
    Case("FLDLN2 and FLDL2E push the x87 constants", 0x0D024000,
         [(0x0, "FLDLN2"), (0x2, "FLDL2E"), (0x4, "FSTP double ptr [ECX]"),
          (0x6, "FSTP double ptr [ECX + 0x8]"), (0x9, "RET")],
         "D9 ED  D9 EA  DD 19  DD 59 08  C3", lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x100)},
         ignore=((SCRATCH + 0x100, 16),), check=check_x87_constants),
    Case("FCLEX clears the status word like FNCLEX", 0x0D025000,
         [(0x0, "FLDZ"), (0x2, "FLDZ"), (0x4, "FDIVP ST1,ST0"), (0x6, "FCLEX"),
          (0x9, "FNSTSW word ptr [ECX]"), (0xb, "FSTP double ptr [ECX + 0x8]"), (0xe, "RET")],
         "D9 EE  D9 EE  DE F9  9B DB E2  DD 39  DD 59 08  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x100)}, ignore=((SCRATCH + 0x108, 8),)),
    Case("FBSTP stores packed BCD and pops", 0x0D026000,
         [(0x0, "FILD dword ptr [ECX + 0x10]"), (0x3, "FBSTP tword ptr [ECX]"), (0x5, "RET")],
         "DB 41 10  DF 31  C3", fbstp_setup),
    Case("CMPXCHG8B compares EDX:EAX and stores ECX:EBX changing only ZF", 0x0D027000,
         # Materialize CMP's flags: Unicorn otherwise corrupts its lazy
         # flag path through CMPXCHG8B. PUSHFD after it also checks preserved AF.
         [(0x0, "CMP ESI,EBP"), (0x2, "PUSHFD"), (0x3, "POPFD"),
          (0x4, "CMPXCHG8B.LOCK qword ptr [EDI]"),
          (0x8, "PUSHFD"), (0x9, "POP ESI"), (0xa, "RET")],
         "39 EE  9C  9D  F0 0F C7 0F  9C  5E  C3", cmpxchg8b_setup),
    Case("EMMS changes no integer registers or flags", 0x0D028000,
         [(0x0, "CMP ESI,EBP"), (0x2, "EMMS"), (0x4, "RET")],
         "39 EE  0F 77  C3", lambda rng: {"regs": rand_regs(rng)}),
]


def integer_copy_setup(rng, value=None):
    return {"regs": rand_regs(rng, EAX=SCRATCH, EDX=SCRATCH + 64),
            "mem": [(SCRATCH, struct.pack("<Q", rng.getrandbits(64) if value is None else value))]}


# x87 has enough mantissa bits to copy any signed qword without rounding.
# Include fixed pointer/string-like patterns and a fresh random value each run.
CASES += [
    Case("FILD FISTP qword exact copy %s" % name, 0x0D02A000 + i * 0x100,
         [(0, "FILD qword ptr [EAX]"), (2, "FISTP qword ptr [EDX]"), (4, "RET")],
         "df28 df3a c3", lambda rng, value=value: integer_copy_setup(rng, value))
    for i, (name, value) in enumerate([
        ("pattern", 0x0102030405060708), ("minus one", 0xffffffffffffffff),
        ("near minimum", 0x8000000000000001), ("random", None),
        ("UTF16", int.from_bytes("Out ".encode("utf-16le"), "little"))])
]
CASES += [
    Case("FILD FISTP copies an overlapping 26-byte resource string", 0x0D02B000,
         [(0, "FILD qword ptr [EAX + 0x12]"), (3, "FILD qword ptr [EAX]"),
          (5, "FILD qword ptr [EAX + 0x8]"), (8, "FILD qword ptr [EAX + 0x10]"),
          (11, "FISTP qword ptr [EDX + 0x10]"), (14, "FISTP qword ptr [EDX + 0x8]"),
          (17, "FISTP qword ptr [EDX]"), (19, "FISTP qword ptr [EDX + 0x12]"), (22, "RET")],
         "df68 12 df28 df68 08 df68 10 df7a 10 df7a 08 df3a df7a 12 c3",
         lambda rng: {"regs": rand_regs(rng, EAX=SCRATCH, EDX=SCRATCH + 64),
                      "mem": [(SCRATCH, "Out of memory".encode("utf-16le"))]}),
    Case("FILD FXCH preserves both exact integers", 0x0D02B100,
         [(0, "FILD qword ptr [EAX]"), (2, "FILD qword ptr [EAX + 0x8]"),
          (5, "FXCH ST1"), (7, "FISTP qword ptr [EDX]"),
          (9, "FISTP qword ptr [EDX + 0x8]"), (12, "RET")],
         "df28 df6808 d9c9 df3a df7a08 c3",
         lambda rng: {"regs": rand_regs(rng, EAX=SCRATCH, EDX=SCRATCH + 64),
                      "mem": [(SCRATCH, struct.pack("<QQ", rng.getrandbits(64), rng.getrandbits(64)))]}),
    Case("FILD FLD ST duplicates exact integer metadata", 0x0D02B200,
         [(0, "FILD qword ptr [EAX]"), (2, "FLD ST0"),
          (4, "FISTP qword ptr [EDX]"), (6, "FISTP qword ptr [EDX + 0x8]"), (9, "RET")],
         "df28 d9c0 df3a df7a08 c3", integer_copy_setup),
    Case("FILD FST ST copies exact integer metadata", 0x0D02B300,
         [(0, "FLDZ"), (2, "FILD qword ptr [EAX]"), (4, "FST ST1"),
          (6, "FISTP qword ptr [EDX]"), (8, "FISTP qword ptr [EDX + 0x8]"), (11, "RET")],
         "d9ee df28 ddd1 df3a df7a08 c3", integer_copy_setup),
    Case("FILD FSTP ST copies exact integer metadata", 0x0D02B400,
         [(0, "FLDZ"), (2, "FILD qword ptr [EAX]"), (4, "FSTP ST1"),
          (6, "FISTP qword ptr [EDX]"), (8, "RET")],
         "d9ee df28 ddd9 df3a c3", integer_copy_setup),
    Case("FILD arithmetic clears exact integer metadata", 0x0D02B500,
         [(0, "FILD qword ptr [EAX]"), (2, "FADD ST0,ST0"),
          (4, "FISTP qword ptr [EDX]"), (6, "RET")],
         "df28 d8c0 df3a c3", lambda rng: integer_copy_setup(rng, 1234567)),
    Case("FILD floating register copy clears stale integer metadata", 0x0D02B600,
         [(0, "FILD qword ptr [EAX]"), (2, "FLD1"), (4, "FSTP ST1"),
          (6, "FISTP qword ptr [EDX]"), (8, "RET")],
         "df28 d9e8 ddd9 df3a c3", integer_copy_setup),
    Case("FILD reused stack slot clears exact integer metadata", 0x0D02B700,
         [(0, "FILD qword ptr [EAX]"), (2, "FISTP qword ptr [EDX]"),
          (4, "FLD1"), (6, "FISTP qword ptr [EDX + 0x8]"), (9, "RET")],
         "df28 df3a d9e8 df7a08 c3", integer_copy_setup),
]
CASES += [
    Case("FILD %s sign extends into qword" % size, 0x0D02C000 + i * 0x100,
         [(0, "FILD %s ptr [EAX]" % size), (2, "FISTP qword ptr [EDX]"), (4, "RET")],
         opcode + " df3a c3", integer_copy_setup)
    for i, (size, opcode) in enumerate([("word", "df00"), ("dword", "db00")])
]
CASES += [
    Case("FILD qword narrows to %s %s" % (size, name), 0x0D02C200 + i * 0x100,
         [(0, "FILD qword ptr [EAX]"), (2, "FIST %s ptr [EDX]" % size),
          (4, "FISTP %s ptr [EDX + 0x8]" % size), (7, "RET")],
         "df28 " + store + " " + pop + "08 c3",
         lambda rng, value=value: integer_copy_setup(rng, value & 0xffffffffffffffff))
    for i, (size, store, pop, name, value) in enumerate([
        ("dword", "db12", "db5a", "in range", -123456789),
        ("dword", "db12", "db5a", "overflow", 0x80000000),
        ("word", "df12", "df5a", "in range", -12345),
        ("word", "df12", "df5a", "overflow", 0x8000)])
]


CASES += [
    Case("%s short register form writes %s" % (name, dest),
         0x0D02D100 + i * 0x100,
         [(0, "FLD double ptr [EAX]"), (2, "FLD double ptr [EAX + 0x8]"),
          (5, name + " ST1"), (5 + len(bytes.fromhex(code)), "FSTP double ptr [EDX]"),
          (7 + len(bytes.fromhex(code)), "FSTP double ptr [EDX + 0x8]"),
          (10 + len(bytes.fromhex(code)), "RET")],
         "dd00 dd4008 " + code + " dd1a dd5a08 c3",
         lambda rng: {"regs": rand_regs(rng, EAX=SCRATCH, EDX=SCRATCH + 0x100),
                      "mem": [(SCRATCH, struct.pack("<2d", 8.0, 2.0))]})
    for i, (name, code, dest) in enumerate([
        ("FADD", "d8c1", "ST0"), ("FADD", "dcc1", "ST1"),
        ("FMUL", "d8c9", "ST0"), ("FMUL", "dcc9", "ST1"),
        ("FSUB", "d8e1", "ST0"), ("FSUB", "dce9", "ST1"),
        ("FSUBR", "d8e9", "ST0"), ("FSUBR", "dce1", "ST1"),
        ("FDIV", "d8f1", "ST0"), ("FDIV", "dcf9", "ST1"),
        ("FDIVR", "d8f9", "ST0"), ("FDIVR", "dcf1", "ST1"),
        ("FMUL", "66dcc9", "ST1 with prefix")])
]


# -------------------------------------------------------------- translate --
# What an MSVC 14 /arch:SSE2 build (Heroes V's 2025 exe) emits without asking
# CPUID: packed single arithmetic, the int/single/double conversions, the
# single unpacks, and its auto-vectorised dword loops and sign extensions.
def sse_packed_setup(rng):
    floats = [rng.uniform(-1e5, 1e5) for _ in range(8)]
    ints = [rng.randrange(-1 << 31, 1 << 31) for _ in range(4)]
    return {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
            "mem": [(SCRATCH + 0x300, struct.pack("<8f4i", *floats, *ints)),
                    (SCRATCH + 0x100, b"\x00" * 0xa0)]}


def sse_int_setup(rng):
    return {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
            "mem": [(SCRATCH + 0x300, bytes(rng.randrange(256) for _ in range(0x20))),
                    (SCRATCH + 0x100, b"\x00" * 0x80)]}


CASES += [
    Case("SSE packed single arithmetic, unpacks and conversions", 0x0D02E400,
         [(0, "MOVUPS XMM0,xmmword ptr [ESI]"),
          (3, "MOVUPS XMM1,xmmword ptr [ESI + 0x10]"),
          (7, "MOVAPS XMM2,XMM0"),
          (10, "ADDPS XMM2,XMM1"),
          (13, "MOVUPS xmmword ptr [EDI],XMM2"),
          (16, "MOVAPS XMM2,XMM0"),
          (19, "MULPS XMM2,XMM1"),
          (22, "MOVUPS xmmword ptr [EDI + 0x10],XMM2"),
          (26, "MOVAPS XMM2,XMM0"),
          (29, "SUBPS XMM2,XMM1"),
          (32, "MOVUPS xmmword ptr [EDI + 0x20],XMM2"),
          (36, "MOVAPS XMM3,XMM0"),
          (39, "UNPCKLPS XMM3,XMM1"),
          (42, "MOVUPS xmmword ptr [EDI + 0x30],XMM3"),
          (46, "MOVAPS XMM3,XMM0"),
          (49, "UNPCKHPS XMM3,XMM1"),
          (52, "MOVUPS xmmword ptr [EDI + 0x40],XMM3"),
          (56, "CVTPS2PD XMM4,XMM0"),
          (59, "MOVUPS xmmword ptr [EDI + 0x50],XMM4"),
          (63, "CVTPD2PS XMM5,XMM4"),
          (67, "MOVUPS xmmword ptr [EDI + 0x60],XMM5"),
          (71, "MOVUPS XMM6,xmmword ptr [ESI + 0x20]"),
          (75, "CVTDQ2PS XMM7,XMM6"),
          (78, "MOVUPS xmmword ptr [EDI + 0x70],XMM7"),
          (82, "CVTDQ2PD XMM7,XMM6"),
          (86, "MOVUPS xmmword ptr [EDI + 0x80],XMM7"),
          (93, "CVTTPS2DQ XMM7,XMM0"),
          (97, "MOVUPS xmmword ptr [EDI + 0x90],XMM7"),
          (104, "RET")],
         "0f 10 06 0f 10 4e 10 0f 28 d0 0f 58 d1 0f 11 17 0f 28 d0 0f 59 d1 0f 11 57 10 "
         "0f 28 d0 0f 5c d1 0f 11 57 20 0f 28 d8 0f 14 d9 0f 11 5f 30 0f 28 d8 0f 15 d9 "
         "0f 11 5f 40 0f 5a e0 0f 11 67 50 66 0f 5a ec 0f 11 6f 60 0f 10 76 20 0f 5b fe "
         "0f 11 7f 70 f3 0f e6 fe 0f 11 bf 80 00 00 00 f3 0f 5b f8 0f 11 bf 90 00 00 00 c3",
         sse_packed_setup),
    Case("SSE2 packed dword arithmetic, shifts and sign extension", 0x0D02E800,
         [(0, "MOVUPS XMM0,xmmword ptr [ESI]"),
          (3, "MOVUPS XMM1,xmmword ptr [ESI + 0x10]"),
          (7, "MOVAPS XMM2,XMM0"),
          (10, "PADDD XMM2,XMM1"),
          (14, "MOVUPS xmmword ptr [EDI],XMM2"),
          (17, "MOVAPS XMM2,XMM0"),
          (20, "PSUBD XMM2,XMM1"),
          (24, "MOVUPS xmmword ptr [EDI + 0x10],XMM2"),
          (28, "MOVAPS XMM2,XMM0"),
          (31, "PCMPGTD XMM2,XMM1"),
          (35, "MOVUPS xmmword ptr [EDI + 0x20],XMM2"),
          (39, "MOVAPS XMM3,XMM0"),
          (42, "PSLLD XMM3,0x5"),
          (47, "MOVUPS xmmword ptr [EDI + 0x30],XMM3"),
          (51, "MOVAPS XMM3,XMM0"),
          (54, "PSRAD XMM3,0x3"),
          (59, "MOVUPS xmmword ptr [EDI + 0x40],XMM3"),
          (63, "MOVAPS XMM4,XMM0"),
          (66, "PUNPCKLBW XMM4,XMM4"),
          (70, "PSRAW XMM4,0x8"),
          (75, "MOVUPS xmmword ptr [EDI + 0x50],XMM4"),
          (79, "MOVAPS XMM5,XMM0"),
          (82, "PSRLDQ XMM5,0x3"),
          (87, "MOVUPS xmmword ptr [EDI + 0x60],XMM5"),
          (91, "MOVAPS XMM6,XMM0"),
          (94, "PMULLD XMM6,XMM1"),
          (99, "MOVUPS xmmword ptr [EDI + 0x70],XMM6"),
          (103, "RET")],
         "0f 10 06 0f 10 4e 10 0f 28 d0 66 0f fe d1 0f 11 17 0f 28 d0 66 0f fa d1 "
         "0f 11 57 10 0f 28 d0 66 0f 66 d1 0f 11 57 20 0f 28 d8 66 0f 72 f3 05 "
         "0f 11 5f 30 0f 28 d8 66 0f 72 e3 03 0f 11 5f 40 0f 28 e0 66 0f 60 e4 "
         "66 0f 71 e4 08 0f 11 67 50 0f 28 e8 66 0f 73 dd 03 0f 11 6f 60 "
         "0f 28 f0 66 0f 38 40 f1 0f 11 77 70 c3",
         sse_int_setup),
]


# granny2's SSE paths (half moves and non-temporal stores) and the game's SSE
# vertex path (two singles to MMn), which run once CPUID reports SSE. The
# conversion inputs include ties, NaN and values past int32.
def sse_cvt_pi_setup(rng):
    special = [2.5, -3.5, 0.5, -0.5, 1e10, -1e10, float("nan"), -2147483648.0,
               2147483520.0, 7.5]
    floats = [rng.uniform(-1e5, 1e5) for _ in range(8)] + [rng.choice(special) for _ in range(4)]
    return {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
            "mem": [(SCRATCH + 0x300, struct.pack("<12f", *floats)),
                    (SCRATCH + 0x100, b"\x00" * 0x80)]}


CASES += [
    Case("SSE half moves, non-temporal store and single to MMX conversions", 0x0D02EC00,
         [(0, "MOVUPS XMM0,xmmword ptr [ESI]"),
          (3, "MOVUPS XMM1,xmmword ptr [ESI + 0x10]"),
          (7, "MOVAPS XMM2,XMM0"),
          (10, "MOVHLPS XMM2,XMM1"),
          (13, "MOVUPS xmmword ptr [EDI],XMM2"),
          (16, "MOVAPS XMM3,XMM0"),
          (19, "MOVLHPS XMM3,XMM1"),
          (22, "MOVUPS xmmword ptr [EDI + 0x10],XMM3"),
          (26, "MOVNTPS xmmword ptr [EDI + 0x20],XMM0"),
          (30, "MOVUPS XMM4,xmmword ptr [ESI + 0x20]"),
          (34, "CVTPS2PI MM0,XMM4"),
          (37, "MOVQ qword ptr [EDI + 0x30],MM0"),
          (41, "CVTTPS2PI MM1,qword ptr [ESI + 0x28]"),
          (45, "MOVQ qword ptr [EDI + 0x38],MM1"),
          (49, "CVTPS2PI MM2,XMM1"),
          (52, "MOVQ qword ptr [EDI + 0x40],MM2"),
          (56, "EMMS"),
          (58, "RET")],
         "0f 10 06 0f 10 4e 10 0f 28 d0 0f 12 d1 0f 11 17 0f 28 d8 0f 16 d9 0f 11 5f 10 "
         "0f 2b 47 20 0f 10 66 20 0f 2d c4 0f 7f 47 30 0f 2c 4e 28 0f 7f 4f 38 "
         "0f 2d d1 0f 7f 57 40 0f 77 c3",
         sse_cvt_pi_setup),
]



class NoImage(object):
    """Complete synthetic listings need no recovery, but expose their bytes
    to disambiguate instruction forms that share the same listing text."""
    base = 0
    end = 0
    size = 0
    md = None

    def __init__(self, case=None):
        self.case = case

    def insn_end(self, addr, mnem):
        return None

    def rd32(self, va):
        return None

    def rd8(self, va):
        if self.case and self.case.addr <= va < self.case.addr + len(self.case.code):
            return self.case.code[va - self.case.addr]
        return None

    def is_exec(self, va):
        return False

    def relocated_pointers(self):
        return set()


class Opts(object):
    eager_flags = False


def translate_case(case):
    """The emitted C for one case, or the TranslateError it raised."""
    tr = T.Translator(NoImage(case), {a for c in CASES for a in (c.addr,) + c.entries}, Opts())
    insns = T.parse_listing_text(case.listing())
    fn = T.Function(case.addr, case.name, len(case.code), insns)
    fn.measure(NoImage())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn, case.entries))
    assert "FN(" not in text, "synthetic cases must not call anything"
    return text


# ------------------------------------------------------------------ build --

class Native(object):
    def __init__(self, lib_path):
        self.lib = C.CDLL(lib_path)
        self.lib.harness_mem.restype = C.POINTER(C.c_uint8)
        self.lib.harness_x86_size.restype = C.c_uint64
        self.lib.harness_run.argtypes = [C.c_uint32, C.POINTER(X86)]
        self.lib.harness_eflags.restype = C.c_uint32
        self.lib.harness_eflags.argtypes = [C.POINTER(X86)]
        assert self.lib.harness_x86_size() == C.sizeof(X86)
        self.mem = self.lib.harness_mem()

    def write(self, addr, data):
        C.memmove(C.addressof(self.mem.contents) + addr, data, len(data))

    def read(self, addr, n):
        return C.string_at(C.addressof(self.mem.contents) + addr, n)

    def zero(self, addr, n):
        C.memset(C.addressof(self.mem.contents) + addr, 0, n)


@pytest.fixture(scope="module")
def built():
    """Translate every case, compile the ones that translated into one shared
    library with the harness, and return (Native, {case name: error})."""
    clang = shutil.which("clang")
    if clang is None:
        pytest.skip("clang is not on PATH")
    if platform.system() == "Windows":
        # The harness is a POSIX shared library read through ctypes: clang's
        # MSVC target takes no -fPIC and exports nothing without dllexport.
        # The instruction semantics are checked on the other runners.
        pytest.skip("the instruction harness builds as a POSIX shared library")
    errors, parts, ok = {}, ['#include "x86.h"', ""], []
    for case in CASES:
        try:
            parts.append("/* %s */\n%s\n" % (case.name, translate_case(case)))
            ok.append(case)
        except T.TranslateError as e:
            errors[case.name] = str(e)
    work = tempfile.mkdtemp(prefix="translate-insns-")
    with open(os.path.join(work, "synth.c"), "w") as fh:
        fh.write("\n".join(parts))
    with open(os.path.join(work, "table.c"), "w") as fh:
        fh.write('#include "x86.h"\n')
        entries = [a for case in ok for a in (case.addr,) + case.entries]
        for addr in entries:
            fh.write("void fn_%08x(X86 *c);\n" % addr)
        fh.write("int recomp_is_call_return(uint32_t target) { return target == 0x%08xu; }\n" % MAGIC_RET)
        fh.write("int32_t recomp_index_of(uint32_t target) { switch (target) {\n")
        for index, addr in enumerate(entries):
            fh.write("case 0x%08xu: return %d;\n" % (addr, index))
        fh.write("default: return -1; } }\n")
        fh.write("void recomp_call(X86 *c, uint32_t target) {\n    switch (target) {\n")
        for addr in entries:
            fh.write("    case 0x%08xu: fn_%08x(c); return;\n" % (addr, addr))
        fh.write("    default: recomp_unknown_call(c, target); c->eip = rd32(c->r[R_ESP]); c->r[R_ESP] += 4;\n"
                 "    }\n}\n")
        # The harness calls each case from MAGIC_RET outside the code arena.
        # Like a non-entry CALL continuation, it returns without popping or
        # dispatching: the case's computed-return epilogue already cleaned ESP.
        fh.write("void recomp_jump(X86 *c, uint32_t target) {\n"
                 "    if (target == 0x%08xu) { c->eip = target; return; }\n"
                 "    recomp_call(c, target);\n}\n" % MAGIC_RET)
    lib = os.path.join(work, "libinsns" + (".dylib" if platform.system() == "Darwin" else ".so"))
    # The bundled libm the transcendentals call (third_party/musl-libm), its
    # names prefixed as runtime/CMakeLists.txt builds it.
    libm_dir = os.path.join(ROOT, "third_party", "musl-libm")
    objs = []
    for name in sorted(os.listdir(libm_dir)):
        if name.endswith(".c"):
            obj = os.path.join(work, "libm_" + name[:-2] + ".o")
            subprocess.check_call([clang, "-O1", "-std=c11", "-w", "-fPIC", "-ffp-contract=off",
                                   "-include", os.path.join(libm_dir, "recomp_libm_names.h"),
                                   "-c", os.path.join(libm_dir, name), "-o", obj])
            objs.append(obj)
    subprocess.check_call([clang, "-O1", "-g", "-std=c11", "-Wall", "-Wextra", "-Wno-unused",
                           "-fPIC", "-shared", "-ffp-contract=off", "-I", os.path.join(ROOT, "runtime"),
                           os.path.join(HERE, "harness.c"), os.path.join(work, "synth.c"),
                           os.path.join(work, "table.c")] + objs + ["-o", lib])
    return Native(lib), errors


# -------------------------------------------------------------------- run --

class Emu(object):
    def __init__(self):
        self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        self.u.mem_map(CODE_BASE, CODE_SIZE)
        self.u.mem_map(STACK_BASE, STACK_SIZE)
        self.u.mem_map(SCRATCH, SCRATCH_SIZE)
        self.u.mem_map(CAVE, CAVE_SIZE)
        self.interrupts = []
        self.u.hook_add(UC_HOOK_INTR, lambda u, intno, user: self.interrupts.append(intno))

    def reset(self, case):
        self.u.mem_write(STACK_BASE, b"\0" * STACK_SIZE)
        self.u.mem_write(SCRATCH, b"\0" * SCRATCH_SIZE)
        self.u.mem_write(case.addr, case.code)
        self.u.reg_write(UC_X86_REG_FPCW, FPU_CW_INIT)
        self.u.reg_write(UC_X86_REG_MXCSR, MXCSR_INIT)
        self.u.reg_write(UC_X86_REG_FPSW, 0)
        self.u.reg_write(UC_X86_REG_FPTAG, FPU_TAG_INIT)
        self.u.reg_write(UC_X86_REG_EFLAGS, EFLAGS_MISC_INIT)
        self.interrupts = []


def run_once(case, native, emu, rng):
    state = case.setup(rng)
    regs = dict(rand_regs(rng), **state.get("regs", {}))
    args = state.get("args", [])
    frame = struct.pack("<I", MAGIC_RET) + struct.pack("<%dI" % len(args), *args)

    emu.reset(case)
    native.zero(STACK_BASE, STACK_SIZE)
    native.zero(SCRATCH, SCRATCH_SIZE)
    for addr, data in state.get("mem", []):
        emu.u.mem_write(addr, data)
        native.write(addr, data)
    emu.u.mem_write(ESP_INIT, frame)
    native.write(ESP_INIT, frame)

    c = X86()
    for i, name in enumerate(REG_NAMES):
        value = ESP_INIT if name == "ESP" else regs[name]
        c.r[i] = value
        emu.u.reg_write(UC_REGS[i], value)
    c.eflags_misc = EFLAGS_MISC_INIT
    c.fpu_cw = FPU_CW_INIT
    c.fpu_tag = FPU_TAG_INIT
    c.mxcsr = MXCSR_INIT

    emu.u.emu_start(case.addr, MAGIC_RET, timeout=2 * 1000 * 1000, count=100000)
    native.lib.harness_run(case.addr, C.byref(c))

    problems = []
    for i, name in enumerate(REG_NAMES):
        u = emu.u.reg_read(UC_REGS[i])
        if c.r[i] != u:
            problems.append("%s: native %08x unicorn %08x" % (name, c.r[i], u))
    nf = native.lib.harness_eflags(C.byref(c)) & EFLAGS_COMPARED
    uf = emu.u.reg_read(UC_X86_REG_EFLAGS) & EFLAGS_COMPARED
    if nf != uf:
        problems.append("EFLAGS: native %03x unicorn %03x" % (nf, uf))
    for base, size, what in ((STACK_BASE, STACK_SIZE, "stack"), (SCRATCH, SCRATCH_SIZE, "scratch")):
        n, u = bytearray(native.read(base, size)), bytearray(emu.u.mem_read(base, size))
        for lo, length in case.ignore:
            if base <= lo < base + size:
                n[lo - base:lo - base + length] = u[lo - base:lo - base + length] = b"\0" * length
        if n != u:
            first = next(k for k in range(size) if n[k] != u[k])
            problems.append("%s differs first at %08x: native %02x unicorn %02x"
                             % (what, base + first, n[first], u[first]))
    if case.expect_intr is not None:
        assert emu.interrupts == [case.expect_intr], emu.interrupts
    assert not problems, "\n".join(problems)
    if case.check:
        native_view = _MemView(native)
        emu_view = _MemView(emu)
        case.check(native_view, emu_view)


class _MemView(object):
    """Slice guest memory of either engine by absolute address."""

    def __init__(self, engine):
        self.engine = engine

    def __getitem__(self, s):
        assert isinstance(s, slice) and s.step is None
        n = s.stop - s.start
        if isinstance(self.engine, Native):
            return self.engine.read(s.start, n)
        return bytes(self.engine.u.mem_read(s.start, n))


@pytest.mark.parametrize("case", CASES, ids=[c.name for c in CASES])
def test_case(case, built):
    native, errors = built
    if case.name in errors:
        pytest.fail("translator: %s" % errors[case.name])
    emu = Emu()
    rng = random.Random(0x4D616A ^ case.addr)
    for _ in range(40):
        try:
            run_once(case, native, emu, rng)
        except UcError as e:
            pytest.fail("unicorn: %s" % e)


def test_mmx_goes_to_the_mm_registers_and_xmm_forms_still_go_to_sse():
    """MMX is translated onto the MMn registers, because some codecs use it
    without asking CPUID, and the NFS Most Wanted race needs it.  MOVQ and MOVD
    are spelled the same for both register files, so the operands are what
    decide: an MMn operand takes the MMX path, an XMM one the SSE2 path, whose
    MOVQ also clears the upper half of the destination.  EMMS marks every x87
    register empty, which is all the x87 model has to do for it."""
    tr = T.Translator(NoImage(), {0x0D01D000}, Opts())
    insns = T.parse_listing_text(
        "0d01d000  MOVQ MM0,qword ptr [ESI]\n"
        "0d01d003  PXOR MM7,MM7\n"
        "0d01d006  PADDW MM0,MM7\n"
        "0d01d009  EMMS\n"
        "0d01d00b  MOVQ XMM0,qword ptr [EDX]\n"
        "0d01d00f  RET\n")
    fn = T.Function(0x0D01D000, "simd", 0x10, insns)
    fn.measure(NoImage())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn))
    assert "c->mm[0] = rd64(" in text
    assert "c->mm[7] = mmx_pxor(c->mm[7], c->mm[7]);" in text
    assert "c->mm[0] = mmx_padd(c->mm[0], c->mm[7], 16u);" in text
    assert "c->fpu_tag = 0xffffu;" in text
    # The XMM spelling of MOVQ keeps the SSE2 lanes, and zeroes the upper half.
    assert "c->xmm[0][0] = s0_;" in text and "c->xmm[0][2] = 0u;" in text
    # MMX must not touch the SSE2 register file, nor the reverse.
    assert "c->xmm[0][0] = c->mm" not in text


def test_segment_register_loads_do_not_reach_the_flat_model():
    """Ghidra decodes data as code now and then, and `MOV CS,[mem]` is what
    such bytes can spell.  A segment load has no meaning in the flat model
    the runtime provides: a load of CS is an invalid opcode on the CPU too,
    so it raises #UD; a load of a data segment is dropped."""
    tr = T.Translator(NoImage(), {0x0D01F000}, Opts())
    insns = T.parse_listing_text("0d01f000  MOV CS,word ptr [ECX]\n0d01f003  MOV DS,AX\n0d01f005  RET\n")
    fn = T.Function(0x0D01F000, "seg", 6, insns)
    fn.measure(NoImage())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn))
    assert "recomp_int(c, 6u);" in text
    assert "0x23u" not in text and "wr" not in text.split("MOV DS,AX")[1].split("\n")[0]


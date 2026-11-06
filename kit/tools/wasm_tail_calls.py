#!/usr/bin/env python3
"""Turn the tail calls LLVM leaves as `call; return` in WebAssembly objects into
`return_call`, in place.

    tools/wasm_tail_calls.py <archive.a or object.o> ...

The translation continues a guest jump into another function with JUMP_FN, a
call in tail position (runtime/x86.h, the generated funcs.h). Natively the
backend makes it a jump. The WebAssembly backend emits return_call only when
the call and the `ret` share a basic block, and a function with several
`return;` statements has one shared return block that CodeGenPrepare does not
duplicate for WebAssembly as it does for x86 and arm64. Each such call then
keeps its caller's frame on the browser's stack, and a guest loop spread over
two functions (Heroes V's square-root tables, 00a0b380 and 00a0b4eb) overflows
it within a second.

The rewrite is exact: in a relocatable object every call's operand carries a
relocation, so the opcode before it and the instruction after its padded
operand are known without decoding the function. A `call` (or
`call_indirect`) followed by `return` or by the function's final `end`,
directly or through the `end`s of enclosing blocks, whose callee returns
exactly what the caller returns, becomes `return_call`
(`return_call_indirect`). The bytes keep their size and the relocations their
offsets; what followed the call is left as unreachable code. An object built
with wasm exceptions is left alone (a call inside `try` must keep its frame).
Archives are patched in place, member by member (cmake/Translate.cmake runs
this on the translation's archives after they are built; patching again
finds nothing).
"""

import argparse
import sys
from pathlib import Path

CALL, CALL_INDIRECT, RETURN_CALL, RETURN_CALL_INDIRECT = 0x10, 0x11, 0x12, 0x13
RETURN, END = 0x0F, 0x0B
# Relocation types (llvm/BinaryFormat/WasmRelocs.def) that carry an addend.
ADDEND_TYPES = {3, 4, 5, 8, 9, 11, 14, 15, 16, 17, 21, 22, 23, 25}
R_FUNCTION_INDEX_LEB, R_TYPE_INDEX_LEB = 0, 6
SYMTAB, SYM_FUNCTION, SYM_DATA, SYM_SECTION = 8, 0, 1, 3
SYM_UNDEFINED, SYM_EXPLICIT_NAME = 0x10, 0x40


def uleb(b, i):
    """(value, next index) of the unsigned LEB128 at b[i]."""
    v = shift = 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7F) << shift
        if x < 0x80:
            return v, i
        shift += 7


def sleb(b, i):
    v = shift = 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7F) << shift
        shift += 7
        if x < 0x80:
            return (v - (1 << shift) if x & 0x40 else v), i


def name(b, i):
    n, i = uleb(b, i)
    return bytes(b[i:i + n]), i + n


def limits(b, i):
    flags = b[i]
    _, i = uleb(b, i + 1)
    if flags & 1:
        _, i = uleb(b, i)
    return i


class WasmObject:
    """The parts of a relocatable wasm object the rewrite reads: each type's
    results, each function's type (imports first), the code section's span and
    function bodies, the symbol table's function indices and the code
    relocations."""

    def __init__(self, b):
        if bytes(b[:4]) != b"\0asm":
            raise ValueError("not a wasm object")
        self.b = b
        self.results = []      # per type index: tuple of result value types
        self.func_types = []   # per function index (imports first): type index
        self.code = None       # (payload start, payload end) of the code section
        self.bodies = []       # (start, end) of each body, payload-relative
        self.symbols = []      # per symbol: function index, or None
        self.relocs = []       # (type, offset, index) for the code section
        self.exceptions = False  # built with wasm exceptions: a call in `try` keeps its frame
        sections = []
        i = 8
        while i < len(b):
            sid = b[i]
            size, j = uleb(b, i + 1)
            sections.append((sid, j, j + size))
            i = j + size
        code_index = None
        for k, (sid, start, end) in enumerate(sections):
            if sid == 1:
                self._types(start)
            elif sid == 2:
                self._imports(start)
            elif sid == 3:
                n, j = uleb(b, start)
                for _ in range(n):
                    t, j = uleb(b, j)
                    self.func_types.append(t)
            elif sid == 10:
                code_index = k
                self.code = (start, end)
                n, j = uleb(b, start)
                for _ in range(n):
                    size, j = uleb(b, j)
                    self.bodies.append((j - start, j - start + size))
                    j += size
        for sid, start, end in sections:
            if sid != 0:
                continue
            title, j = name(b, start)
            if title == b"linking":
                self._linking(j, end)
            elif title == b"target_features":
                n, j = uleb(b, j)
                for _ in range(n):
                    prefix = b[j]
                    feature, j = name(b, j + 1)
                    if prefix == ord("+") and feature == b"exception-handling":
                        self.exceptions = True
            elif title.startswith(b"reloc."):
                target, j = uleb(b, j)
                if target == code_index:
                    self._relocs(j)

    def _types(self, i):
        b = self.b
        n, i = uleb(b, i)
        for _ in range(n):
            if b[i] != 0x60:
                raise ValueError("type form %#x is not a function type" % b[i])
            np, i = uleb(b, i + 1)
            i += np  # value types are one byte each in these objects
            nr, i = uleb(b, i)
            self.results.append(tuple(b[i:i + nr]))
            i += nr

    def _imports(self, i):
        b = self.b
        n, i = uleb(b, i)
        for _ in range(n):
            _, i = name(b, i)
            _, i = name(b, i)
            kind = b[i]
            i += 1
            if kind == 0:
                t, i = uleb(b, i)
                self.func_types.append(t)
            elif kind == 1:
                i = limits(b, i + 1)
            elif kind == 2:
                i = limits(b, i)
            elif kind == 3:
                i += 2
            elif kind == 4:
                _, i = uleb(b, i + 1)
            else:
                raise ValueError("import kind %d" % kind)

    def _linking(self, i, end):
        b = self.b
        _, i = uleb(b, i)  # version
        while i < end:
            kind = b[i]
            size, j = uleb(b, i + 1)
            i = j + size
            if kind == SYMTAB:
                n, j = uleb(b, j)
                for _ in range(n):
                    sk = b[j]
                    flags, j = uleb(b, j + 1)
                    index = None
                    if sk == SYM_DATA:
                        _, j = name(b, j)
                        if not flags & SYM_UNDEFINED:
                            for _ in range(3):
                                _, j = uleb(b, j)
                    elif sk == SYM_SECTION:
                        _, j = uleb(b, j)
                    else:
                        index, j = uleb(b, j)
                        if not flags & SYM_UNDEFINED or flags & SYM_EXPLICIT_NAME:
                            _, j = name(b, j)
                    self.symbols.append(index if sk == SYM_FUNCTION else None)

    def _relocs(self, i):
        b = self.b
        n, i = uleb(b, i)
        out = self.relocs
        for _ in range(n):
            t = b[i]
            offset, i = uleb(b, i + 1)
            index, i = uleb(b, i)
            if t in ADDEND_TYPES:
                _, i = sleb(b, i)
            out.append((t, offset, index))

    def body_of(self, offset):
        """The index of the function body holding this payload offset (bodies are sorted)."""
        lo, hi = 0, len(self.bodies)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.bodies[mid][1] <= offset:
                lo = mid + 1
            else:
                hi = mid
        return lo

    def tail_calls(self):
        """Payload offsets of the opcodes to rewrite, with their new opcode."""
        b, base = self.b, self.code[0]
        imported = len(self.func_types) - len(self.bodies)
        out = []
        for t, off, index in self.relocs:
            if t == R_FUNCTION_INDEX_LEB:
                op, new = CALL, RETURN_CALL
            elif t == R_TYPE_INDEX_LEB:
                op, new = CALL_INDIRECT, RETURN_CALL_INDIRECT
            else:
                continue
            at = base + off
            if b[at - 1] != op:
                continue  # ref.func, or a type index of a block or a call already rewritten
            _, nxt = uleb(b, at)
            if op == CALL_INDIRECT:  # the table: a relocated number, or the one byte 0
                _, nxt = uleb(b, nxt)
            k = self.body_of(off)
            body_end = base + self.bodies[k][1]
            # Falling out of enclosing blocks (each `end` one byte) to a
            # `return` or to the function's own end is tail position too.
            while b[nxt] == END and nxt + 1 < body_end:
                nxt += 1
            if not (b[nxt] == RETURN or nxt + 1 == body_end):
                continue
            if op == CALL:
                func = self.symbols[index]
                if func is None:
                    continue
                callee = self.results[self.func_types[func]]
            else:
                callee = self.results[index]
            if callee != self.results[self.func_types[imported + k]]:
                continue
            out.append((at - 1, new))
        return out


def patch_object(b, start=0, end=None):
    """Rewrite the tail calls of the object in b[start:end]; the count."""
    view = memoryview(b)[start:end]
    obj = WasmObject(view)
    if obj.code is None or obj.exceptions:
        return 0
    calls = obj.tail_calls()
    for at, new in calls:
        view[at] = new
    return len(calls)


def ar_members(b):
    """(name, data start, data end) of each member of a System V/GNU ar archive."""
    if bytes(b[:8]) != b"!<arch>\n":
        raise ValueError("not an ar archive")
    i = 8
    while i + 60 <= len(b):
        header = bytes(b[i:i + 60])
        size = int(header[48:58].decode().strip())
        yield header[:16].decode(errors="replace").strip(), i + 60, i + 60 + size
        i += 60 + size + (size & 1)


def patch_file(path):
    """Patch an object or every wasm object in an archive, in place; the count."""
    b = bytearray(Path(path).read_bytes())
    if bytes(b[:4]) == b"\0asm":
        n = patch_object(b)
    else:
        n = 0
        for _, start, end in ar_members(b):
            if bytes(b[start:start + 4]) == b"\0asm":
                n += patch_object(b, start, end)
    if n:
        Path(path).write_bytes(b)
    return n


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("files", nargs="+", type=Path)
    args = p.parse_args()
    for f in args.files:
        print("%s: %d tail calls" % (f, patch_file(f)), flush=True)


if __name__ == "__main__":
    sys.exit(main())

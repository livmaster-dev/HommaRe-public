"""tools/wasm_tail_calls.py turns call + return into return_call in wasm objects."""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("wasm_tail_calls", ROOT / "tools/wasm_tail_calls.py")
wasm_tail_calls = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wasm_tail_calls)

# FIXTURE_SOURCE compiled with
#   clang --target=wasm32-unknown-emscripten -O2 -mtail-call -c fixture.c
# (LLVM 24, emsdk 6.0.5): fn1 and fn2 inline body, whose two tail calls reach
# one shared return block and come out as call + return; `later` calls a(c)
# and then reads memory; `value` returns geti's int from inside a block.
FIXTURE_SOURCE = """
typedef struct X86 { unsigned r[8]; unsigned f; } X86;
void a(X86 *c);
void b(X86 *c);
int geti(X86 *c);
static void body(X86 *c, unsigned entry) {
    if (entry == 3)
        goto L;
    c->r[0] += 1;
    if (c->r[1]) {
        a(c);
        return;
    }
L:
    c->r[2] += 7;
    if (c->f) {
        b(c);
        return;
    }
    c->r[3] = 9;
}
void fn1(X86 *c) { body(c, 0); }
void fn2(X86 *c) { body(c, 3); }
unsigned later(X86 *c) {
    a(c);
    return c->r[0];
}
int value(X86 *c) {
    if (c->f)
        return geti(c);
    return 0;
}
"""
FIXTURE = bytes.fromhex(
    "0061736d01000000018a808080000260017f0060017f017f02b3808080000403656e760f5f5f6c696e6561725f6d656d"
    "6f727902000003656e760161000003656e760162000003656e760467657469000103858080800004000001010aa08180"
    "8000044b002000200028020041016a36020002402000280204450d0020001080808080000f0b2000200028020841076a"
    "36020802402000280220450d0020001081808080000f0b2000410936020c0b2a002000200028020841076a3602080240"
    "2000280220450d0020001081808080000f0b2000410936020c0b0f00200010808080800020002802000b170002402000"
    "2802200d0041000f0b20001082808080000b00b980808000076c696e6b696e670208aa808080000700040303666e3100"
    "100000100100040403666e32000405056c617465720004060576616c7565001002009d808080000a72656c6f632e434f"
    "44450305001d01003e02006902007d01009a010600f8808080000970726f647563657273010c70726f6365737365642d"
    "62790105636c616e675832342e302e30676974202868747470733a2f6769746875622e636f6d2f6c6c766d2f6c6c766d"
    "2d70726f6a65637420616264336233613134343562356138656566666165356333393132383833666161393238376662"
    "3729009f818080000f7461726765745f6665617475726573092b0b62756c6b2d6d656d6f72792b0f62756c6b2d6d656d"
    "6f72792d6f70742b1663616c6c2d696e6469726563742d6f7665726c6f6e672b0a6d756c746976616c75652b0f6d7574"
    "61626c652d676c6f62616c732b136e6f6e7472617070696e672d6670746f696e742b0f7265666572656e63652d747970"
    "65732b087369676e2d6578742b097461696c2d63616c6c")


def opcodes(data):
    """The opcode before each function-index relocation, in relocation order."""
    obj = wasm_tail_calls.WasmObject(memoryview(bytearray(data)))
    base = obj.code[0]
    return [data[base + off - 1] for t, off, _ in obj.relocs if t == wasm_tail_calls.R_FUNCTION_INDEX_LEB]


def ar(members):
    """A minimal GNU ar archive of (name, bytes)."""
    out = bytearray(b"!<arch>\n")
    for name, data in members:
        out += ("%-16s%-12s%-6s%-6s%-8s%-10d`\n" % (name + "/", 0, 0, 0, 644, len(data))).encode()
        out += data + (b"\n" if len(data) & 1 else b"")
    return bytes(out)


class WasmTailCallTests(unittest.TestCase):
    def test_object(self):
        self.assertEqual(opcodes(FIXTURE), [0x10, 0x10, 0x10, 0x10, 0x10])
        data = bytearray(FIXTURE)
        self.assertEqual(wasm_tail_calls.patch_object(data), 4)
        # fn1's two, fn2's one and value's become return_call; later's call stays.
        self.assertEqual(opcodes(data), [0x12, 0x12, 0x12, 0x10, 0x12])
        self.assertEqual(len(data), len(FIXTURE))
        self.assertEqual(wasm_tail_calls.patch_object(data), 0)  # patching again finds nothing

    def test_archive_and_exceptions(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "libgen.a"
            # An odd-sized member before the object exercises the archive's padding.
            path.write_bytes(ar([("notes.txt", b"odd"), ("gen.o", FIXTURE)]))
            self.assertEqual(wasm_tail_calls.patch_file(path), 4)
            members = list(wasm_tail_calls.ar_members(path.read_bytes()))
            self.assertEqual([m[0] for m in members], ["notes.txt/", "gen.o/"])
            _, start, end = members[1]
            self.assertEqual(opcodes(path.read_bytes()[start:end]), [0x12, 0x12, 0x12, 0x10, 0x12])
            self.assertEqual(wasm_tail_calls.patch_file(path), 0)
        # An object built with wasm exceptions keeps its calls (one inside `try`
        # needs its frame): the fixture's last section, target_features,
        # rebuilt to declare them.
        at = FIXTURE.index(b"\x0ftarget_features") - 6  # the section id and its padded size
        payload = b"\x0ftarget_features" + b"\x02" + b"+\x09tail-call" + b"+\x12exception-handling"
        eh = bytearray(FIXTURE[:at] + b"\x00" + bytes([len(payload)]) + payload)
        self.assertTrue(wasm_tail_calls.WasmObject(memoryview(eh)).exceptions)
        self.assertEqual(wasm_tail_calls.patch_object(eh), 0)

    @unittest.skipUnless(os.environ.get("EMSDK") and shutil.which("emcc") and shutil.which("node"),
                         "needs the Emscripten SDK's environment")
    def test_deep_loop_runs(self):
        """Two functions that continue each other a million times overflow the
        browser's stack as calls and run as return_calls."""
        source = """
        #include <stdio.h>
        typedef struct X86 { unsigned r[8]; } X86;
        void ping(X86 *c);
        __attribute__((noinline)) void pong(X86 *c) {
            c->r[3]++;
            if (c->r[3] & 2) { ping(c); return; }
            c->r[4]++;
            ping(c);
        }
        __attribute__((noinline)) void ping(X86 *c) {
            if (c->r[0] == 0) return;
            c->r[0]--;
            if (c->r[0] & 1) { pong(c); return; }
            c->r[1] += 3;
            if (c->r[1] & 4) { pong(c); return; }
            c->r[2]++;
            pong(c);
        }
        int main(void) {
            X86 c = {{3000000}};
            ping(&c);
            printf("done %u\\n", c.r[3]);
            return 0;
        }
        """
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "deep.c").write_text(source)
            subprocess.run(["emcc", "-O2", "-mtail-call", "-c", "deep.c", "-o", "deep.o"], cwd=tmp, check=True)
            subprocess.run(["emcc", "-mtail-call", "deep.o", "-o", "plain.js"], cwd=tmp, check=True)
            plain = subprocess.run(["node", "plain.js"], cwd=tmp, capture_output=True, text=True)
            self.assertNotEqual(plain.returncode, 0)
            self.assertIn("Maximum call stack size exceeded", plain.stderr)
            self.assertGreater(wasm_tail_calls.patch_file(tmp / "deep.o"), 0)
            subprocess.run(["emcc", "-mtail-call", "deep.o", "-o", "tail.js"], cwd=tmp, check=True)
            tail = subprocess.run(["node", "tail.js"], cwd=tmp, capture_output=True, text=True)
            self.assertEqual(tail.returncode, 0, tail.stderr)
            self.assertEqual(tail.stdout, "done 3000000\n")


if __name__ == "__main__":
    unittest.main()

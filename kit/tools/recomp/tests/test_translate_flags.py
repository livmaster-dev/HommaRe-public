"""The flag cache (game.toml [translate] flag_cache, translate.cache_flags).

    .venv/bin/python -m pytest -q tools/recomp/tests/test_translate_flags.py

Text-only: the helper tables are checked against runtime/x86.h, and the
rewrite against hand-written bodies in the shape translate() emits. Whether
the cached code computes what the context form does is the instruction
harness's job (test_translate_insns.py, CACHE_MODES "cached")."""

import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
import translate as T  # noqa: E402

SIX = ("cf", "zf", "sf", "of", "pf", "af")
FLAG_RE = re.compile(r"eflags_(cf|zf|sf|of|pf|af)\b")
FLAG_WRITE_RE = re.compile(r"eflags_(cf|zf|sf|of|pf|af)\s*=(?!=)")


# ---------------------------------------------------------------- x86.h --

def header():
    with open(os.path.join(ROOT, "runtime/x86.h")) as fh:
        return fh.read()


def helper_bodies():
    """name -> body text of every function in x86.h, and of each name the
    SHIFT_OPS macro stamps out (shl8_f ... rcr32_f) as the macro's text."""
    text = header()
    bodies = {}
    for m in re.finditer(r"^(?:static inline|RECOMP_HOT_INLINE|RECOMP_COLD_OUTLINE)[^\n(]*?\b(\w+)\(([^)]*)\)\s*\{",
                         text, re.M):
        end = text.index("\n}\n", m.end())
        bodies[m.group(1)] = text[m.end():end]
    shift = re.search(r"#define SHIFT_OPS\(.*?(?<!\\)\n", text, re.S)
    assert shift, "SHIFT_OPS is gone; update REG_FREE_HELPER_RE's flag sets"
    for op in ("shl", "shr", "sar", "rol", "ror", "rcl", "rcr"):
        for bits in (8, 16, 32):
            bodies.setdefault("%s%d_f" % (op, bits), shift.group(0))
    return bodies


def touched(name, bodies, seen=None):
    """(read, written) flags of a helper, through the helpers it calls with
    the context."""
    seen = set() if seen is None else seen
    if name in seen or name not in bodies:
        return set(), set()
    seen.add(name)
    body = bodies[name]
    written = set(FLAG_WRITE_RE.findall(body))
    read = set(FLAG_RE.findall(FLAG_WRITE_RE.sub("", body)))
    for callee in re.findall(r"\b(\w+)\(\s*c\s*[,)]", body):
        r, w = touched(callee, bodies, seen)
        read |= r
        written |= w
    return read, written


def test_flag_free_helpers_leave_the_flags_alone():
    bodies = helper_bodies()
    for name in sorted(T.FLAG_FREE_HELPERS):
        if name not in bodies:
            assert re.search(r"#define %s\(" % name, header()), name
            continue
        read, written = touched(name, bodies)
        assert not read and not written, (name, read, written)


def test_flag_helpers_cover_what_they_touch():
    bodies = helper_bodies()
    names = dict(T.FLAG_HELPERS)
    for op in ("shl", "shr", "sar", "rol", "ror", "rcl", "rcr"):
        for bits in (8, 16, 32):
            names["%s%d_f" % (op, bits)] = T.flag_helper_flags("%s%d_f" % (op, bits))
    for name, (spill, reload) in sorted(names.items()):
        assert name in bodies, name
        read, written = touched(name, bodies)
        assert read <= set(spill), (name, "reads", read - set(spill))
        assert written <= set(reload), (name, "writes", written - set(reload))
        # A flag stored before but not read after must not be written.
        if not reload:
            assert not written, name


def test_fcom_and_fucom_report_in_the_status_word_not_eflags():
    """They are in REG_FREE_HELPERS and FLAG_FREE_HELPERS: FCOM writes C0-C3
    of the FPU status word. FCOMI and COMISS write EFLAGS (FLAG_HELPERS)."""
    bodies = helper_bodies()
    assert touched("fcom", bodies) == (set(), set())
    assert touched("fucom", bodies) == (set(), set())
    assert touched("fcomi", bodies)[1] == set(SIX)
    assert touched("recomp_comis", bodies)[1] == set(SIX)
    for name in ("fcomi", "fucomi", "recomp_comis"):
        assert T.FLAG_HELPERS[name][1] == T.ALL_FLAGS


def test_every_context_helper_in_x86h_is_classified_or_observing():
    """A helper missing from both tables is treated as reading and writing
    everything (correct, only slower); one that touches flags must never be
    listed as flag-free."""
    bodies = helper_bodies()
    for name in bodies:
        f = T.flag_helper_flags(name)
        if f == (T.NO_FLAGS, T.NO_FLAGS):
            assert touched(name, bodies) == (set(), set()), name


# ------------------------------------------------------------ the rewrite --

OPEN = "void fn_00401000(X86 *c) {"


def cmp_block(addr="00401000"):
    """A CMP in the emitted form: every flag stored."""
    return ["    {   /* %s CMP EAX,0x1 */" % addr,
            "        uint32_t a_ = c->r[0], b_ = 0x1u;",
            "        uint64_t rf_ = (uint64_t)a_ - b_;",
            "        uint32_t r_ = (uint32_t)rf_ & 0xffffffffu;",
            "        c->eflags_cf = (uint32_t)((rf_ >> 32) & 1u);",
            "        c->eflags_of = (uint32_t)(((a_ ^ b_) & (a_ ^ r_)) >> 31) & 1u;",
            "        c->eflags_af = ((a_ ^ b_ ^ r_) >> 4) & 1u;",
            "        c->eflags_zf = (r_ == 0);",
            "        c->eflags_sf = (r_ >> 31) & 1u;",
            "        c->eflags_pf = parity8(r_);",
            "    }"]


def call(target):
    return "    CALL_FN(%s);  /* CALL */" % target


RET = "    c->eip = rd32(c->r[4]); c->r[4] += 4u; recomp_return(c); return;  /* RET */"
ALL_SPILL = " ".join("c->eflags_%s = f%s_;" % (f, f) for f in SIX)
ALL_RELOAD = " ".join("f%s_ = c->eflags_%s;" % (f, f) for f in SIX)


def run(body, flags=SIX):
    out = T.cache_flags([OPEN] + body, 1)
    assert out is not None
    assert out[0] == OPEN
    assert out[1] == "    uint32_t " + ", ".join("f%s_ = c->eflags_%s" % (f, f) for f in flags) + ";"
    return out[2:]


def spills_on(line):
    return re.findall(r"c->eflags_(\w\w) = f\w\w_;", line)


def test_flags_become_locals_and_are_stored_at_a_call():
    out = run(cmp_block() + ["    if (!c->eflags_zf) { goto L_00401020; }", call("00402000"),
                             "L_00401020: ;", RET])
    text = "\n".join(out)
    assert "fzf_ = (r_ == 0);" in text
    assert "if (!fzf_) { goto L_00401020; }" in text
    call_line = [l for l in out if "CALL_FN(00402000)" in l][0]
    assert call_line.strip().startswith(ALL_SPILL)
    assert out[out.index(call_line) + 1].strip() == ALL_RELOAD
    # The return is reached dirty from the goto (skipping the call): stored again.
    ret_line = [l for l in out if "recomp_return" in l][0]
    assert spills_on(ret_line) == list(SIX)
    # Nothing to read back after an unconditional return.
    assert out[-1] == ret_line


def test_a_second_call_with_nothing_changed_stores_nothing():
    out = run(cmp_block() + [call("00402000"), call("00403000"), RET])
    lines = [l for l in out if "CALL_FN" in l or "recomp_return" in l]
    assert spills_on(lines[0]) == list(SIX)
    assert spills_on(lines[1]) == []
    assert spills_on(lines[2]) == []


def test_a_backward_goto_carries_its_dirty_flags_to_the_label():
    # The call at the loop head is first reached clean; the CMP at the bottom
    # jumps back dirty, so the store must be there for the second time round.
    out = run(["L_00401000: ;", call("00402000")] + cmp_block("00401010")
              + ["    if (!c->eflags_zf) { goto L_00401000; }", RET])
    call_line = [l for l in out if "CALL_FN" in l][0]
    assert spills_on(call_line) == list(SIX)


def test_a_store_inside_an_if_block_does_not_clean_the_path_around_it():
    out = run(cmp_block() + ["    if (c->r[1] != 0u) {", "    " + call("00402000"), "    }",
                             call("00403000"), RET])
    calls = [l for l in out if "CALL_FN" in l]
    assert spills_on(calls[0]) == list(SIX)
    assert spills_on(calls[1]) == list(SIX)     # the path that skipped the block


def test_a_helper_that_always_writes_cf_of_is_only_read_back():
    out = run(cmp_block() + ["    imul32(c, c->r[1]);  /* IMUL ECX */",
                             "    if (c->eflags_of) { goto L_00401040; }",
                             "L_00401040: ;", RET])
    i = [k for k, l in enumerate(out) if "imul32" in l][0]
    assert spills_on(out[i]) == []
    assert out[i + 1].strip() == "fcf_ = c->eflags_cf; fof_ = c->eflags_of;"
    assert "if (fof_)" in out[i + 2]


def test_a_shift_that_may_leave_the_flags_is_stored_before_and_read_after():
    out = run(cmp_block() + ["    c->r[0] = shl32_f(c, c->r[0], c->r[1] & 0xffu);  /* SHL EAX,CL */",
                             "    if (c->eflags_zf) { goto L_00401040; }",
                             "L_00401040: ;", RET])
    i = [k for k, l in enumerate(out) if "shl32_f" in l][0]
    assert spills_on(out[i]) == ["cf", "zf", "sf", "of", "pf"]
    assert out[i + 1].strip() == "fcf_ = c->eflags_cf; fzf_ = c->eflags_zf; fsf_ = c->eflags_sf; " \
                                 "fof_ = c->eflags_of; fpf_ = c->eflags_pf;"


def test_pushfd_reads_the_flags_from_the_context():
    out = run(cmp_block() + ["    { uint32_t v_ = x86_get_eflags(c); c->r[4] -= 4; wr32(c->r[4], v_); }", RET])
    i = [k for k, l in enumerate(out) if "x86_get_eflags" in l][0]
    assert spills_on(out[i]) == list(SIX)
    assert "= c->eflags_" not in out[i + 1]       # nothing written: no reload


def test_comis_writes_every_flag():
    out = run(cmp_block() + ["    recomp_comis(c, 1.0, 2.0);  /* COMISD */",
                             "    if (c->eflags_cf) { goto L_00401040; }", "L_00401040: ;", RET])
    i = [k for k, l in enumerate(out) if "recomp_comis" in l][0]
    assert spills_on(out[i]) == []
    assert out[i + 1].strip() == ALL_RELOAD


def test_fcom_is_not_an_observer():
    out = run(cmp_block() + ["    fcom(c, ST(c, 0), ST(c, 1));  /* FCOM */", call("00402000"), RET])
    i = [k for k, l in enumerate(out) if "fcom(" in l][0]
    assert spills_on(out[i]) == []
    assert "= c->eflags_" not in out[i + 1]
    assert spills_on([l for l in out if "CALL_FN" in l][0]) == list(SIX)


def test_an_unknown_context_use_is_an_observer():
    for line in ("    recomp_rdtsc(c);", "    FN(00402000)(c);", "    body_00401100(c, 0x401100u);",
                 "    SOME_MACRO(1);"):
        out = run(cmp_block() + [line, RET])
        i = [k for k, l in enumerate(out) if line.strip() in l][0]
        assert spills_on(out[i]) == list(SIX), line
        assert out[i + 1].strip() == ALL_RELOAD, line


def test_a_jump_table_arm_that_leaves_stores_after_its_label():
    out = run(cmp_block() + [
        "    {   /* JMP dword ptr [EAX*4 + 0x401100] */",
        "        uint32_t t_ = rd32(c->r[0] * 4u + 0x401100u);",
        "        switch (t_) {",
        "        case 0x401040u: goto L_00401040;",
        "        default: c->eip = t_; recomp_jump(c, t_); return;",
        "        }",
        "    }",
        "L_00401040: ;", RET])
    arm = [l for l in out if "default:" in l][0]
    assert arm.strip().startswith("default: " + ALL_SPILL)
    assert arm.strip().endswith("recomp_jump(c, t_); return;")
    # The case's goto carries the CMP's flags to the label, so the RET stores.
    assert spills_on([l for l in out if "recomp_return" in l][0]) == list(SIX)


def test_a_label_named_other_than_by_goto_starts_dirty():
    out = run([call("00402000"), "    c->eip = 0x401040u; /* L_00401040 */ x_ = (uintptr_t)&&L_00401040;",
               "L_00401040: ;", call("00403000"), "    if (c->eflags_zf) { goto L_00401040; }", RET],
              flags=("zf",))
    second = [l for l in out if "CALL_FN(00403000)" in l][0]
    assert spills_on(second) == ["zf"]


@pytest.mark.parametrize("body", [
    ["    setjmp_like(c);", "    if (c->eflags_zf) { goto L_00401010; }", "L_00401010: ;"],
    ["    uint32_t *p_ = &c->eflags_zf;", "    *p_ = 1u;"],
    ["    while (c->r[1]) {", "        c->eflags_zf = 0;", "    }"],
    ["    if (c->r[1]) {", "        c->eflags_zf = 0;", "    } else {", "        c->eflags_zf = 1;", "    }"],
    ["    if (c->eflags_zf) { CALL_FN(00402000); goto L_00401010; }", "L_00401010: ;"],
    ["    if (c->eflags_zf) CALL_FN(00402000); {"],
])
def test_shapes_it_does_not_model_are_left_alone(body):
    assert T.cache_flags([OPEN] + body + [RET], 1) is None


def test_no_flags_no_rewrite():
    assert T.cache_flags([OPEN, call("00402000"), RET], 1) is None


def test_the_prologue_is_untouched_and_the_declaration_comes_first():
    head = [
        "static void body_00401000(X86 *c, uint32_t entry_) {",
        "    switch (entry_) {",
        "    case 0x401040u: goto L_00401040;",
        "    default: break;",
        "    }",
    ]
    body = cmp_block() + ["L_00401040: ;", "    if (c->eflags_zf) { goto L_00401040; }", RET]
    out = T.cache_flags(head + body, len(head))
    assert out[0] == head[0]
    assert out[1].startswith("    uint32_t fcf_ = c->eflags_cf")
    assert out[2:2 + len(head) - 1] == head[1:]
    # A dispatch entry reaches its label with the flags just loaded: clean, but
    # the CMP before it makes the label dirty on the fall-through path.
    assert spills_on([l for l in out if "recomp_return" in l][0]) == list(SIX)


# ------------------------------------------------------------- end to end --

def test_the_instruction_cases_translate_with_every_cache_on():
    """Every synthetic case of the instruction harness translates with
    reg_cache and flag_cache on, and its flag fields are only named on the
    lines that store or read them back, or that hand the context to
    something that reads them."""
    import test_translate_insns as I
    saved = T.REG_CACHE, T.FLAG_CACHE
    T.REG_CACHE = T.FLAG_CACHE = True
    try:
        cached = 0
        for case in I.CASES:
            try:
                text = I.translate_case(case)
            except T.TranslateError:
                continue
            if "uint32_t fcf_" in text or re.search(r"uint32_t f\w\w_ = c->eflags_", text):
                cached += 1
            for line in text.split("\n"):
                code = T._COMMENT.sub("", line)
                if not T._FLAG_REF.search(code):
                    continue
                observer = (T._MACRO_CALL.search(code) or T._RETURN.search(code)
                            or any(T.flag_helper_flags(n) != (T.NO_FLAGS, T.NO_FLAGS)
                                   for n in T._CTX_CALL.findall(code)))
                declares = re.match(r"\s*uint32_t f\w\w_ = c->eflags_", line)
                stores_or_reads = re.fullmatch(r"\s*(?:f\w\w_ = c->eflags_\w\w;\s*)+", code)
                assert observer or declares or stores_or_reads, (case.name, line)
        assert cached > 0
    finally:
        T.REG_CACHE, T.FLAG_CACHE = saved

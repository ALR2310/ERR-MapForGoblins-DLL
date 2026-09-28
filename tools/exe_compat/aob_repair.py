"""Why does this AOB miss on other builds, and what pattern would not?

A signature misses on a downpatched exe for one of two reasons, and they need opposite fixes:
  - it BAKED a build-specific operand (a rip-relative displacement, a rel32 call target).
    Those bytes are different on every build; wildcard them.
  - it ran PAST the end of the function it identifies, into whatever the linker put next.
    Those bytes belong to another function; cut them.

This disassembles the signature's match on the build-target exe, marks both, and prints a
repaired pattern together with its match count on every build we hold - so the repair is
measured before it is pasted into tools/aob_signatures.py.

  py tools\\exe_compat\\aob_repair.py map_wmd_dtor_hook stallprobe_job_poll
  py tools\\exe_compat\\aob_repair.py --all-broken
"""
import argparse
import sys
from pathlib import Path

import capstone

PROJ = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJ / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from aob_signatures import SIGNATURES  # noqa: E402
from anchor_patterns import fmt_pattern, volatile_ranges  # noqa: E402
from check_aobs import to_regex  # noqa: E402
from known_exes import known_exes, text_section  # noqa: E402


def analyse(code, sec_va, off, length):
    """[(byte, wildcard, note)] over the signature's bytes, plus the offset its function ends
    at (or None). Notes name WHY a byte is wildcarded, so the printed pattern is auditable."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    out, end_at, furthest = [], None, 0
    for insn in md.disasm(code[off:off + length + 16], sec_va + off):
        pos = insn.address - (sec_va + off)
        if pos >= length:
            break
        vol = volatile_ranges(insn)
        note = ""
        if vol:
            note = "rip-relative" if any(
                o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP
                for o in insn.operands) else "rel32 target"
        for i, b in enumerate(insn.bytes):
            if len(out) < length:
                out.append((b, i in vol, note if i in vol else ""))
        if capstone.CS_GRP_JUMP in set(insn.groups) and insn.operands and \
                insn.operands[0].type == capstone.x86.X86_OP_IMM and \
                (insn.mnemonic != "jmp" or insn.size == 2):
            t = insn.operands[0].imm - (sec_va + off)
            if 0 < t < length:
                furthest = max(furthest, t)
        if end_at is None and insn.mnemonic in ("ret", "int3", "jmp") and \
                pos + insn.size > furthest:
            end_at = pos + insn.size
    while len(out) < length:
        out.append((code[off + len(out)], False, ""))
    return out, end_at


def counts(texts, needle, mask):
    res = []
    for ver, code in texts.items():
        pat = " ".join("??" if not m else f"{b:02X}" for b, m in zip(needle, mask))
        n = len(to_regex(pat).findall(code))
        res.append((ver, n))
    return res


def report(sig, texts, target_ver):
    code = texts[target_ver]
    rx = to_regex(sig["pattern"])
    hits = [m.start() for m in rx.finditer(code)]
    print(f"\n=== {sig['name']}  ({len(hits)} match(es) on {target_ver})")
    if len(hits) != 1:
        print("  not uniquely located on the target exe - repair it by hand")
        return
    ln = len(sig["pattern"].split())
    ann, end_at = analyse(code, 0, hits[0], ln)
    # keep the signature's own wildcards, add the operand ones
    own = [t == "??" for t in sig["pattern"].split()]
    marks = [(b, w or own[i]) for i, (b, w, _) in enumerate(ann)]
    for i, (_, w, note) in enumerate(ann):
        if w and not own[i] and (i == 0 or not ann[i - 1][1]):
            print(f"  byte +{i}: baked {note} - differs on every build")
    if end_at is not None and end_at < ln:
        print(f"  the function ends at byte +{end_at}; the last {ln - end_at} bytes belong "
              f"to whatever follows it")

    for label, keep in (("wildcarded", ln), ("wildcarded + cut to the function",
                                             end_at if end_at else ln)):
        if keep is None or keep <= 0:
            continue
        needle = bytes(b for b, _ in marks[:keep])
        mask = [0 if w else 1 for _, w in marks[:keep]]
        c = counts(texts, needle, mask)
        verdict = "USABLE" if all(n == 1 for _, n in c) else "no"
        print(f"  [{verdict:6}] {label} ({keep} bytes): "
              + " ".join(f"{v}={n}" for v, n in c))
        if verdict == "USABLE":
            print("        " + fmt_pattern(needle, mask, wrap=68, indent=19))
            return

    # Neither shrinking nor wildcarding was enough: the remaining copies are byte-identical
    # up to here, so the pattern has to reach FURTHER for something that tells them apart.
    # Growth is instruction-aware, one instruction at a time, and every step is counted on
    # every build - a longer pattern that only works on the target exe is the failure this
    # whole exercise is about.
    base = end_at if (end_at and end_at < ln) else ln
    grown, _ = analyse(code, 0, hits[0], min(base + 96, 240))
    keep_marks = [(b, w or (i < len(own) and own[i])) for i, (b, w, _) in enumerate(grown)]
    n = base
    while n < len(keep_marks):
        n += 1
        if keep_marks[n - 1][1]:
            continue  # a wildcard tail discriminates nothing
        needle = bytes(b for b, _ in keep_marks[:n])
        mask = [0 if w else 1 for _, w in keep_marks[:n]]
        c = counts(texts, needle, mask)
        if all(k == 1 for _, k in c):
            reach = "" if n <= (end_at or ln) else \
                f" - reaches {n - (end_at or ln)} bytes past the function's end"
            print(f"  [USABLE] grown to {n} bytes{reach}: "
                  + " ".join(f"{v}={k}" for v, k in c))
            print("        " + fmt_pattern(needle, mask, wrap=68, indent=19))
            return
    print("  [no    ] no length up to "
          f"{len(keep_marks)} bytes is a single match on every build")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("names", nargs="*")
    ap.add_argument("--all-broken", action="store_true",
                    help="every signature that is not a single match on every build")
    a = ap.parse_args()

    exes = known_exes()
    target = next(e for e in exes if e.is_target)
    texts = {e.version: text_section(e.path)[0] for e in exes}

    names = set(a.names)
    if a.all_broken or not names:
        for sig in SIGNATURES:
            rx = to_regex(sig["pattern"])
            if any(len(rx.findall(texts[e.version])) != 1 for e in exes):
                names.add(sig["name"])
    for sig in SIGNATURES:
        if sig["name"] in names:
            report(sig, texts, target.version)
    return 0


if __name__ == "__main__":
    sys.exit(main())

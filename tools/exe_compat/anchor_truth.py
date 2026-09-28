"""Report 31's missing instrument: is each RESOLVED anchor the RIGHT function, or merely A
function that matched?

anchor_test.exe answers "did we find something" - dead / order-dropped counts. That metric
printed 46/46 green on every exe while the resolver was placing clip_proxy_dtor on a
stranger, because finding is not identifying. This answers the other question, and it is the
one that decides whether the mod is safe on a build.

Ground truth = the function body at the BAKED rva on the build-target exe. It is disassembled
to its end and compared instruction by instruction against the body at the resolved address
on each foreign exe. Comparison ignores exactly what a rebuild legitimately changes - the
value of a rip-relative displacement and of a rel32 branch target - and nothing else. So a
correctly re-found helper reads IDENTICAL, and a stranger diverges at some instruction, which
gets printed.

  py tools\\exe_compat\\anchor_truth.py            # summary + every divergence
  py tools\\exe_compat\\anchor_truth.py --all      # also list the anchors that verified
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

import capstone

PROJ = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJ / "tools"))
from known_exes import known_exes, text_section  # noqa: E402

TEST = PROJ / "builds" / "anchor_test" / "anchor_test.exe"
MAX_BODY = 0x600


def disasm_body(code, sec_va, rva):
    """[(offset, mnemonic, normalized operands)] for the function at rva, to its end.

    Normalisation replaces a rip-relative displacement with 'rip+X' and a branch target with
    'L' - the two things that legitimately differ between builds of the same function. Every
    other byte must agree, so this is a comparison, not a similarity score.
    """
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    off = rva - sec_va
    if off < 0 or off + MAX_BODY > len(code):
        return []
    out, furthest = [], 0
    for insn in md.disasm(code[off:off + MAX_BODY], rva):
        pos = insn.address - rva
        ops = insn.op_str
        if "rip +" in ops or "rip -" in ops:
            ops = re.sub(r"rip [+-] 0x[0-9a-f]+", "rip+X", ops)
        grp = set(insn.groups)
        if (capstone.CS_GRP_JUMP in grp or capstone.CS_GRP_CALL in grp) and \
                insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
            tgt = insn.operands[0].imm - rva
            # An INTRA-function jump keeps its relative target: that is structure, and a
            # stranger with the same mnemonics but different block layout must not pass.
            ops = f"L{tgt:+d}" if 0 <= tgt < MAX_BODY else "Lext"
            # Only a jump that CONTROL FALLS BACK FROM extends the body: a conditional jump
            # always, an unconditional one only in its 2-byte rel8 form (an MSVC jump over an
            # else-branch). A 5-byte `jmp rel32` is a tail call - menu_row_build_dispatch ends
            # in two of them, and counting their targets as body kept the walk going 576 bytes
            # past the ret, straight into the next function and a desynced decode.
            internal = (capstone.CS_GRP_JUMP in grp and
                        (insn.mnemonic != "jmp" or insn.size == 2))
            if internal and 0 < tgt < MAX_BODY:
                furthest = max(furthest, tgt)
        out.append((pos, insn.mnemonic, ops))
        # A function ends at a ret, an int3, or a TAIL JUMP - and only when nothing already
        # decoded jumps past that point. Leaving `jmp` out of the list is what made this tool
        # report five false divergences: menu_row_build_dispatch ends in a tail call, so the
        # walk ran into the next function and desynced mid-instruction on BOTH exes.
        if insn.mnemonic in ("ret", "int3", "jmp") and pos + insn.size > furthest:
            break
    return out


def parse_test(exe_paths):
    r = subprocess.run([str(TEST)] + [str(p) for p in exe_paths],
                       capture_output=True, text=True, errors="replace")
    per_exe, cur = {}, None
    for line in r.stdout.splitlines():
        if line.startswith("=== "):
            # the header carries a timing suffix; the key is the path alone
            cur = line[4:].split("   (")[0].strip()
            per_exe[cur] = {}
        m = re.match(r"\s+\[(AT|REBASE|DEAD)\s*\]\s+(\S+)\s+0x([0-9A-Fa-f]+)"
                     r"(?:\s+->\s+0x([0-9A-Fa-f]+))?", line)
        if m and cur:
            baked = int(m.group(3), 16)
            got = int(m.group(4), 16) if m.group(4) else 0
            per_exe[cur][m.group(2)] = (baked, got)
    return per_exe


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--all", action="store_true", help="list verified anchors too")
    a = ap.parse_args()

    if not TEST.exists():
        print("builds/anchor_test/anchor_test.exe missing - run tools/exe_compat/run_anchor_test.py first")
        return 1
    exes = known_exes()
    target = next(e for e in exes if e.is_target)
    ref, ref_va = text_section(target.path)
    resolved = parse_test([e.path for e in exes])

    bad = 0
    for e in exes:
        if e.is_target:
            continue
        code, va = text_section(e.path)
        got = resolved.get(str(e.path), {})
        print(f"\n=== {e.version}  ({len(got)} anchors)")
        ok = []
        for name, (baked, addr) in got.items():
            if not addr:
                print(f"  [UNPLACED] {name}")
                continue
            want = disasm_body(ref, ref_va, baked)
            have = disasm_body(code, va, addr)
            if not want:
                print(f"  [NO-REF  ] {name}: no body at the baked rva on the target exe")
                continue
            diff = None
            for i, w in enumerate(want):
                if i >= len(have):
                    diff = (i, w, None)
                    break
                if (w[1], w[2]) != (have[i][1], have[i][2]):
                    diff = (i, w, have[i])
                    break
            if diff is None and len(have) != len(want):
                diff = (len(want), None, have[len(want)] if len(have) > len(want) else None)
            if diff is None:
                ok.append(f"{name} ({len(want)} insns)")
                continue
            bad += 1
            i, w, h = diff
            print(f"  [DIFFERS ] {name:<28} 0x{baked:X} -> 0x{addr:X} "
                  f"(shift {addr - baked:+#x}), insn #{i}:")
            print(f"               target: {w[1] + ' ' + w[2] if w else '<end of body>'}")
            print(f"               here  : {h[1] + ' ' + h[2] if h else '<end of body>'}")
        print(f"  ---- {len(ok)} bodies identical, {len(got) - len(ok)} not")
        if a.all:
            for s in ok:
                print(f"       ok: {s}")

    print(f"\n{bad} resolutions land on a body that is not the anchored function")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

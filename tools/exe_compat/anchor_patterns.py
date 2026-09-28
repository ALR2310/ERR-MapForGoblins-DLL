"""Report 31 fix, step 1: give every anchor a pattern that identifies it, not just matches it.

The shipped anchors carry a flat 16-byte prefix. 33 of 44 of those are shorter than uniqueness
needs - clip_proxy_dtor's matches 3638 places in .text - so the resolver's "is it at home"
test (R1) and its nearest-to-prediction rounds are both deciding on evidence that cannot tell
two functions apart. Merely lengthening the prefix does not work either: the extra bytes
swallow rip-relative displacements and rel32 branch targets, which differ on every build, and
the pattern then matches ZERO times on a shifted exe.

So the pattern is grown INSTRUCTION BY INSTRUCTION with capstone, wildcarding exactly the
operand bytes that are build-specific:
  - a rip-relative memory operand's disp32
  - a call/jmp/jcc rel32 target
and stopping at the function's end (the ret whose successor is padding), because bytes past
it belong to whatever the next build put there - the grid_cursor_get lesson.

Output: a ready-to-paste ANCHORS list, plus a per-anchor report of the length needed and the
match count on every exe we hold.

  py tools\\exe_compat\\anchor_patterns.py                 # report only
  py tools\\exe_compat\\anchor_patterns.py --write         # rewrite tools/rva_anchors.py in place
"""
import argparse
import re
import sys
from pathlib import Path

import capstone

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
from known_exes import known_exes, text_section  # noqa: E402
from rva_anchors import ANCHORS  # noqa: E402

MAX_LEN = 160          # a pattern longer than this buys nothing the shift prior cannot
PAD = (0xCC, 0x90)     # int3 / nop - what MSVC puts between functions


def volatile_ranges(insn):
    """Byte offsets inside insn that differ between builds (rip disp32, branch rel32)."""
    out = set()
    enc = insn.encoding
    # rip-relative displacement: the operand names an address that moved with the section.
    for op in insn.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            if enc.disp_offset and enc.disp_size:
                out.update(range(enc.disp_offset, enc.disp_offset + enc.disp_size))
    # rel32 branch target - same reason, and every call in a moved function has one.
    grp = set(insn.groups)
    is_branch = (capstone.CS_GRP_CALL in grp or capstone.CS_GRP_JUMP in grp)
    if is_branch and enc.imm_offset and enc.imm_size == 4:
        out.update(range(enc.imm_offset, enc.imm_offset + enc.imm_size))
    return out


def function_pattern(code, sec_va, rva, max_len=MAX_LEN):
    """Walk the function at rva; return [(byte, is_wildcard), ...] up to its end.

    "Its end" = a ret/jmp whose next byte is padding, or the byte cap. Internal jumps are
    respected: a ret in the middle of a function (an early-out) is only treated as the end
    when nothing already seen jumps past it.
    """
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    off = rva - sec_va
    if off < 0 or off >= len(code):
        return []
    blob = code[off:off + max_len + 16]
    pat = []
    furthest = 0  # highest intra-function offset any jump we have seen targets
    for insn in md.disasm(blob, rva):
        pos = insn.address - rva
        if pos >= max_len:
            break
        vol = volatile_ranges(insn)
        for i, b in enumerate(insn.bytes):
            pat.append((b, i in vol))
        end = pos + insn.size
        # track intra-function jump targets so an early ret does not truncate the pattern
        # Only a jump control falls back from extends the body: a conditional one always, an
        # unconditional one only in its 2-byte rel8 form. A 5-byte `jmp rel32` is a tail call,
        # and treating its target as body walks the pattern into the next function.
        if capstone.CS_GRP_JUMP in set(insn.groups) and insn.operands and \
                insn.operands[0].type == capstone.x86.X86_OP_IMM and \
                (insn.mnemonic != "jmp" or insn.size == 2):
            tgt = insn.operands[0].imm - rva
            if 0 < tgt < max_len:
                furthest = max(furthest, tgt)
        # Stop at the first ret, int3 or TAIL JUMP that nothing already seen jumps past.
        # Padding is NOT part of the test: on the target exe grid_cursor_get's ret is followed
        # straight by the next function, so a padding-gated stop walked into it, grew the
        # pattern to 8 bytes and the anchor then matched ZERO times on 2.6.0 - a pattern must
        # never contain a byte the function does not own. A tail `jmp` ends a function just as
        # a ret does (menu_row_build_dispatch is one), so it terminates the walk too.
        # An INDIRECT tail jump (jmp rax, jmp [rip+..]) ends a function just as a rel32 one
        # does, so the terminator is the mnemonic, not the operand kind.
        if insn.mnemonic in ("ret", "int3", "jmp") and end > furthest:
            break
    return pat[:max_len]


def pat_to_bytes(pat, n):
    """(needle, mask) for the first n entries; wildcards excluded from the needle compare."""
    return bytes(b for b, _ in pat[:n]), [0 if w else 1 for _, w in pat[:n]]


SEED = 4  # bytes scanned for with bytes.find; must stay SHORT or it does the discriminating
          # itself and every anchor reports a fictitious "unique at 4 bytes"


def seed_run(pat):
    """(lead, seed) - the first SEED fixed bytes. Scanning for that with bytes.find gives the
    candidate set in one C-level pass; every longer prefix is then a filter over that list,
    so growing the pattern costs nothing per extra byte. The seed is deliberately capped:
    seeding with the WHOLE leading fixed run made the search report length 4 for anchors
    whose real requirement is 34+, because the seed had already excluded everything."""
    lead = next((i for i, (_, w) in enumerate(pat) if not w), None)
    if lead is None:
        return 0, b""
    j = lead
    while j < len(pat) and not pat[j][1] and j - lead < SEED:
        j += 1
    return lead, bytes(b for b, _ in pat[lead:j])


def candidates(code, pat):
    """Every offset in code where pat's leading fixed run sits."""
    lead, seed = seed_run(pat)
    if not seed:
        return []
    out, i = [], 0
    while True:
        i = code.find(seed, i)
        if i < 0:
            return out
        if i - lead >= 0:
            out.append(i - lead)
        i += 1


def survivors(code, pat, n, starts):
    """Which of starts still match pat's first n entries."""
    keep = []
    for s in starts:
        if s < 0 or s + n > len(code):
            continue
        if all(w or code[s + k] == b for k, (b, w) in enumerate(pat[:n])):
            keep.append(s)
    return keep


def count_matches(code, pat, n):
    return len(survivors(code, pat, n, candidates(code, pat)))


def fmt_pattern(needle, mask, wrap=72, indent=15):
    """The pattern as source text. Long ones are split into adjacent string literals so the
    table stays readable; Python concatenates them and the reader sees one pattern."""
    toks = ["??" if not m else f"{b:02X}" for b, m in zip(needle, mask)]
    lines, cur = [], ""
    for t in toks:
        if cur and len(cur) + 1 + len(t) > wrap:
            lines.append(cur + " ")
            cur = t
        else:
            cur = f"{cur} {t}" if cur else t
    lines.append(cur)
    if len(lines) == 1:
        return lines[0]
    return ('"\n' + " " * indent + '"').join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--write", action="store_true",
                    help="rewrite the bytes field of every anchor in tools/rva_anchors.py")
    args = ap.parse_args()

    exes = known_exes()
    target = next((e for e in exes if e.is_target), None)
    if not target:
        print("No build-target exe (game_dir). Set it in tools/config.ini.")
        return 1
    others = [e for e in exes if not e.is_target]
    texts = {e.version: text_section(e.path) for e in exes}
    ref, ref_va = texts[target.version]

    print(f"target {target.version}; also checking {', '.join(e.version for e in others)}\n")
    print(f"{'anchor':<28} {'cur':>4} {'new':>4} {'wild':>4}  matches on "
          f"{'/'.join([target.version] + [e.version for e in others])}")
    print("-" * 92)

    new_bytes = {}
    unresolvable = []
    for a in ANCHORS:
        cur = len(a["bytes"].split())
        pat = function_pattern(ref, ref_va, a["rva"])
        if not pat:
            print(f"{a['name']:<28} {cur:>4}    -     -  RVA outside .text")
            unresolvable.append(a["name"])
            continue
        # Grow until unique on the target exe; a wildcard tail cannot help, so the chosen
        # length always ends on a fixed byte.
        alive = candidates(ref, pat)
        chosen, n = len(pat), SEED
        while n <= len(pat):
            alive = survivors(ref, pat, n, alive)
            if len(alive) == 1 and not pat[n - 1][1]:
                chosen = n
                break
            n += 1
        needle, mask = pat_to_bytes(pat, chosen)
        counts = [len(alive)]
        for e in others:
            code, _ = texts[e.version]
            counts.append(count_matches(code, pat, chosen))
        wild = sum(1 for m in mask if not m)
        # A PIN matches exactly once on every build we hold: it identifies its function, so
        # the resolver can trust it outright and use it to establish the local shift. Anything
        # else is a FOLLOWER - the bytes alone cannot tell its copies apart, so it may only be
        # placed relative to the pins around it, never accepted on its own evidence.
        if counts[0] != 1:
            kind, why = "FOLLOWER", f"{counts[0]} identical copies on the target exe"
        elif any(c == 0 for c in counts[1:]):
            kind, why = "FOLLOWER", "vanishes on a shifted exe"
        elif any(c > 1 for c in counts[1:]):
            kind, why = "FOLLOWER", "ambiguous on a shifted exe"
        else:
            kind, why = "pin", ""
        if kind == "FOLLOWER":
            unresolvable.append(f"{a['name']} ({why})")
        print(f"{a['name']:<28} {cur:>4} {chosen:>4} {wild:>4}  "
              f"{'/'.join(str(c) for c in counts):<20} {kind}")
        new_bytes[a["name"]] = fmt_pattern(needle, mask)

    print(f"\n{len(new_bytes)} patterns generated: {len(new_bytes) - len(unresolvable)} pins, "
          f"{len(unresolvable)} followers")
    for u in unresolvable:
        print(f"  follower: {u}")

    if args.write:
        src = (TOOLS / "rva_anchors.py").read_text(encoding="utf-8")
        n = 0
        kinds = {name: ("follower" if any(u.startswith(name + " ") for u in unresolvable)
                        else "pin") for name in new_bytes}
        block = ["ANCHOR_KIND = {"]
        for name, k in kinds.items():
            block.append(f'    "{name}": "{k}",')
        block.append("}")
        src = re.sub(r"# <kinds>\n.*?# </kinds>",
                     "# <kinds>\n" + "\n".join(block) + "\n# </kinds>", src, flags=re.S)
        for name, pattern in new_bytes.items():
            # Replace only the bytes line that follows this anchor's name line.
            rx = re.compile(r'(\{"name": "' + re.escape(name) +
                            r'",.*?"bytes": ")([0-9A-F? \n"\s]*?)(",)', re.S)
            new_src, k = rx.subn(lambda m: m.group(1) + pattern + m.group(3), src, count=1)
            if k:
                src, n = new_src, n + 1
            else:
                print(f"  WARN could not rewrite {name}")
        (TOOLS / "rva_anchors.py").write_text(src, encoding="utf-8")
        print(f"rewrote {n} anchor patterns in tools/rva_anchors.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""For a signature that matches N times: are those N places the SAME function?

A pattern with an exact byte-twin is not automatically broken. refcount_addref has four
copies and any of them is the same call - what matters is whether the copies are identical
all the way to their ends, in which case picking one is safe, or whether they merely share a
prologue, in which case picking one is the report-31 mistake.
"""
import sys
from pathlib import Path

PROJ = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJ / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from aob_signatures import SIGNATURES  # noqa: E402
from anchor_truth import disasm_body  # noqa: E402
from check_aobs import to_regex  # noqa: E402
from known_exes import known_exes, text_section  # noqa: E402

args = sys.argv[1:]
# --pattern "<aob>" checks a candidate pattern that is not in the table yet, which is the
# case that matters when repairing one: the ORIGINAL pattern is unique on the target exe and
# only its wildcarded successor has twins.
pattern = None
if args and args[0] == "--pattern":
    pattern = args[1]
    args = args[2:]
names = args
for e in known_exes():
    code, va = text_section(e.path)
    probes = ([{"name": "<candidate>", "pattern": pattern}] if pattern
              else [s for s in SIGNATURES if not names or s["name"] in names])
    for sig in probes:
        hits = [m.start() for m in to_regex(sig["pattern"]).finditer(code)]
        if len(hits) < 2:
            continue
        print(f"\n=== {sig['name']}: {len(hits)} matches on {e.version}")
        bodies = []
        for h in hits[:8]:
            b = disasm_body(code, va, va + h)
            bodies.append([(m, o) for _, m, o in b])
            print(f"  0x{va + h:X}  {len(b)} insns")
        same = all(b == bodies[0] for b in bodies)
        print(f"  -> bodies {'IDENTICAL - either copy is the same code' if same else 'DIFFER'}")
        if not same:
            for i, b in enumerate(bodies[1:], 1):
                for k, (w, h2) in enumerate(zip(bodies[0], b)):
                    if w != h2:
                        print(f"     copy {i} diverges at insn #{k}: "
                              f"{w[0]} {w[1]}   vs   {h2[0]} {h2[1]}")
                        break

"""Build builds/anchor_test/anchor_test.exe from the SHIPPING resolver and run it on every exe we hold.

The compile is part of the run on purpose: the near-miss worth remembering here is that the
DLL once carried a second, private copy of the algorithm while the offline test proved a
different one. Rebuilding from src/goblin_anchor_resolve.hpp every time keeps them the same
code. Pass --no-build to skip the compile.

  py tools\\exe_compat\\run_anchor_test.py
"""
import argparse
import subprocess
import sys
from pathlib import Path

PROJ = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJ / "tools"))
from known_exes import known_exes  # noqa: E402

SRC = Path(__file__).resolve().parent / "anchor_resolver_test.cpp"
OUT = PROJ / "builds" / "anchor_test"  # gitignored build output
EXE = OUT / "anchor_test.exe"
VSWHERE = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")


def find_vcvars():
    """vcvars64.bat of whichever VS2022 is installed - the same way build.bat finds it."""
    if VSWHERE.exists():
        r = subprocess.run([str(VSWHERE), "-latest", "-products", "*", "-requires",
                            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                            "-property", "installationPath"],
                           capture_output=True, text=True, errors="replace")
        root = r.stdout.strip().splitlines()
        if root:
            p = Path(root[0]) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if p.exists():
                return p
    return None


def build():
    vcvars = find_vcvars()
    if not vcvars:
        print("VS2022 x64 toolset not found - install it or pass --no-build")
        return False
    # Through a .bat: cmd.exe strips the outer quotes of a /c string, so a command that
    # both starts and ends with a quoted path gets mangled.
    OUT.mkdir(parents=True, exist_ok=True)
    bat = OUT / "_build_anchor_test.bat"
    bat.write_text(
        "@echo off\r\n"
        f'call "{vcvars}" >nul\r\n'
        f'cl /nologo /std:c++20 /EHsc /O2 /I "{PROJ / "src"}" "{SRC}" '
        f'/Fe:"{EXE}" /Fo:"{OUT / "anchor_test.obj"}"\r\n',
        encoding="utf-8")
    r = subprocess.run(["cmd.exe", "/c", str(bat)], capture_output=True, text=True,
                       errors="replace", cwd=str(PROJ))
    if r.returncode:
        print(r.stdout)
        print(r.stderr)
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--verbose", action="store_true", help="print every anchor, not summaries")
    a = ap.parse_args()
    if not a.no_build and not build():
        return 1
    exes = known_exes()
    if not exes:
        print("no exes configured (game_dir / exe_dir in tools/config.ini)")
        return 1
    r = subprocess.run([str(EXE)] + [str(e.path) for e in exes],
                       capture_output=True, text=True, errors="replace")
    label = {str(e.path): str(e) for e in exes}
    for line in r.stdout.splitlines():
        if line.startswith("=== "):
            print(f"\n=== {label.get(line[4:].strip(), line[4:].strip())}")
        elif a.verbose or line.strip().startswith(("----", "[DEAD", "[OK", "[SKIP")) or \
                not line.strip().startswith("["):
            print(line)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())

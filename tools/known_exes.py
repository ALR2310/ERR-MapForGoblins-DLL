"""The set of eldenring.exe builds we can prove the anchor rebase against.

One source of truth for every tool that needs a foreign exe. The build-target exe is
config.GAME_DIR/eldenring.exe; every other build lives one-per-subfolder under
config.EXE_DIR (`exe_dir` in tools/config.ini). The version label is read from the file's
own VERSIONINFO, never from the folder name - a folder called "4" tells nobody anything,
and a mislabelled exe would silently invalidate a green test run.

    from known_exes import known_exes, text_section
    for e in known_exes():
        print(e.version, e.path, e.is_target)
"""

import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import config  # noqa: E402


@dataclass(frozen=True)
class GameExe:
    version: str      # "2.6.2" - from VERSIONINFO, dropped to 3 components
    path: Path
    is_target: bool   # the exe the RVAs are baked against (game_dir)

    def __str__(self):
        return f"{self.version}{' (target)' if self.is_target else ''}"


def _version_of(path):
    """FileVersion out of the PE's VERSIONINFO, e.g. '2.6.2'. None when absent."""
    import pefile
    pe = pefile.PE(str(path), fast_load=True)
    pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    try:
        for fi in pe.FileInfo[0]:
            if fi.Key != b"StringFileInfo":
                continue
            for st in fi.StringTable:
                v = st.entries.get(b"FileVersion")
                if v:
                    parts = v.decode("utf-8", "replace").strip().split(".")
                    return ".".join(parts[:3]) if len(parts) >= 3 else ".".join(parts)
    except (AttributeError, IndexError):
        pass
    # No VERSIONINFO: fall back to the fixed VS_FIXEDFILEINFO block.
    try:
        ffi = pe.VS_FIXEDFILEINFO[0]
        return f"{ffi.FileVersionMS >> 16}.{ffi.FileVersionMS & 0xFFFF}.{ffi.FileVersionLS >> 16}"
    except (AttributeError, IndexError):
        return None


def known_exes(include_target=True):
    """Every eldenring.exe we hold, newest version first, target included by default."""
    found = {}
    if include_target and config.GAME_DIR:
        p = config.GAME_DIR / "eldenring.exe"
        if p.exists():
            found[p.resolve()] = True  # value = is_target
    if config.EXE_DIR and config.EXE_DIR.exists():
        for p in sorted(config.EXE_DIR.rglob("eldenring.exe")):
            found.setdefault(p.resolve(), False)
    out = []
    for p, is_target in found.items():
        v = _version_of(p)
        if v:
            out.append(GameExe(v, p, is_target))
    out.sort(key=lambda e: [int(x) for x in e.version.split(".")], reverse=True)
    return out


def text_section(path):
    """(bytes, virtual_address) of the FIRST .text - eldenring.exe carries several, and
    every anchored function lives in the lowest-VA one. Taking the last cost two debugging
    rounds once; it is centralised here so no tool repeats that."""
    import pefile
    pe = pefile.PE(str(path), fast_load=True)
    secs = sorted([s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text"],
                  key=lambda s: s.VirtualAddress)
    if not secs:
        raise RuntimeError(f"no .text section in {path}")
    t = secs[0]
    return pe.__data__[t.PointerToRawData:t.PointerToRawData + t.SizeOfRawData], t.VirtualAddress


if __name__ == "__main__":
    exes = known_exes()
    if not exes:
        print("No exes found. Set game_dir / exe_dir in tools/config.ini.")
        sys.exit(1)
    print(f"{'version':<10} {'target':<7} path")
    for e in exes:
        print(f"{e.version:<10} {'yes' if e.is_target else '':<7} {e.path}")
    print(f"\n{len(exes)} builds")

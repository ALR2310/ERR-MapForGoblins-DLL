"""Final pass over the extracted Rune/Ember piece positions (ERR only).

Produces data/rune_pieces_final.json and data/ember_pieces_final.json - the lists
generate_pieces actually bakes. When the optional local refinement hook
tools/local/refine_pieces.py exists (that directory is untracked and machine-specific),
it runs instead of this file's body and writes the *_final.json outputs itself; a stock
checkout has no hook, and the extracted positions pass through unchanged.
"""
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import config  # noqa: E402

hook = TOOLS / 'local' / 'refine_pieces.py'
if hook.exists():
    sys.exit(subprocess.call([sys.executable, str(hook)]))

for src, dst in (('rune_pieces.json', 'rune_pieces_final.json'),
                 ('ember_pieces.json', 'ember_pieces_final.json')):
    shutil.copyfile(config.DATA_DIR / src, config.DATA_DIR / dst)
print('  pieces pass through unchanged (no local refinement hook)')

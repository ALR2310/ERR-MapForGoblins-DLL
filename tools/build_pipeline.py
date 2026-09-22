#!/usr/bin/env python3
"""
Incremental build orchestrator with hash-based caching.

For each stage: hash inputs (file content for files, mtime+size aggregate
for directories), compare with cached signature. If match AND all outputs
exist → skip. Otherwise re-run script and update cache.

Cache: data/.build_cache.json
Override: --force <stage_name> | --force-all
"""
import re
import sys, os, json, hashlib, subprocess, time
from pathlib import Path


def _parse_profile(argv):
    """Resolve the build profile from argv/env BEFORE importing config
    (config reads MFG_PROFILE at import time)."""
    for i, a in enumerate(argv):
        if a == '--profile' and i + 1 < len(argv):
            return argv[i + 1].strip().lower()
        if a.startswith('--profile='):
            return a.split('=', 1)[1].strip().lower()
    return os.environ.get('MFG_PROFILE', 'err').strip().lower()


PROFILE = _parse_profile(sys.argv[1:])
if PROFILE not in ('err', 'vanilla', 'convergence2', 'convergence3', 'erte', 'goldenage', 'goldenage361', 'vins', 'reborn', 'graceborne', 'throne'):
    PROFILE = 'err'
os.environ['MFG_PROFILE'] = PROFILE  # propagate to every child subprocess
os.environ['PYTHONUTF8'] = '1'       # child stages read/write text as UTF-8 (non-ASCII game data,
                                     # e.g. the Golden Age Chinese overhaul, else trips cp1252 decode)

import config

REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / 'tools'
DATA = config.DATA_DIR                 # data/ (err) or data/<profile>/ otherwise
INPUTS = config.INPUTS_DIR                  # committed, never generated (see config.py)
PROFILE_INPUTS = config.PROFILE_INPUTS_DIR
DATA.mkdir(parents=True, exist_ok=True)
CACHE_FILE = DATA / '.build_cache.json'

# The convergence source is a STAGED dir (built by the prepare_merged_src
# stage below) - create it up front so require_err_mod_dir passes on the
# very first run; the stage then populates it before anything reads it.
if PROFILE in ('convergence2', 'convergence3', 'erte', 'goldenage', 'goldenage361', 'vins', 'reborn', 'graceborne', 'throne'):
    config.DATA_SRC_DIR.mkdir(parents=True, exist_ok=True)

ERR_MOD = config.require_err_mod_dir()  # profile-aware: mod overlay / vanilla game / merged dir
MSB_DIR = ERR_MOD / 'map' / 'MapStudio'
EVENT_DIR = ERR_MOD / 'event'
REGULATION = ERR_MOD / 'regulation.bin'
MSGBND = ERR_MOD / 'msg' / 'engus' / 'item_dlc02.msgbnd.dcx'

ROWS_OUT = DATA / 'rows_generated'
GENERATED_CPP = config.GENERATED_DIR   # src/generated or src/generated_vanilla

# Stages that only make sense for the ERR mod (their source assets/items do
# not exist in vanilla). Dropped from the vanilla pipeline.
ERR_ONLY_STAGES = {'generate_pieces', 'generate_kindling_spirits',
                   'extract_rune_positions', 'extract_itemlot_csv',
                   'finalize_pieces'}

MENU_MSGBND = ERR_MOD / 'msg' / 'engus' / 'menu_dlc02.msgbnd.dcx'


# ── Hashing ──
def hash_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk: break
            h.update(chunk)
    return h.hexdigest()


def hash_dir_meta(path, glob='*'):
    """Hash directory by (relpath, size, mtime_ns) tuples."""
    h = hashlib.sha256()
    files = sorted(path.rglob(glob)) if glob != '*' else sorted(path.rglob('*'))
    for f in files:
        if f.is_file():
            st = f.stat()
            rel = str(f.relative_to(path)).replace('\\', '/')
            h.update(f'{rel}\0{st.st_size}\0{st.st_mtime_ns}\0'.encode())
    return h.hexdigest()


def hash_input(p):
    p = Path(p)
    if not p.exists():
        return f'MISSING:{p}'
    if p.is_file():
        return f'F:{hash_file(p)}'
    if p.is_dir():
        return f'D:{hash_dir_meta(p)}'
    return f'?:{p}'


def stage_signature(inputs):
    h = hashlib.sha256()
    for p in inputs:
        h.update(str(p).encode() + b'\0' + hash_input(p).encode() + b'\0')
    return h.hexdigest()


# ── Stage definition ──
class Stage:
    def __init__(self, name, *, inputs, outputs, script, also_scripts=None, args=None):
        self.name = name
        self.inputs = [Path(p) for p in inputs]
        self.outputs = [Path(p) for p in outputs]
        self.script = TOOLS / script
        # Other Python files this script depends on (imports etc.)
        self.also_scripts = [TOOLS / p for p in (also_scripts or [])]
        self.args = list(args or [])

    def all_inputs(self):
        return self.inputs + self.also_scripts + [self.script]

    def signature(self):
        # Include args so signature changes if invocation changes
        sig = stage_signature(self.all_inputs())
        if self.args:
            h = hashlib.sha256()
            h.update(sig.encode())
            h.update(b'\0' + ' '.join(self.args).encode())
            return h.hexdigest()
        return sig

    def is_up_to_date(self, cache):
        if not all(o.exists() for o in self.outputs):
            return False
        return cache.get(self.name) == self.signature()

    def run(self):
        cmd = [sys.executable, str(self.script)] + self.args
        result = subprocess.run(cmd, cwd=str(TOOLS))
        return result.returncode == 0


# ── Pipeline ──
COMMON = ['config.py', 'marker_common.py', 'row_id_registry.py', 'icon_registry.py', 'map_categories.py',
          # rowsink.py renders every marker row for every generator, so a change to it changes all
          # of their output. Without it here a format change would leave every stage [CACHED] and
          # the tree would keep the old bytes while looking up to date.
          'rowsink.py']

STAGES = [
    # Source-table extractors: regenerate the category/codex/placename tables
    # from the active profile's game data (these used to be committed one-off
    # snapshots; now they stay fresh and exist for the vanilla profile too).
    Stage('extract_goods_categories',
          inputs=[REGULATION, config.PARAMDEF_DIR],
          outputs=[DATA / 'goods_sort_groups.json',
                   DATA / 'goods_crafting_ids.json',
                   DATA / 'goods_sorcery_ids.json',
                   DATA / 'goods_incantation_ids.json',
                   DATA / 'goods_spirit_ash_ids.json',
                   DATA / 'goods_steed_regalia_ids.json'],
          script='extract_goods_categories.py',
          also_scripts=['config.py']),

    Stage('extract_tutorial_codex',
          inputs=[MENU_MSGBND],
          outputs=[DATA / 'tutorial_title_ids.json',
                   DATA / 'tutorial_title_names.json',
                   DATA / 'enemy_tutorial_mapping.json'],
          script='extract_tutorial_codex.py',
          also_scripts=['config.py']),

    Stage('extract_placename_dump',
          inputs=[MSGBND],
          outputs=[DATA / 'PlaceName_engus.json'],
          script='extract_placename_dump.py',
          also_scripts=['config.py']),

    # English strings for every band a marker references (NpcName/ActionButtonText/
    # PlaceName), offset-encoded. Lowest-priority fallback so non-English clients get
    # English instead of blank text (which makes the engine draw no icon) when the
    # overhaul left that content untranslated. Consumed by generate_data.
    Stage('extract_english_fallback',
          inputs=[MSGBND, MENU_MSGBND],
          outputs=[DATA / 'english_fallback.json'],
          script='extract_english_fallback.py',
          also_scripts=['config.py']),

    # ERR-only: Rune/Ember Piece positions from ERR MSBs (AEG099_821/822).
    Stage('extract_rune_positions',
          inputs=[MSB_DIR],
          outputs=[DATA / 'rune_pieces.json',
                   DATA / 'ember_pieces.json'],
          script='extract_rune_positions.py',
          also_scripts=['config.py']),

    # ERR-only: the final pass over the extracted piece positions before the row
    # bake. Runs the optional local refinement hook (tools/local/, untracked and
    # machine-specific); a stock checkout has no hook and the positions pass through
    # unchanged. The hook's own inputs live outside the tree, so they are not listed:
    # their mtimes say nothing the stage could depend on.
    Stage('finalize_pieces',
          inputs=[DATA / 'rune_pieces.json',
                  DATA / 'ember_pieces.json',
                  MSB_DIR, EVENT_DIR],
          outputs=[DATA / 'rune_pieces_final.json',
                   DATA / 'ember_pieces_final.json'],
          script='finalize_pieces.py',
          also_scripts=['config.py']),

    # ERR-only: ItemLotParam_map CSV dump (consumed by generate_pieces).
    Stage('extract_itemlot_csv',
          inputs=[REGULATION, config.PARAMDEF_DIR],
          outputs=[DATA / 'ItemLotParam_map.csv'],
          script='extract_itemlot_csv.py',
          also_scripts=['config.py']),

    # BEFORE extract_items on purpose: the item database is enriched from this mapping, and
    # enrich cannot revert a stale upgrade. With the scan running after the extract, a change to
    # the scanner only reached the bake on the NEXT pipeline run - which silently left a phantom
    # marker in every profile built once (2026-07-30). The scan reads the event files, the
    # regulation and the entity index, never the database, so nothing wanted the old order.


    Stage('entity_index',
          inputs=[MSB_DIR],
          outputs=[DATA / 'msb_entity_index.json'],
          script='build_entity_index.py',
          also_scripts=['config.py']),

    Stage('emevd_scan',
          inputs=[EVENT_DIR, REGULATION, DATA / 'msb_entity_index.json'],
          outputs=[DATA / 'emevd_lot_mapping.json'],
          script='scan_emevd_awards.py',
          also_scripts=['config.py']),

    Stage('extract_items',
          # emevd_lot_mapping is NOT read by extract_all_items, but listed as an input
          # on purpose: enrich_fallback mutates items_database.json IN PLACE using the
          # mapping, and is not idempotent (it can't revert stale upgrades). When the
          # mapping changes, the DB must be re-extracted from scratch so enrich applies
          # the new mapping to a virgin DB (converges one pipeline run after a scan
          # change; same-run ordering puts extract before emevd_scan).
          inputs=[REGULATION, MSB_DIR, MSGBND, config.PARAMDEF_DIR,
                  DATA / 'emevd_lot_mapping.json'],
          outputs=[DATA / 'items_database.json',
                   DATA / 'npc_name_ids.json',
                   DATA / 'unreachable_msb_lots.json',
                   DATA / 'boss_flag_drops.json'],
          script='extract_all_items.py',
          also_scripts=['unreachable.py'] + COMMON),


    Stage('grace_index',
          inputs=[REGULATION, MSGBND],
          outputs=[DATA / 'grace_position_index.json'],
          script='build_grace_index.py',
          also_scripts=['config.py']),


    Stage('enrich_fallback',
          inputs=[DATA / 'items_database.json',
                  DATA / 'emevd_lot_mapping.json',
                  DATA / 'unreachable_msb_lots.json'],
          outputs=[DATA / 'items_database.json'],  # modified in-place
          script='enrich_fallback_with_emevd.py',
          also_scripts=['config.py']),

    Stage('generate_boss_list',
          inputs=[REGULATION, MSB_DIR, MSGBND, EVENT_DIR],
          outputs=[DATA / 'boss_list.json'],
          script='generate_boss_list.py',
          also_scripts=['extract_all_items.py'] + COMMON),

    # Relocating-boss flee-spawns (Lansseax): DETECTION ONLY, and it has to run before the
    # generators because they read its result and never create the wrong rows. It used to sit after
    # every generator and rewrite their output in place, which is the single reason the pipeline
    # needed an editable text intermediate at all.
    Stage('relocating_boss_fix',
          inputs=[MSB_DIR, EVENT_DIR],
          outputs=[DATA / '_relocating_boss_fix.done',
                   config.PROJECT_DIR / 'data' / 'relocating_flee_spawns.json'],
          script='generate_relocating_boss_fix.py',
          also_scripts=['config.py']),

    Stage('generate_loot',
          inputs=[INPUTS / 'enemy_bloodmsg_mapping.json',
                  INPUTS / 'enemy_names_i18n.json',
                  INPUTS / 'enemy_model_aliases.json',
                  DATA / 'items_database.json',
                  DATA / 'goods_sort_groups.json',
                  DATA / 'goods_crafting_ids.json',
                  DATA / 'goods_sorcery_ids.json',
                  DATA / 'goods_incantation_ids.json',
                  DATA / 'goods_spirit_ash_ids.json',
                  DATA / 'goods_steed_regalia_ids.json',
                  DATA / 'boss_list.json',
                  DATA / 'boss_flag_drops.json',
                  DATA / 'enemy_tutorial_mapping.json',
                  DATA / 'tutorial_title_ids.json',
                  DATA / 'tutorial_title_names.json',
                  DATA / 'grace_position_index.json'],
          outputs=[ROWS_OUT / 'Loot - Consumables.rows',
                   ROWS_OUT / 'Equipment - Armaments.rows',
                   ROWS_OUT / 'Quest - Progression.rows',
                   ROWS_OUT / 'World - Bosses.rows',
                   DATA / 'loot_lot_linkage.json',
                   DATA / 'item_icon_table.json',
                   DATA / 'english_fallback.json',                       # npcname_known: NpcName ids this profile resolves
                   REPO / 'data' / 'npc_name_text_map.json'],
          script='generate_loot.py',
          # relocating_spawns.py decides which rows are NOT created (the flee-spawn duplicates), so
          # a change to that rule has to invalidate this stage or the old rows survive as cached.
          also_scripts=['marker_common.py', 'row_id_registry.py', 'icon_registry.py', 'map_categories.py',
                        'npcname_known.py', 'relocating_spawns.py']),

    Stage('generate_pieces',
          inputs=[DATA / 'ItemLotParam_map.csv',
                  DATA / 'grace_position_index.json',
                  DATA / 'rune_pieces_final.json',
                  DATA / 'ember_pieces_final.json'],
          outputs=[ROWS_OUT / 'Reforged - Rune Pieces.rows',
                   ROWS_OUT / 'Reforged - Ember Pieces.rows',
                   ROWS_OUT / 'Reforged - Rune Pieces_slots.json',
                   ROWS_OUT / 'Reforged - Ember Pieces_slots.json'],
          script='generate_pieces.py',
          also_scripts=['marker_common.py', 'row_id_registry.py', 'icon_registry.py',
                        'map_categories.py', 'relocating_spawns.py']),

    Stage('scan_gathering_nodes',
          inputs=[MSB_DIR, DATA / 'aeg099_item_mapping.json'],
          outputs=[DATA / 'all_gathering_nodes_final.json'],
          script='scan_all_gathering_nodes.py',
          also_scripts=['config.py']),

    Stage('scan_gathering_node_flags',
          inputs=[EVENT_DIR],
          outputs=[DATA / 'gathering_node_flags.json'],
          script='scan_gathering_node_flags.py',
          also_scripts=['config.py']),

    Stage('generate_material_nodes',
          inputs=[DATA / 'aeg099_item_mapping.json',
                  DATA / 'aeg463_item_mapping.json',
                  DATA / 'all_gathering_nodes_final.json',
                  DATA / 'gathering_node_flags.json'],
          outputs=[ROWS_OUT / 'Loot - Material Nodes.rows',
                   ROWS_OUT / 'Loot - Material Nodes_slots.json'],
          script='generate_material_nodes.py',
          also_scripts=['marker_common.py', 'row_id_registry.py', 'icon_registry.py', 'map_categories.py', 'unreachable.py']),

    Stage('generate_graces',
          inputs=[REGULATION],
          outputs=[ROWS_OUT / 'World - Graces.rows'],
          script='generate_graces.py',
          also_scripts=['extract_all_items.py', 'unreachable.py'] + COMMON),

    Stage('generate_summoning_pools',
          inputs=[REGULATION, MSB_DIR],
          outputs=[ROWS_OUT / 'World - Summoning Pools.rows'],
          script='generate_summoning_pools.py',
          also_scripts=['extract_all_items.py'] + COMMON),

    Stage('generate_kindling_spirits',
          inputs=[PROFILE_INPUTS / 'kindling_spirits.json'],
          outputs=[ROWS_OUT / 'World - Kindling Spirits.rows',
                   ROWS_OUT / 'World - Kindling Spirits_slots.json'],
          script='generate_kindling_spirits.py',
          also_scripts=['marker_common.py', 'row_id_registry.py', 'icon_registry.py', 'map_categories.py']),

    Stage('generate_spirit_springs',
          inputs=[MSB_DIR],
          outputs=[ROWS_OUT / 'World - Spirit Springs.rows',
                   ROWS_OUT / 'World - Spiritspring Hawks.rows'],
          script='generate_spirit_springs.py',
          also_scripts=['unreachable.py'] + COMMON),

    Stage('generate_imp_statues',
          inputs=[MSB_DIR],
          outputs=[ROWS_OUT / 'World - Imp Statues.rows'],
          script='generate_imp_statues.py',
          also_scripts=['unreachable.py'] + COMMON),

    Stage('generate_stakes',
          inputs=[MSB_DIR],
          outputs=[ROWS_OUT / 'World - Stakes of Marika.rows'],
          script='generate_stakes.py',
          also_scripts=COMMON),

    Stage('extract_seal_puzzles',
          inputs=[MSB_DIR, EVENT_DIR],
          outputs=[DATA / 'seal_puzzles.json'],
          script='extract_seal_puzzles.py',
          also_scripts=['config.py']),

    Stage('generate_seal_puzzles',
          inputs=[DATA / 'seal_puzzles.json'],
          outputs=[ROWS_OUT / 'World - Seal Puzzles.rows'],
          script='generate_seal_puzzles.py',
          also_scripts=['marker_common.py', 'row_id_registry.py', 'icon_registry.py', 'map_categories.py']),

    Stage('generate_hero_tomb_statues',
          inputs=[MSB_DIR, EVENT_DIR, DATA / 'WorldMapPointParam.json'],
          outputs=[ROWS_OUT / "World - Hero's Tomb Statues.rows"],
          script='generate_hero_tomb_statues.py',
          also_scripts=['marker_common.py', 'row_id_registry.py', 'icon_registry.py', 'map_categories.py']),

    Stage('generate_paintings',
          inputs=[MSB_DIR, EVENT_DIR],
          outputs=[ROWS_OUT / 'World - Paintings.rows'],
          script='generate_paintings.py',
          also_scripts=COMMON),

    Stage('generate_maps',
          inputs=[DATA / 'items_database.json'],
          outputs=[ROWS_OUT / 'World - Maps.rows'],
          script='generate_maps.py',
          also_scripts=['marker_common.py', 'row_id_registry.py', 'icon_registry.py', 'map_categories.py']),

    Stage('generate_gestures',
          inputs=[DATA / 'msb_entity_index.json', EVENT_DIR, REGULATION],
          outputs=[ROWS_OUT / 'Loot - Gestures.rows'],
          script='generate_gestures.py',
          also_scripts=['marker_common.py', 'row_id_registry.py', 'icon_registry.py', 'map_categories.py', 'config.py']),

    Stage('generate_hostile_npcs',
          inputs=[REGULATION, MSB_DIR, config.PARAMDEF_DIR, EVENT_DIR,
                  DATA / 'items_database.json',
                  DATA / 'english_fallback.json',                       # npcname_known: NpcName ids this profile resolves
                  REPO / 'data' / 'npc_name_text_map.json',
                  INPUTS / 'quest_invader_overrides.json'],
          outputs=[ROWS_OUT / 'World - Hostile NPC.rows'],
          script='generate_hostile_npcs.py',
          also_scripts=['marker_common.py', 'row_id_registry.py', 'icon_registry.py', 'map_categories.py', 'config.py', 'npcname_known.py']),

    # Enemies wired through the common "strong enemy" templates 90005300/301: they stay dead once
    # killed, so each gets a marker that hides (or checkmarks) on its kill flag.
    Stage('generate_strong_enemies',
          inputs=[REGULATION, MSB_DIR, config.PARAMDEF_DIR, EVENT_DIR,
                  INPUTS / 'enemy_names_i18n.json',
                  INPUTS / 'enemy_model_aliases.json',
                  INPUTS / 'enemy_bloodmsg_mapping.json'],
          outputs=[ROWS_OUT / 'World - Strong Enemies.rows'],
          script='generate_strong_enemies.py',
          also_scripts=['generate_hostile_npcs.py', 'generate_loot.py', 'marker_common.py',
                        'row_id_registry.py', 'icon_registry.py', 'map_categories.py', 'config.py']),

    Stage('generate_data',
          inputs=[ROWS_OUT, DATA / 'loot_lot_linkage.json',
                  DATA / 'item_icon_table.json',
                  DATA / 'english_fallback.json',
                  INPUTS / 'unprojectable_tiles.json',          # tiles dropped before the bake
                  INPUTS / 'enemy_names_i18n.json',
                  # the hand-maintained headers the generated .cpp implement (the Category
                  # enum: generate_data's CATEGORY_MAP must name its members)
                  *[config.PROJECT_DIR / 'src' / h
                    for h in ('goblin_map_data.hpp', 'goblin_item_icons.hpp',
                              'goblin_enemy_names.hpp', 'goblin_item_fallback.hpp')]],
          outputs=[GENERATED_CPP / 'goblin_map_blob_data.cpp',
                   GENERATED_CPP / 'goblin_item_icons.cpp',
                   GENERATED_CPP / 'goblin_enemy_names.cpp',
                   GENERATED_CPP / 'goblin_item_fallback.cpp'],
          script='generate_data.py',
          # mapblob.py IS the storage layout; rowsink.py is the field order it packs.
          also_scripts=['icon_registry.py', 'map_categories.py',  # ANON_ICON_ID = iconid("anon")
                        'mapblob.py', 'rowsink.py'],
          args=['--rows-dir', str(ROWS_OUT)]),

    # Tile -> game-zone (PlaceName id) map for the Progress tab, from tile_region_map.json
    # (only err ships one today; other profiles emit an empty map and fall back to the
    # fragment grouping). Cheap; reads inputs/<profile>/tile_region_map.json if present.
    Stage('generate_region_map',
          inputs=[PROFILE_INPUTS / 'tile_region_map.json'],
          outputs=[GENERATED_CPP / 'goblin_region_map.cpp',
                   GENERATED_CPP / 'goblin_region_map.hpp'],
          script='generate_region_map.py',
          also_scripts=['config.py']),

    # Per-row ACTUAL gather-asset models (ERR substitutes some assets with DLC-era models
    # in the MSB: part NAME stays vanilla, ModelName differs; GEOF save entries carry the
    # actual model's hash) - used by collected-tracking. Runs AFTER generate_data.
    Stage('generate_geof_models',
          inputs=[GENERATED_CPP / 'goblin_map_blob_data.cpp',
                  DATA / 'all_gathering_nodes_final.json'],
          outputs=[GENERATED_CPP / 'goblin_geof_models.cpp',
                   GENERATED_CPP / 'goblin_geof_models.hpp'],
          script='generate_geof_models.py',
          # it decodes the packed table, so the layout is one of its inputs
          also_scripts=['config.py', 'mapblob.py', 'rowsink.py']),

    # Alternative (hybrid) loot-location naming, baked as generated::LOCATION_ALT
    # (row_id -> textId2). Shown via INI [Goblin] show_location_compare = true.
    # Must run AFTER generate_data (reads the baked table back).
    Stage('generate_location_overrides',
          inputs=[GENERATED_CPP / 'goblin_map_blob_data.cpp', MSB_DIR,
                  DATA / 'WorldMapPointParam.json', DATA / 'grace_position_index.json',
                  DATA / 'PlaceName_engus.json'],
          outputs=[GENERATED_CPP / 'goblin_location_alt.cpp',
                   GENERATED_CPP / 'goblin_location_alt.hpp'],
          script='generate_location_overrides.py',
          also_scripts=['marker_common.py', 'row_id_registry.py', 'icon_registry.py',
                        'map_categories.py', 'config.py', 'mapblob.py', 'rowsink.py']),
]


# Convergence-only: stage the merged overlay-over-vanilla source dir FIRST
# (everything else reads from it). Defined lazily - the per-profile *_MOD_DIR
# is None in the other profiles.
def _overlay_prepare_stage():
    """Merged-source staging for an overlay-mod profile (convergence / erte /
    goldenage): the mod's partial file overlay laid over the vanilla game."""
    overlay = getattr(config, PROFILE.upper() + '_MOD_DIR', None)  # *_MOD_DIR per profile
    if not overlay or not overlay.exists():
        print(f'ERROR: {PROFILE} profile needs {PROFILE}_mod_dir in tools/config.ini')
        sys.exit(1)
    game = config.require_game_dir()
    return Stage('prepare_merged_src',
                 inputs=[overlay / 'regulation.bin',
                         overlay / 'msg' / 'engus',
                         overlay / 'map' / 'MapStudio',
                         overlay / 'event',
                         game / 'map' / 'mapstudio',
                         game / 'event',
                         game / 'msg' / 'engus'],
                 outputs=[ERR_MOD / 'regulation.bin',
                          ERR_MOD / '.merge_manifest.json'],
                 script='prepare_merged_src.py',
                 also_scripts=['config.py'])


# Non-ERR bootstrap stages: regenerate, from the active profile's game data,
# the committed inputs that ship pre-extracted for ERR (they don't exist under
# data/vanilla/ or data/convergence/). Run before the stages that consume
# them. For the err profile these are NOT added (the committed copies are
# authoritative).
VANILLA_BOOTSTRAP = [
    Stage('extract_param_bootstrap',
          inputs=[REGULATION, MSGBND],
          outputs=[DATA / 'WorldMapLegacyConvParam.json',
                   DATA / 'valid_location_ids.json'],
          script='extract_param_bootstrap.py',
          also_scripts=['config.py']),

    Stage('extract_world_map_param',
          inputs=[REGULATION],
          outputs=[DATA / 'WorldMapPointParam.json',
                   DATA / 'WorldMapPointParam.csv'],
          script='extract_world_map_param.py',
          also_scripts=['config.py']),

    Stage('extract_aeg099_mapping',
          inputs=[REGULATION, MSGBND],
          outputs=[DATA / 'aeg099_item_mapping.json'],
          script='extract_aeg099_mapping.py',
          also_scripts=['config.py']),

    Stage('extract_aeg463_mapping',
          inputs=[REGULATION, MSGBND],
          outputs=[DATA / 'aeg463_item_mapping.json'],
          script='extract_aeg463_mapping.py',
          also_scripts=['config.py']),
]


def active_stages():
    """The stage list for the selected profile.

    err:         the full STAGES list, unchanged (committed inputs are used as-is).
    vanilla:     bootstrap extractors first, then STAGES minus the ERR-only ones.
    convergence: merged-source staging, then the same as vanilla.
    """
    if PROFILE == 'vanilla':
        return VANILLA_BOOTSTRAP + [s for s in STAGES if s.name not in ERR_ONLY_STAGES]
    if PROFILE in ('convergence2', 'convergence3', 'erte', 'goldenage', 'goldenage361', 'vins', 'reborn', 'graceborne', 'throne'):
        return ([_overlay_prepare_stage()] + VANILLA_BOOTSTRAP
                + [s for s in STAGES if s.name not in ERR_ONLY_STAGES])
    return STAGES


def check_source_completeness():
    """Are the DLC maps actually there? A partial UXM unpack loses them SILENTLY.

    The Realm of Shadow lives in map areas m20..m28 and m61. Nothing downstream notices their
    absence: the extractors simply find fewer files, every stage succeeds, and the bake comes
    out ~1260 markers lighter with no warning anywhere. That is the same shape of failure this
    project keeps getting bitten by - a missing input that produces a smaller, confident answer
    instead of an error.

    A warning, not a hard stop: a deliberate no-DLC build is somebody's business, and the point
    is that it can no longer happen by accident.
    """
    if not MSB_DIR.is_dir():
        return
    dlc = [p for p in MSB_DIR.iterdir()
           if re.match(r'm(2[0-8]|61)_', p.name)]
    base = [p for p in MSB_DIR.iterdir()
            if re.match(r'm(1[0-9]|3[0-9]|60)_', p.name)]
    if base and not dlc:
        print('=' * 78)
        print('WARNING: no Realm of Shadow maps (m20..m28, m61) in')
        print(f'  {MSB_DIR}')
        print('The bake will come out roughly 1260 markers short and nothing else will say so.')
        print('A UXM unpack must include the files from DLC.bdt as well as the base archives.')
        print('=' * 78)
    elif dlc:
        print(f'source check: {len(base)} base + {len(dlc)} Realm of Shadow map file(s)')

def main():
    args = sys.argv[1:]
    force_all = '--force-all' in args
    check_source_completeness()
    force_stages = set()
    for i, a in enumerate(args):
        if a == '--force' and i + 1 < len(args):
            force_stages.add(args[i + 1])

    print(f'[PROFILE] {PROFILE}  (data={DATA}, generated={GENERATED_CPP})')

    cache = {}
    if CACHE_FILE.exists():
        try:
            cache = json.load(open(CACHE_FILE, encoding='utf-8'))
        except Exception:
            cache = {}

    overall_t0 = time.time()
    for stage in active_stages():
        t0 = time.time()
        forced = force_all or stage.name in force_stages
        if not forced and stage.is_up_to_date(cache):
            print(f'[CACHED]  {stage.name}')
            continue
        print(f'[BUILD]   {stage.name}  ({stage.script.name})')
        if not stage.run():
            print(f'[FAILED]  {stage.name}')
            return 1
        cache[stage.name] = stage.signature()
        with open(CACHE_FILE, 'w', encoding='utf-8') as f:
            json.dump(cache, f, indent=1)
        print(f'[OK]      {stage.name}  ({time.time()-t0:.1f}s)')

    print(f'\n[DONE]    pipeline in {time.time()-overall_t0:.1f}s')
    return 0


if __name__ == '__main__':
    sys.exit(main())

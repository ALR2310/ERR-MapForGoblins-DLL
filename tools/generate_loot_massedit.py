#!/usr/bin/env python3
"""
Generate Loot MASSEDIT files from items_database.json.
Fully automatic - no dependency on existing MASSEDIT files.
Uses goodsId as textId1 for localized names via GoodsName FMG.

Output: data/massedit_generated/Loot - <category>.MASSEDIT
"""

import json
import re
from pathlib import Path
from collections import defaultdict, Counter

import config
from massedit_common import (DATA_DIR, OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS,
                             OVERWORLD_AREAS, VALID_LOCATION_IDS, resolve_location_id,
                             resolve_location_id_at, get_disp_mask, is_dlc_plane)
from switched_chests import build_switch_gate_map
DB_PATH = DATA_DIR / 'items_database.json'

# {(map, partName): (flag, show_when_on)} for same-position switched-chest loot pairs
# (e.g. Patches' m31_00 Cloth chest vs Glass Shard chest on flag 3691). Populated in
# main() and applied per-marker in write_massedit as group-2 gate flags so a switched
# chest's marker only shows in the world-state where that chest is actually present.
SWITCH_GATE = {}

# Goods sortGroupId lookup (goodsType=0 items only) for consumable filtering
_sort_groups_path = DATA_DIR / 'goods_sort_groups.json'
GOODS_SORT_GROUPS = {}
if _sort_groups_path.exists():
    with open(_sort_groups_path) as _f:
        GOODS_SORT_GROUPS = {int(k): v for k, v in json.load(_f).items()}

# sortGroupId values for combat consumables:
#   20 = healing/buffs (boluses, cured meats, dried livers, flesh)
#   50 = throwing items/tools (darts, daggers, stones, chakrams)
#   70 = greases
#   80 = utility (rainbow stone, glowstone, soap, soft cotton)
CONSUMABLE_SORT_GROUPS = {20, 50, 70, 80}

# Torrent's caparisons, by their own inventory sort group (extract_goods_categories). They are key
# items, so goods_sort_groups.json - goodsType 0 only - does not carry them.
_steed_path = DATA_DIR / 'goods_steed_regalia_ids.json'
STEED_REGALIA_IDS = set()
if _steed_path.exists():
    with open(_steed_path) as _f:
        STEED_REGALIA_IDS = {int(i) for i in json.load(_f)}

# Prattling Pate IDs - excluded from Consumables/Utilities to avoid
# double-markers (they have their own dedicated category).
PATE_IDS = {2200, 2201, 2202, 2203, 2204, 2205, 2206, 2207, 2002150}


def is_consumable_goods(item_id):
    """Check if a goods item is a combat consumable by sortGroupId."""
    return GOODS_SORT_GROUPS.get(item_id, -1) in CONSUMABLE_SORT_GROUPS


# Crafting material IDs (goodsType=2)
_crafting_path = DATA_DIR / 'goods_crafting_ids.json'
CRAFTING_IDS = set()
if _crafting_path.exists():
    with open(_crafting_path) as _f:
        CRAFTING_IDS = set(json.load(_f))

# Spirit Ash IDs (goodsType=8)
_spirit_path = DATA_DIR / 'goods_spirit_ash_ids.json'
SPIRIT_ASH_IDS = set()
if _spirit_path.exists():
    with open(_spirit_path) as _f:
        SPIRIT_ASH_IDS = set(json.load(_f))

# Sorcery IDs (goodsType=17)
_sorc_path = DATA_DIR / 'goods_sorcery_ids.json'
SORCERY_IDS = set()
if _sorc_path.exists():
    with open(_sorc_path) as _f:
        SORCERY_IDS = set(json.load(_f))

# Incantation IDs (goodsType=18)
_incan_path = DATA_DIR / 'goods_incantation_ids.json'
INCANTATION_IDS = set()
if _incan_path.exists():
    with open(_incan_path) as _f:
        INCANTATION_IDS = set(json.load(_f))

# Key-item IDs (goodsType 1 = key item, 3 = remembrance, 12 = info/note/message)
# - the "Quest - Progression" catch-all set, derived per-profile by
# extract_goods_categories. The catch-all is the LAST loot category, so any
# more-specific category (cookbooks, stonesword keys, bell bearings, whetblades,
# crystal tears, prayerbooks, mp-fingers...) claims its items first
# (first-match-wins in the main loop) and Progression mops up the remaining key
# items + notes. Map fragments are excluded by name (World - Maps marks them).
_keyitem_path = DATA_DIR / 'goods_keyitem_ids.json'
KEYITEM_IDS = set()
if _keyitem_path.exists():
    with open(_keyitem_path) as _f:
        KEYITEM_IDS = set(json.load(_f))

# Crystal Tear IDs (goodsType 10) - param-driven so the category works regardless
# of item-name language (the old English-name match missed e.g. Golden Age's
# Chinese-named tears). Wondrous Physick flask (goodsType 9) is added explicitly.
_tear_path = DATA_DIR / 'goods_crystal_tear_ids.json'
CRYSTAL_TEAR_IDS = set()
if _tear_path.exists():
    with open(_tear_path) as _f:
        CRYSTAL_TEAR_IDS = set(json.load(_f))

# Ammo IDs - EquipParamWeapon rows with wepType 81/83/85/86 (Arrow/Great Arrow/Bolt/
# Ballista Bolt), derived per-profile. Splits Ammo from Armaments by weapon class
# rather than an id range, so overhaul/SOTE weapons at high ids stay weapons.
_ammo_path = DATA_DIR / 'weapon_ammo_ids.json'
AMMO_IDS = set()
if _ammo_path.exists():
    with open(_ammo_path) as _f:
        AMMO_IDS = set(json.load(_f))

# Quest items the goodsType signal can't reach: these share goodsType 0 with
# ordinary consumables (so they are not in KEYITEM_IDS) but are one-off quest
# items that belong in Progression. 8867 (Three Fingers' Scrawls) exists only on
# some profiles - harmless where the id is absent. 2170 = Potent Dreambrew (ERR
# quest item that opens an alternate DLC entrance). 1000000 = Codex of the
# All-Knowing (ERR unique static item; goods id collides with the Dagger weapon id
# but category==1 disambiguates). 2920 = Rune of Grace (Reborn unique item; sits in
# the golden-rune sortGroup 100 but is a one-off, not a stackable rune).
PROGRESSION_EXTRA = {2002120, 2002130, 2190, 8867, 2170, 1000000, 2920}

# True if a lot contains a "<X> Merchant Bell Bearing" - the bell bearings tied to
# killable merchants. Name-based, because some of these come from a treasure/map
# lot rather than the enemy drop (e.g. Convergence's Nomadic Merchant's Bell
# Bearing [8]) - source alone misroutes those into the plain Bell-Bearings
# category, so disabling Merchant Bell-Bearings wouldn't hide them.
def _is_merchant_bell(rec):
    return any('bell bearing' in i.get('name', '').lower() and
               'merchant' in i.get('name', '').lower()
               for i in rec.get('items', []))


# Rune Arc goods id differs by profile: ERR uses 150, vanilla/Convergence/ERTE use
# 190 (in vanilla id 150 is Furlcalling Finger Remedy). Hardcoding 150 mislabeled
# Furlcalling as Rune Arc and missed the real Rune Arcs on the non-ERR profiles.
RUNE_ARC_ID = 150 if config.PROFILE == 'err' else 190


# Which item names go into which file, with iconId and start row ID
# Filter by item goodsId or item name
# Equipment categories use ItemLotParam category (2=weapon, 3=armour, 4=accessory, 5=gem)
# ERR-only loot categories: their item IDs don't exist in vanilla, so they
# would only ever produce empty files there. Skip them in the vanilla profile.
ERR_ONLY_CATS = {'Reforged - Items', 'Reforged - Fortunes', 'Reforged - Sealed Curios'}

# Talisman Pouch: a key item by goodsType, a talisman thing by meaning (it is the slot itself).
TALISMAN_POUCH_IDS = {10040}

LOOT_CATEGORIES = {
    'Key - Celestial Dew': {
        'filter': lambda items: any(i['id'] == 2130 and i['category'] == 1 for i in items),
        'iconId': 419,
        'startId': 3050000,
    },
    'Key - Cookbooks': {
        'filter': lambda items: any(
            i['category'] == 1 and 'cookbook' in i.get('name', '').lower()
            for i in items
        ),
        'iconId': 424,
        'startId': 3100000,
    },
    'Key - Crystal Tears': {
        # Crystal Tears, Cracked Tears, Hardtears, Bubbletears, Hidden Tears. Plus the
        # Convergence "Physick Remnant" key_items, which are crafted into Flask of Wondrous
        # Physick crystal tears (the crystal-tear source on that overhaul, analogous to how
        # Talisman Remnants are the talisman source) - mutually exclusive with the talisman
        # remnant match (that one excludes 'physick').
        'filter': lambda items: any(
            i['category'] == 1 and (i['id'] in CRYSTAL_TEAR_IDS   # goodsType 10 (language-independent)
                or i['id'] in (250, 251, 2011010)  # Flask of Wondrous Physick (goodsType 9), Crimsonburst Dried Tear
                or (i.get('broad_category') == 'key_item'
                    and 'remnant' in i.get('name', '').lower()
                    and 'physick' in i.get('name', '').lower()))
            for i in items
        ),
        'iconId': 392,
        'startId': 3200000,
    },
    'Key - Imbued Sword Keys': {
        'filter': lambda items: any(i['id'] == 8186 and i['category'] == 1 for i in items),
        'iconId': 432,
        'startId': 3300000,
    },
    'Key - Larval Tears': {
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in (8185, 2008033)
            for i in items
        ),
        'iconId': 418,
        'startId': 3400000,
    },
    'Key - Lost Ashes': {
        'filter': lambda items: any(i['id'] == 10070 and i['category'] == 1 for i in items),
        'iconId': 396,
        'startId': 3500000,
    },
    'Key - Pots n Perfumes': {
        # Reusable crafting vessels by id (Ritual Pot 2009500, Perfume Bottle
        # 9500/9501/9510) PLUS the throwable pots (sortGroupId 30: Fire/Rancor/Rot/
        # Poison Pot...) and aromatics/perfumes (sortGroupId 40: Bloodboil/Uplifting
        # Aromatic, Spraymist). 30/40 are standard goodsType-0 sort groups our filters
        # never covered - in vanilla these items are crafted-only (not world-placed)
        # so it never showed; overhauls (Convergence) place them as treasure, exposing
        # the gap. sortGroup-based so it stays correct per-profile.
        'filter': lambda items: any(
            i['category'] == 1 and (i['id'] in (9500, 9501, 9510, 2009500)
                or GOODS_SORT_GROUPS.get(i['id'], -1) in (30, 40))
            for i in items
        ),
        'iconId': 425,
        'startId': 3600000,
    },
    'Key - Seeds Tears Ashes': {
        # Golden Seeds, Sacred Tears (Revered Spirit Ash has its own category below)
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in (10010, 10020)
            for i in items
        ),
        'iconId': 377,
        'startId': 3700000,
    },
    'Key - Scadutree Fragments': {
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] == 2010000
            for i in items
        ),
        'iconId': 401,
        'startId': 3750000,
    },
    'Key - Revered Spirit Ashes': {
        # the DLC spirit-ash blessing upgrade - the Scadutree Fragment's counterpart
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] == 2010100
            for i in items
        ),
        'iconId': 0,       # assigned by icon_registry below
        'startId': 0,      # assigned by row_id_registry below
    },
    'Key - Spectral Steed Regalia': {
        # Torrent's caparisons (Tarnished Pack, game patch 1.17) - cosmetic, and they used to land
        # in Quest - Progression. No id list here: EquipParamGoods files them under an inventory
        # sort group of their own (verified: exactly the three regalia), and the extractor dumps it.
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in STEED_REGALIA_IDS
            for i in items
        ),
        'iconId': 0,       # assigned by icon_registry below
        'startId': 0,      # assigned by row_id_registry below
    },
    'Key - Whetblades': {
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in (8970, 8971, 8972, 8973, 8974)
            for i in items
        ),
        'iconId': 431,
        'startId': 3800000,
    },
    'Quest - Deathroot': {
        'filter': lambda items: any(i['id'] == 2090 and i['category'] == 1 for i in items),
        'iconId': 428,
        'startId': 2300000,
    },
    'Quest - Seedbed Curses': {
        'filter': lambda items: any(i['id'] == 8193 and i['category'] == 1 for i in items),
        'iconId': 430,
        'startId': 2100000,
    },
    # (Rada Fruit folded into Loot - Crafting Materials: it's a vanilla DLC crafting
    # ingredient, goodsType==2, so it matches the crafting filter like every other.)
    'Reforged - Items': {
        # ERR-added items: Oracle Effigy/Remedy, Starlight Tokens
        'filter': lambda items: any(i['id'] in (
            900000,    # Oracle Effigy
            900010,    # Oracle's Remedy
            22000,     # Starlight Token
        ) and i['category'] == 1 for i in items),
        'iconId': 421,
        'startId': 3900000,
    },
    'Reforged - Fortunes': {
        # 12 Fortune types (ERR-specific stat/resistance trinkets)
        'filter': lambda items: any(i['id'] in (
            900218,  # Fortune of the Houses
            900238,  # Fortune of the Godslayers
            900258,  # Fortune of Haima
            900268,  # Fortune of the Crucible
            900278,  # Fortune of the Dynasts
            900288,  # Fortune of the Warmaster
            900308,  # Fortune of the Bold
            900318,  # Fortune of the Wise
            900328,  # Fortune of the Cunning
            900338,  # Fortune of the Bulwark
            900348,  # Fortune of the Reeds
            900368,  # Fortune of the Brave
        ) and i['category'] == 1 for i in items),
        'iconId': 422,
        'startId': 3920000,
    },
    'Reforged - Sealed Curios': {
        # ERR-added Sealed Curio family (9 types)
        'filter': lambda items: any(i['id'] in (
            1301900,   # Sealed Knifeprint Curio
            1302900,   # Sealed Poacher's Curio
            1303900,   # Physician's Sealed Curio
            1304900,   # Sealed Scadutear Curio
            1305900,   # Sealed Fanatic's Curio
            1306900,   # Sealed Gate Curio
            1307900,   # Sealed Curio of Ranah
            1308900,   # Sealed Academy Curio
            1309900,   # Sealed Dragonscale Curio
        ) and i['category'] == 1 for i in items),
        'iconId': 423,
        'startId': 3950000,
    },
    'Equipment - Armaments': {
        # Weapons (cat=2) that are not ammo. Ammo is identified by weapon class
        # (AMMO_IDS = wepType 81/83/85/86) instead of an id range - the old
        # "id >= 50M = ammo" rule wrongly swept the SOTE/overhaul weapon block
        # (Smithscript Dagger/Cirque/Rosary, Backhand Blade, Great Katana... wepType
        # 88-95, ids >= 60M) into Ammo and hid them from the map.
        'filter': lambda items: any(
            i['category'] == 2 and i['id'] not in AMMO_IDS for i in items),
        'iconId': 380,
        'startId': 4000000,
    },
    'Equipment - Armour': {
        'filter': lambda items: any(i['category'] == 3 for i in items),
        'iconId': 381,
        'startId': 4100000,
    },
    'Equipment - Talismans': {
        # cat=4 = real talismans. The Convergence overhauls no longer scatter talismans;
        # they are crafted at a site of grace from "Remnants" looted from the world/dungeons/
        # bosses (Combat/Enchanted/Heirloom/Warding/Relic AND the Physick remnants - ALL of
        # them craft talismans, e.g. "Combat Remnants are tools used for crafting Talismans
        # which raise combat prowess"). Those remnants are goods (cat=1, classified as
        # key_item) so they fell through every filter and showed NO marker - the player's
        # entire talisman source was invisible on the map. Catch every "* Remnant" good here
        # so they share the talisman icon + show_talismans toggle.
        # Match a remnant by name AND broad_category=key_item (its goodsType bucket): there is no
        # remnant-exclusive param field (goodsType=key_item is shared with map fragments, whetstones,
        # bell-bearings, etc.), and an id range would mis-hit other profiles since LOOT_CATEGORIES is
        # shared - so name is the most specific signal. Names are read from the engus msgbnd at build
        # time (language-independent), and base/non-Convergence profiles ship zero "Remnant" goods,
        # so this never mis-fires there. The key_item guard keeps a stray non-key "...Remnant"
        # consumable, if one ever appears, out of the talisman bucket.
        # NOTE: excludes "Physick Remnant"s - those craft Flask of Wondrous Physick crystal tears,
        # not talismans (per the Convergence docs), so they are routed to Key - Crystal Tears.
        # The Talisman Pouch (goods 10040) rides along: it is what talismans are worn in, and as a
        # key item it would otherwise sit in the Quest - Progression catch-all. Its two drops are
        # boss rewards (Margit, Godfrey), which only became markers once the flag-awarded boss
        # drops were placed.
        'filter': lambda items: any(
            i['category'] == 4
            or (i['category'] == 1 and i['id'] in TALISMAN_POUCH_IDS)
            or (i['category'] == 1 and i.get('broad_category') == 'key_item'
                and 'remnant' in (i.get('name') or '').lower()
                and 'physick' not in (i.get('name') or '').lower())
            for i in items),
        'iconId': 382,
        'startId': 4200000,
    },
    'Equipment - Spirits': {
        # Spirit Ashes. ERR renumbers spirit-ash goods into 300000-399999;
        # vanilla keeps them at their stock ids (200000+, goodsType==8 -
        # see goods_spirit_ash_ids.json from extract_goods_categories).
        # (The Lhutel id=358000 exclusion that previously lived here was a
        # workaround for phantom emevd records produced by templates
        # 90005200/90005210; those templates are no longer in
        # extract_all_items.py::TEMPLATE_EVENTS, so the workaround is moot.)
        'filter': (lambda items: any(
            i['category'] == 1 and i['id'] in SPIRIT_ASH_IDS
            for i in items
        )) if config.PROFILE != 'err' else (lambda items: any(
            i['category'] == 1 and 300000 <= i['id'] <= 399999
            for i in items
        )),
        'iconId': 383,
        'startId': 4300000,
    },
    'Equipment - Ashes of War': {
        'filter': lambda items: any(i['category'] == 5 for i in items),
        'iconId': 384,
        'startId': 4400000,
    },
    'Magic - Incantations': {
        'filter': lambda items: any(i['category'] == 1 and i['id'] in INCANTATION_IDS for i in items),
        'iconId': 385,
        'startId': 4500000,
    },
    'Magic - Sorceries': {
        'filter': lambda items: any(i['category'] == 1 and i['id'] in SORCERY_IDS for i in items),
        'iconId': 386,
        'startId': 4600000,
    },
    'Magic - Memory Stones': {
        'filter': lambda items: any(i['id'] == 10030 and i['category'] == 1 for i in items),
        'iconId': 429,
        'startId': 4700000,
    },
    'Magic - Prayerbooks': {
        # Prayerbooks and Scrolls that unlock spells at vendors - plus, on the Convergence
        # profiles, the Spell Runes that REPLACED that whole system.
        #
        # The Convergence gives out almost no spell pickups: its three magic categories hold
        # 5 sorceries, 2 incantations and 1 scroll between them, because "most spells in
        # Convergence are obtained by using Spell Runes" (their wiki) - 18 caster schools x
        # 6 tiers (Faint / Shimmering / Glowing / Shining / Radiant / Shadow), of which 99
        # lots are placed in the world. They are consumables that teach a spell, which is
        # what a scroll/prayerbook is, so they belong in this bucket rather than a new one.
        #
        # An ID RANGE is safe here even though LOOT_CATEGORIES is shared: 8300-8489 was
        # checked against every profile's items_database and is EMPTY in vanilla, err, erte,
        # goldenage, goldenage361, vins, reborn and graceborne - only convergence3 (97 ids)
        # and convergence2 (55) use it, so the range needs no profile gate and picks up both
        # Convergence generations by itself.
        #
        # Why this matters beyond the runes: the runes carry sortGroup=10, which is exactly
        # what 'Loot - Stat Boosts' filters on, so before this rule 96 of the 99 rune lots
        # were landing in Stat Boosts - a 157-marker category that was mostly Spell Runes
        # with the Starlight Shards it exists for in the minority. Prayerbooks is ordered
        # ahead of Stat Boosts, so first-match-wins moves them without touching that filter.
        'filter': lambda items: any(
            i['category'] == 1 and (
                i['id'] in (
                    8850, 8851, 8852, 8854,  # Conspectus, Royal House, Ranni's, Gelmirian Scrolls
                    8855, 8856, 8857, 8858,  # Fire Monks', Giant's, Godskin, Two Fingers' Prayerbooks
                    8859, 8862, 8864, 8865, 8866,  # Assassin's, Golden Order, Dragon Cult, Ancient Dragon, Academy
                    2008014,  # Secret Rite Scroll (DLC)
                )
                or 8300 <= i['id'] <= 8489  # Convergence Spell Runes (18 schools x 6 tiers)
            )
            for i in items
        ),
        'iconId': 427,
        'startId': 4800000,
    },
    'Loot - Stonesword Keys': {
        'filter': lambda items: any(i['id'] == 8000 and i['category'] == 1 for i in items),
        'iconId': 398,  # custom: key icon, 75% size, 80% bg, red tint
        'startId': 5000000,
    },
    'Loot - Bell-Bearings': {
        # Bell bearings from treasures / EMEVD awards (chests, quest rewards),
        # EXCLUDING the merchant-named ones (those are Merchant Bell-Bearings even
        # when a profile sources them from a treasure/map lot, not the enemy drop).
        'filter': lambda items: any('bell bearing' in i.get('name', '').lower() for i in items),
        'source_filter': lambda rec: rec.get('source') != 'enemy' and not _is_merchant_bell(rec),
        'iconId': 426,
        'startId': 5100000,
    },
    'Loot - Merchant Bell-Bearings': {
        # Bell bearings tied to killable merchants: dropped by the merchant NPC
        # (source == enemy: Kalé, Patches, Gostoc, nomadic/hermit/isolated, etc.)
        # OR named "<X> Merchant Bell Bearing" however the lot is sourced. Separated
        # so players can toggle merchant rewards independently of chest pickups.
        'filter': lambda items: any('bell bearing' in i.get('name', '').lower() for i in items),
        'source_filter': lambda rec: rec.get('source') == 'enemy' or _is_merchant_bell(rec),
        'iconId': 426,
        'startId': 5150000,
    },
    'Loot - Ammo': {
        # Ammo = cat-2 weapons of arrow/bolt class (AMMO_IDS = wepType 81/83/85/86,
        # from extract_goods_categories). Class-based, not an id range, so overhaul
        # weapons at high ids are not mistaken for ammo.
        'filter': lambda items: any(
            i['category'] == 2 and i['id'] in AMMO_IDS for i in items),
        'iconId': 388,
        'startId': 5200000,
    },
    'Loot - Smithing Stones (Low)': {
        # Smithing Stone [1]-[6], Somber [1]-[6]
        'filter': lambda items: any(i['id'] in (
            10100, 10101, 10102, 10103, 10104, 10105,  # Smithing Stone [1]-[6]
            10160, 10161, 10162, 10163, 10164, 10165,  # Somber [1]-[6]
        ) and i['category'] == 1 for i in items),
        'iconId': 433,
        'startId': 5300000,
    },
    'Loot - Smithing Stones': {
        # Smithing Stone [7]-[8], Somber [7]-[9], Scadushards, + SOTE Shadow/Somber
        # stones (goodsType 14; placed as treasure on overhauls like Convergence,
        # crafted-only/unplaced in vanilla so harmless there). Primordials -> Rare.
        'filter': lambda items: any(i['id'] in (
            10106, 10107,          # Smithing Stone [7]-[8]
            10166, 10167, 10200,   # Somber [7]-[9]
            10150, 10151,          # Smithing Scadushard, Somber Smithing Scadushard
            10110, 10111,          # Shadow Smithing Stone, Large Shadow Smithing Stone
            10170, 10171, 10172, 10173,  # Somber Shadow Stone: base/Large/Great/Colossal
        ) and i['category'] == 1 for i in items),
        'iconId': 378,
        'startId': 5350000,
    },
    'Loot - Smithing Stones (Rare)': {
        # Ancient Dragon Smithing Stone, Somber Ancient Dragon Smithing Stone, + the
        # top-tier SOTE Primordial Shadow/Somber stones (analogous to Ancient Dragon).
        'filter': lambda items: any(i['id'] in (
            10140,  # Ancient Dragon Smithing Stone
            10168,  # Somber Ancient Dragon Smithing Stone
            10114,  # Primordial Shadow Smithing Stone
            10174,  # Primordial Somber Shadow Stone
        ) and i['category'] == 1 for i in items),
        'iconId': 434,
        'startId': 5380000,
    },
    'Loot - Golden Runes (Low)': {
        # Golden Rune [200]-[3000], Broken Rune [500], Shadow Realm [2500]-[5000]
        'filter': lambda items: any(i['id'] in (
            2900, 2901, 2902, 2903, 2904, 2905, 2906, 2907,  # Golden Rune [200]-[3000]
            2002951,       # Broken Rune [500]
            2002952, 2002953,  # Shadow Realm Rune [2500]-[5000]
        ) and i['category'] == 1 for i in items),
        'iconId': 400,  # custom: consumable icon, 75% size, 80% bg, pale yellow tint
        'startId': 5400000,
    },
    'Loot - Golden Runes': {
        # Golden Rune [4000]+, Hero's, Numen's, Lord's, Shadow Realm [7500]+, Marika's
        'filter': lambda items: any(i['id'] in (
            2908, 2909, 2910, 2911, 2912,  # Golden Rune [4000]-[10000]
            2913,          # Numen's Rune [12500]
            2914, 2915, 2916, 2917, 2918,  # Hero's Rune [15000]-[35000]
            2919,          # Lord's Rune [50000]
            2002954, 2002955, 2002956, 2002957, 2002958,  # Shadow Realm [7500]-[30000]
            2002959,       # Rune of an Unsung Hero [50000]
            2002960,       # Marika's Rune [80000]
        ) and i['category'] == 1 for i in items),
        'iconId': 399,  # custom: consumable icon, 75% size, 80% bg, golden tint
        'startId': 5450000,
    },
    'Loot - Rune Arcs': {
        'filter': lambda items: any(i['id'] == RUNE_ARC_ID and i['category'] == 1 for i in items),
        'iconId': 411,
        'startId': 5500000,
    },
    'Loot - Dragon Hearts': {
        # 10060 Dragon Heart, 2008011 Heart of Bayle (the same Dragon Communion currency)
        'filter': lambda items: any(i['id'] in (10060, 2008011) and i['category'] == 1
                                    for i in items),
        'iconId': 413,
        'startId': 5510000,
    },
    'Loot - Gloveworts': {
        # Grave Glovewort [1-9] + Ghost Glovewort [1-9]
        'filter': lambda items: any(
            i['category'] == 1 and (10900 <= i['id'] <= 10908 or 10910 <= i['id'] <= 10918)
            for i in items
        ),
        'iconId': 435,
        'startId': 5900000,
    },
    'Loot - Great Gloveworts': {
        # Great Grave Glovewort (10909) + Great Ghost Glovewort (10919)
        'filter': lambda items: any(
            i['id'] in (10909, 10919) and i['category'] == 1 for i in items
        ),
        'iconId': 436,
        'startId': 5950000,
    },
    'Loot - Consumables': {
        # Healing/buff consumables: boluses, cured meats, dried livers (sortGroup=20),
        # plus ERR-specific consumables (sortGroup=61 - Lamp Oil etc).
        # Prattling Pates are excluded (they have their own category). CONSUMABLE_EXTRA
        # holds buff consumables an overhaul filed under an off sortGroup (Golden Age's
        # "Cold Blood" series sits in sortGroup 100 = golden runes, so by-id here).
        'filter': lambda items: any(
            i['category'] == 1
            and (GOODS_SORT_GROUPS.get(i['id'], -1) in (20, 61)
                 or i['id'] in (561518, 561519, 561520, 561521))  # Golden Age 冷血 [9]-[12]
            and i['id'] not in PATE_IDS
            for i in items
        ),
        'iconId': 387,
        'startId': 5600000,
    },
    'Loot - Greases': {
        # Weapon-buff greases (sortGroup=70)
        'filter': lambda items: any(
            i['category'] == 1 and GOODS_SORT_GROUPS.get(i['id'], -1) == 70
            for i in items
        ),
        'iconId': 408,
        'startId': 5610000,
    },
    'Loot - Utilities': {
        # Utility items: rainbow stones, glowstones, soap, soft cotton (sortGroup=80)
        # Excludes Prattling Pates (own category).
        'filter': lambda items: any(
            i['category'] == 1
            and GOODS_SORT_GROUPS.get(i['id'], -1) == 80
            and i['id'] not in PATE_IDS
            for i in items
        ),
        'iconId': 412,
        'startId': 5620000,
    },
    'Loot - Stat Boosts': {
        # goodsType=0 items in sortGroup=10 (Starlight Shards, etc). World-placed
        # loot only, so in practice mostly Starlight Shards.
        # Excludes Rune Arc (RUNE_ARC_ID, also sg=10) - it belongs to Unique Drops.
        'filter': lambda items: any(
            i['category'] == 1
            and GOODS_SORT_GROUPS.get(i['id'], -1) == 10
            and i['id'] != RUNE_ARC_ID  # Rune Arc -> Unique Drops
            for i in items
        ),
        'iconId': 410,
        'startId': 5630000,
    },
    'Loot - Throwables': {
        # Throwing items and tools: darts, daggers, stones, chakrams, DLC throwables
        # sortGroupId=50
        'filter': lambda items: any(
            i['category'] == 1 and GOODS_SORT_GROUPS.get(i['id'], -1) == 50
            for i in items
        ),
        'iconId': 409,
        'startId': 5650000,
    },
    'Loot - Crafting Materials': {
        # Gathering ingredients: herbs, bones, bugs, flowers, etc. (goodsType=2)
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in CRAFTING_IDS
            for i in items
        ),
        'iconId': 389,
        'startId': 5750000,
    },
    'Loot - Reusables': {
        # Multi-use tools (sortGroupId 60; goodsType=0, excluding AoW and Great Runes).
        # ERR relocates its reusable tools (Rock Heart, Priestess Heart, Lamenter's
        # Mask) to sortGroupId 81 - a group vanilla leaves empty - so 81 is included
        # too (safe: vanilla/other profiles have nothing there).
        'filter': lambda items: any(
            i['category'] == 1
            and GOODS_SORT_GROUPS.get(i['id'], -1) in (60, 81)
            and i['id'] < 4000000  # exclude Ashes of War goods
            and i['id'] not in (2008000,)  # exclude Miquella's Great Rune
            for i in items
        ),
        'iconId': 416,
        'startId': 5800000,
    },
    'Loot - MP-Fingers': {
        # Multiplayer items: Furled Fingers, Recusant Finger, etc. Furlcalling Finger
        # Remedy is matched by NAME, not id: its id is 150 in vanilla but 150 = Rune
        # Arc in ERR (a different item with a different name), so an id match would
        # misroute. Must be claimed here BEFORE the goodsType-1 Progression catch-all.
        'filter': lambda items: any((i['id'] in (
            100,   # Tarnished's Furled Finger
            101,   # Duelist's Furled Finger
            103,   # Finger Severer
            104,   # Host's Injured Finger
            105,   # Redeemer's Plated Finger
            106,   # Tarnished's Wizened Finger
            108,   # Taunter's Tongue
            110,   # Small Red Effigy
            111,   # Festering Bloody Finger
            112,   # Recusant Finger
        ) or 'furlcalling finger remedy' in (i.get('name') or '').lower())
            and i['category'] == 1 for i in items),
        'iconId': 414,
        'startId': 5850000,
    },
    'Loot - Prattling Pates': {
        'filter': lambda items: any(i['id'] in (
            2200, 2201, 2202, 2203, 2204, 2205, 2206, 2207,  # base game
            2002150,  # DLC: "Lamentation"
        ) and i['category'] == 1 for i in items),
        'iconId': 415,
        'startId': 5700000,
    },
    # MUST stay LAST: catch-all for key/quest items. Matches every goods row with
    # goodsType 1 (key item), 3 (remembrance) or 12 (info/note/message) - KEYITEM_IDS,
    # derived per-profile - that no more-specific category above already claimed
    # (first-match-wins in main()). Auto-includes medallions, needles, tailoring
    # tools, letters, prosthetics, eyes, cross-messages, Mirage Riddle, Whetstone
    # Knife, etc., and adapts to each profile's own key items. Excludes 'Map:'
    # fragments (the World - Maps generator marks those - skipping them here avoids
    # a double marker). PROGRESSION_EXTRA adds the goodsType-0 quest items the type
    # signal can't reach (Iris of Grace / Occultation, Miquella's Needle).
    'Quest - Progression': {
        'filter': lambda items: any(
            i['category'] == 1
            and not (i.get('name') or '').startswith('Map:')
            and (i['id'] in KEYITEM_IDS or i['id'] in PROGRESSION_EXTRA
                 or GOODS_SORT_GROUPS.get(i['id'], -1) == 90)  # sortGroup 90 = quest consumables
            for i in items
        ),
        'iconId': 376,
        'startId': 2200000,
    },
}

# Single source of truth for iconIds: resolve each category's icon from icon_registry (keyed by its human
# name). The 'iconId' numbers in the dict above are no longer the source - they are overwritten here, so
# the registry is the one place that owns icon numbering (and can auto-assign).
import icon_registry as _icon_registry
import row_id_registry as _row_id_registry
for _name, _cfg in LOOT_CATEGORIES.items():
    _cfg['iconId'] = _icon_registry.iconid_for_name(_name)
    # Row-ID base = this category's map z-order slot. The 'startId' numbers in the
    # dict above are no longer the source - row_id_registry owns ordering (reorder
    # its LAYER_ORDER to relayer; lower base draws on top). base() raises if the
    # category name isn't in LAYER_ORDER, catching renames at build time.
    _cfg['startId'] = _row_id_registry.base(_name)


def enemy_instance_keys(rec):
    """Two ways a record names its enemy, both with the tile's variant suffix dropped
    (m61_46_47_00 and m61_46_47_10 place the same enemy): its MSB part - a variant may move the
    part - and its spot - one NPC may be several parts in one place (Igon: c0000_9000/_9001,
    0.2u apart, one per state). Two records are the same enemy when either key matches."""
    tile = rec.get('map', '')[:9]
    pos = ('pos', tile, round(rec.get('x', 0)), round(rec.get('z', 0)))
    part = ('part', tile, rec['partName']) if rec.get('partName') else pos
    return part, pos


def deduplicate(records):
    """Remove _00/_10 MSB duplicates by (primary item id, rounded coords), and an enemy's drop
    repeated as the same enemy (enemy_instance_keys): the first record - the _00 tile, by MSB
    file order - is kept."""
    seen = set()
    unique = []
    dupes = 0
    for rec in records:
        primary_id = rec['items'][0]['id'] if rec['items'] else 0
        key = (primary_id, round(rec['x'], 1), round(rec['y'], 1), round(rec['z'], 1))
        enemy_keys = ([(primary_id,) + k for k in enemy_instance_keys(rec)]
                      if rec.get('source') == 'enemy' else [])
        if key in seen or any(k in seen for k in enemy_keys):
            dupes += 1
            continue
        seen.add(key)
        seen.update(enemy_keys)
        unique.append(rec)
    return unique, dupes


# Profile-independent localized enemy-name table (committed; built from the
# enemy-name source by extract_enemy_names_i18n). Used by the non-ERR builds so
# their enemy labels match the ERR-build quality; the strings themselves are
# FromSoft / community-wiki enemy names (see the comparison notes).
def _load_enemy_names_i18n():
    p = config.PROJECT_DIR / 'data' / 'enemy_names_i18n.json'
    if p.exists():
        with open(p, encoding='utf-8') as f:
            return json.load(f)  # {"<id>": {"engus": name, ...}}
    return {}

ENEMY_NAMES_I18N = _load_enemy_names_i18n()


def _model_map_from_i18n():
    """model 'cNNNN' -> base name id (NNNN*1000+4), derived from the i18n id set."""
    families = {}
    for k in ENEMY_NAMES_I18N:
        tid = int(k)
        if tid % 100 == 4:
            model, variant = tid // 1000, (tid % 1000) // 100
            if 1000 <= model <= 9999 and variant <= 9:
                families.setdefault(model, tid)
    return {f'c{m}': b for m, b in sorted(families.items())}


def load_enemy_names():
    """Enemy model -> name-id mapping. ERR uses its own extracted mapping; other
    profiles derive it from the profile-independent enemy-name table so their
    enemy labels resolve the same way. Either way, models with no name of their own
    borrow the base-game model that is the same enemy (data/enemy_model_aliases.json:
    the DLC scarabs c6201 -> c4191 and the like)."""
    names = None
    path = DATA_DIR / 'enemy_tutorial_mapping.json'
    if path.exists():
        with open(path) as f:
            names = json.load(f) or None
    names = names or _model_map_from_i18n()
    alias_path = config.PROJECT_DIR / 'data' / 'enemy_model_aliases.json'
    if alias_path.exists():
        with open(alias_path, encoding='utf-8') as f:
            for model, target in json.load(f).items():
                if not model.startswith('_') and model not in names and target in names:
                    names[model] = names[target]
    return names

ENEMY_NAMES = load_enemy_names()


def load_npc_name_ids():
    """NpcParam ID -> NpcName FMG id for named NPCs (Millicent, Vyke...)."""
    path = DATA_DIR / 'npc_name_ids.json'
    if path.exists():
        with open(path) as f:
            return {int(k): int(v) for k, v in json.load(f).items()}
    return {}

NPC_NAME_IDS = load_npc_name_ids()
from npcname_known import npcname_resolvable  # NpcName ids this profile can resolve


def load_bloodmsg_words():
    """Enemy model -> BloodMsg vocabulary word id (vanilla profile only).

    Hand-curated table for enemy types that have no proper-name string in
    vanilla FMGs (regular mobs): the closest word from the blood-message
    vocabulary (BloodMsg FMG, localized in all languages). Lives in the
    committed data/ root (profile-independent source table)."""
    path = config.PROJECT_DIR / 'data' / 'enemy_bloodmsg_mapping.json'
    if path.exists():
        with open(path, encoding='utf-8') as f:
            return {m: int(v['word_id']) for m, v in json.load(f).items()}
    return {}

BLOODMSG_WORDS = load_bloodmsg_words()


def load_tutorial_ids():
    """Valid name-entry IDs (for variant validation). Non-ERR profiles add the
    profile-independent enemy-name ids so variant resolution works there too."""
    ids = set()
    path = DATA_DIR / 'tutorial_title_ids.json'
    if path.exists():
        with open(path) as f:
            ids = set(json.load(f))
    if config.PROFILE != 'err':
        ids |= {int(k) for k in ENEMY_NAMES_I18N}
    return ids

TUTORIAL_IDS = load_tutorial_ids()


def _load_tutorial_names():
    """Name id -> clean name. Non-ERR profiles merge the (English) enemy-name
    table so variant text-matching resolves there too."""
    names = {}
    path = DATA_DIR / 'tutorial_title_names.json'
    if path.exists():
        with open(path, encoding='utf-8') as f:
            names = {int(k): v for k, v in json.load(f).items()}
    if config.PROFILE != 'err':
        for k, langs in ENEMY_NAMES_I18N.items():
            if 'engus' in langs:
                names.setdefault(int(k), langs['engus'])
    return names

TUTORIAL_NAMES = _load_tutorial_names()


def resolve_enemy_tutorial_id(enemy_model, npc_param_id, vanilla_place_name=None):
    """Resolve variant-specific TutorialTitle ID from NpcParam.

    If vanilla_place_name is provided, tries to match by text first
    (finds the variant whose TutorialTitle text matches the PlaceName).
    Falls back to NpcParam variant digit formula.
    """
    base_id = ENEMY_NAMES.get(enemy_model, 0)
    if base_id <= 0:
        return 0

    # Strategy 1: match by vanilla PlaceName text
    if vanilla_place_name:
        target = vanilla_place_name.lower().strip()
        for variant in range(10):
            vid = base_id + variant * 100
            tut_name = TUTORIAL_NAMES.get(vid, '')
            if tut_name and tut_name.lower().strip() == target:
                return vid

    # Strategy 2: NpcParam variant digit
    if npc_param_id <= 0:
        return base_id
    variant = (npc_param_id // 1000) % 10
    if variant == 0:
        return base_id
    variant_id = base_id + variant * 100
    if variant_id in TUTORIAL_IDS:
        return variant_id
    return base_id


REMEMBRANCE_GOODS_TYPE = 3   # EquipParamGoods.goodsType of a boss Remembrance


def boss_for_award_flag(drop, boss_by_flag):
    """The boss whose death event sets this award flag. A boss in the setter event's OWN map wins:
    Morgott's Leyndell event also sets Margit's Stormveil flag (one character, two fights)."""
    for map_must_match in (True, False):
        for setter in drop.get('setters', ()):
            for flag in setter.get('flags', ()):
                if flag == drop.get('awardFlag') or flag not in boss_by_flag:
                    continue
                boss = boss_by_flag[flag]
                if map_must_match and boss.get('map') != setter.get('map'):
                    continue
                return boss
    return None


def category_of_item(item):
    """The loot category a single item would land in, by the same filters the writer uses."""
    for cat_name, spec in LOOT_CATEGORIES.items():
        if config.PROFILE != 'err' and cat_name in ERR_ONLY_CATS:
            continue
        try:
            if spec['filter']([item]):
                return cat_name
        except Exception:
            continue
    return None


def boss_flag_drop_records(boss_by_flag):
    """Markers for the drops common event 1100 awards on a boss's death flag (collected by
    extract_all_items). The award carries no position, so the boss's own spot is the marker's.
    One record per (boss, category), so a boss that drops an armour set is one marker, not four.
    Left out: Remembrances (handed over together with the Great Rune - a second marker on the
    same boss says nothing new) and the Great Runes themselves, which have their own category."""
    path = DATA_DIR / 'boss_flag_drops.json'
    if not path.exists():
        print('  WARNING: boss_flag_drops.json not found - run extract_all_items.py')
        return []
    with open(path, encoding='utf-8') as f:
        drops = json.load(f)
    records = []
    unresolved = 0
    for drop in drops:
        boss = boss_for_award_flag(drop, boss_by_flag)
        if not boss:
            unresolved += 1
            continue
        kill_flag = boss.get('killEventFlagId', 0) or boss.get('clearedEventFlagId', 0)
        by_category = {}
        for item in drop.get('items', ()):
            if item.get('greatRune') or item.get('goodsType') == REMEMBRANCE_GOODS_TYPE:
                continue
            cat_name = category_of_item(item)
            if cat_name:
                by_category.setdefault(cat_name, []).append(item)
        for cat_name, items in by_category.items():
            records.append({
                'map': boss.get('map', ''),
                'x': boss.get('x', 0.0), 'y': boss.get('y', 0.0), 'z': boss.get('z', 0.0),
                'areaNo': boss.get('areaNo', 0),
                'gridX': boss.get('gridX', 0), 'gridZ': boss.get('gridZ', 0),
                'itemLotId': (drop.get('lots') or [0])[0],
                'eventFlag': kill_flag,
                'partName': '',
                'items': items,
                'primary_category': items[0].get('broad_category', ''),
                'source': 'boss_flag_award',
                'lotAggregate': True,  # one marker per category, not one per lot slot
                'guaranteed': True,
                'lotParam': 'map',
                'partBucket': 'live',
            })
    print(f'  {len(records)} boss-reward marker(s) from {len(drops)} flag-awarded drop(s)'
          + (f'; {unresolved} award(s) had no boss' if unresolved else ''))
    return records


def write_massedit(records, filepath, icon_id, start_id, lot_linkage=None):
    """Write MASSEDIT file + slots JSON from records.

    If lot_linkage (dict) is given, records each marker's source item-lot so the
    DLL can read the LIVE getItemFlagId/item from memory at runtime (live-loot /
    randomizer compatibility): lot_linkage[row_id] = [lotId, lotType, aggregate] where
    lotType 1=ItemLotParam_map (treasure/emevd), 2=ItemLotParam_enemy, and aggregate=1
    marks a marker that stands for SEVERAL of the lot's items (a boss's reward, split
    into one marker per category). For those the lot is not an address: slot 1 of it is
    some other category's item, so live labels and live hide-flags must leave them alone.
    """
    lines = []
    row_id = start_id

    for rec in records:
        area = rec['areaNo']
        gx = rec['gridX']
        gz = rec['gridZ']

        # Live-loot linkage: bake the source lot id + which param it's in.
        if lot_linkage is not None:
            _lot = rec.get('itemLotId', 0) or 0
            if _lot > 0:
                # Which param, as MEASURED by the extractor when it read the items - not inferred
                # from the marker being an enemy drop. A named NPC whose NpcParam uses the
                # itemLotId_map field drops through ItemLotParam_map, and looking it up in
                # ItemLotParam_enemy simply misses (the DLL then keeps the baked icon and logs an
                # error). The `source` fallback keeps older databases, from before lotParam was
                # recorded, behaving exactly as they did.
                _param = rec.get('lotParam')
                if _param == 'enemy':
                    _lt = 2
                elif _param == 'map':
                    _lt = 1
                else:
                    _lt = 2 if rec.get('source') == 'enemy' else 1
                lot_linkage[row_id] = [int(_lot), _lt,
                                       1 if rec.get('lotAggregate') else 0]

        # Primary item ID for localized text, offset-encoded by item category:
        #   cat=1 (goods):    id as-is         → DLL reads GoodsName FMG
        #   cat=2 (weapon):   id + 100000000   → DLL reads WeaponName FMG
        #   cat=3 (armour):   id + 200000000   → DLL reads ProtectorName FMG
        #   cat=4 (talisman): id + 300000000   → DLL reads AccessoryName FMG
        #   cat=5 (gem/aow):  id + 400000000   → DLL reads GemName FMG
        CATEGORY_OFFSETS = {1: 500000000, 2: 100000000, 3: 200000000, 4: 300000000, 5: 400000000}
        primary_item = rec['items'][0] if rec['items'] else {}
        item_id = primary_item.get('id', 0)
        item_cat = primary_item.get('category', 1)
        # Band-width invariant: each family's textId band is 100M wide, so a real id
        # must stay < 100M or it overflows into the NEXT family's band and the DLL
        # would read the wrong source FMG (wrong item name). Real ids are far below
        # this (weapons <=68M, goods <=~3M); fail the build loudly if a profile ever
        # ships an id that breaks it, instead of mislabelling a marker in-game.
        if item_id >= 100000000:
            raise SystemExit(
                f"[band-overflow] {cat_name}: item id {item_id} (cat {item_cat}) >= 100M "
                f"overflows its textId band into the next family. The offset encoding "
                f"requires real ids < 100M per family.")
        text_id1 = item_id + CATEGORY_OFFSETS.get(item_cat, 0) if item_id > 0 else 0

        # Determine display mask
        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif is_dlc_plane(area, gx):
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = {icon_id};')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')

        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gz > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')

        if rec['x'] != 0.0 or rec['z'] != 0.0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {rec["x"]:.3f};')
            if rec['y'] != 0.0:
                lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {rec["y"]:.3f};')
            lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {rec["z"]:.3f};')

        if text_id1 > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {text_id1};')

        # Event flag: hide text when collected
        flag = rec.get('eventFlag', 0)
        if flag > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {flag};')

        # Switched-chest ENABLE gate (group-2). Only fires for the chest that the
        # EMEVD enables when the switch flag is ON (Patches' m31_00 Glass Shard chest
        # today; the Cloth twin, enabled when the flag is OFF, is intentionally NOT
        # here - see switched_chests.py). sw is that flag: show this marker's icon
        # ONLY while the flag is ON (its chest is present). It still hides on pickup
        # via textDisableFlagId* (group-1). We MUST use an ENABLE flag, not a DISABLE
        # one: the engine hides a slot only when group1 AND group2 DISABLE flags BOTH
        # fire, so a group-2 disable would break the marker's own pickup-hide.
        # IMPORTANT: apply to EVERY populated text slot, not just slot 1 - the engine
        # drops the icon only once ALL its text lines are hidden, so gating only the
        # item line would leave the location/enemy line (and the icon) visible. Slot 1
        # is written here; slots 2/3 gate at their write sites.
        sw = SWITCH_GATE.get((rec.get('map', ''), rec.get('partName', '')))
        if sw:
            s_flag = sw
            s_field = 'textEnableFlag2Id'
            lines.append(f'param WorldMapPointParam: id {row_id}: {s_field}1: = {s_flag};')

        # Text slot order:
        #   1 = item name (above)
        #   2 = NPC name for named-NPC drops (Millicent, Vyke, ...) so the
        #       second line tells the player WHO drops this. Falls back to
        #       location subtitle when not a named-NPC drop.
        #   3 = location subtitle (if not already used as slot 2)
        #       OR generic enemy name (TutorialTitle: Scarab etc.)
        enemy_model = rec.get('enemyModel', '')
        npc_param = rec.get('npcParamId', 0)
        npc_name_id = NPC_NAME_IDS.get(npc_param, 0)
        next_text_slot = 2

        # Slot 2 - named-NPC name if available, else dungeon location. Only an id the profile's
        # NpcName FMG actually carries: NpcParam.nameId can point at nothing (Golden Age 3.6.8,
        # id 135700), and the DLL clears a text slot it cannot resolve - the drop then simply
        # loses the "who drops it" line, so it falls through to the location subtitle here.
        if npc_name_id > 0 and not npcname_resolvable(npc_name_id):
            npc_name_id = 0
        if npc_name_id > 0:
            npc_text_id = npc_name_id + 700000000  # NpcName FMG offset
            lines.append(f'param WorldMapPointParam: id {row_id}: textId{next_text_slot}: = {npc_text_id};')
            if flag > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId{next_text_slot}: = {flag};')
            if sw:
                lines.append(f'param WorldMapPointParam: id {row_id}: {s_field}{next_text_slot}: = {s_flag};')
            next_text_slot += 1

        # Location subtitle (for non-overworld). Becomes slot 2 for treasures
        # and slot 3 for named-NPC drops.
        if area not in OVERWORLD_AREAS:
            loc_id = resolve_location_id_at(
                rec.get('map', ''),
                float(rec.get('x', 0.0)),
                float(rec.get('y', 0.0)),
                float(rec.get('z', 0.0)),
            )
            if loc_id > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textId{next_text_slot}: = {loc_id};')
                if flag > 0:
                    lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId{next_text_slot}: = {flag};')
                if sw:
                    lines.append(f'param WorldMapPointParam: id {row_id}: {s_field}{next_text_slot}: = {s_flag};')
                next_text_slot += 1

        # Generic enemy name - only when we don't have a specific named-NPC
        # label. ERR: the ERR codex (TutorialTitle, +900M). Vanilla: vanilla
        # has no per-type enemy names in any FMG, so we use the closest word
        # from the blood-message vocabulary (BloodMsg FMG, +950M; localized
        # in all languages). Mapping: data/enemy_bloodmsg_mapping.json.
        if npc_name_id <= 0:
            enemy_text_id = 0
            tutorial_id = resolve_enemy_tutorial_id(enemy_model, npc_param)
            if tutorial_id > 0:
                enemy_text_id = tutorial_id + 900000000
            elif config.PROFILE != 'err':
                word_id = BLOODMSG_WORDS.get(enemy_model[:5], 0)
                if word_id > 0:
                    enemy_text_id = word_id + 950000000
            if enemy_text_id > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textId{next_text_slot}: = {enemy_text_id};')
                if flag > 0:
                    lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId{next_text_slot}: = {flag};')
                if sw:
                    lines.append(f'param WorldMapPointParam: id {row_id}: {s_field}{next_text_slot}: = {s_flag};')

        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')

        row_id += 1

    with open(filepath, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')

    return row_id - start_id


def main():
    OUT_DIR.mkdir(exist_ok=True)

    # row_id -> [lotId, lotType] for live-loot mode (consumed by generate_data.py)
    LOT_LINKAGE = {}

    print('Loading items database...')
    with open(DB_PATH, encoding='utf-8') as f:
        db = json.load(f)
    print(f'  {len(db)} records')

    # Boss rewards that the game awards on a death flag, with no position of their own: give them
    # the boss's spot and let the ordinary categories below claim them (boss_flag_drop_records).
    boss_by_flag = {}
    _boss_list_path_early = DATA_DIR / 'boss_list.json'
    if _boss_list_path_early.exists():
        with open(_boss_list_path_early, encoding='utf-8') as f:
            for b in json.load(f):
                for key in ('clearedEventFlagId', 'killEventFlagId'):
                    if b.get(key, 0) > 0:
                        boss_by_flag.setdefault(b[key], b)
    db += boss_flag_drop_records(boss_by_flag)

    # Switched-chest gating: derive from the FULL record set (positions/pairs must be
    # complete, before the eventFlag/source filters below).
    global SWITCH_GATE
    SWITCH_GATE = build_switch_gate_map(db)
    if SWITCH_GATE:
        print(f'  {len(SWITCH_GATE)} switched-chest markers enable-gated (show only when flag ON): '
              + ', '.join(f'{pn}@{mp}->show-on-{v}'
                          for (mp, pn), v in sorted(SWITCH_GATE.items())))

    # Skip fallback records (no coordinates) and respawning enemy drops (no event flag)
    db = [r for r in db if not r.get('from_fallback')]
    print(f'  {len(db)} with coordinates')
    db = [r for r in db if r.get('eventFlag', 0) > 0]
    print(f'  {len(db)} with event flags (one-time pickups)')

    # For enemy drops: exclude shared flags (generic drops from common enemies)
    # Only keep enemy entries whose flag is unique (used by 1 entry) or from EMEVD/treasure.
    # EXCEPTION: a GUARANTEED SET from a UNIQUE enemy. "Guaranteed" = the lot has no
    # "nothing" slot, so the item always drops (e.g. vanilla Battlemage Set lots
    # 703-706 sharing getItemFlagId 1040557700). "Unique" = all records on that flag
    # sit at ONE world position (one enemy) - this keeps unique field enemies like the
    # Battlemage but drops quest NPCs (Patches, the Twin Maiden brother, Igon...) whose
    # one set shares a flag across many spawn spots and would otherwise spam duplicate
    # icons. The shared flag is the lot getItemFlagId, set when the set is obtained, so
    # all pieces' markers disappear together correctly. Probabilistic drops (with a
    # nothing slot) have no flag and were already removed by the event-flag filter above.
    # "One enemy" is counted with the tile's variant suffix dropped (enemy_instance_keys):
    # m61_46_47_00 and m61_46_47_10 carry the same c5240_9088, and keying the map name WITH its
    # variant counted that single enemy twice - which dropped 11 of vanilla's 20 flagged Pot
    # Shadow lots (Antiquity Scholar's Cookbook [1] among them). One enemy = one part OR one spot.
    from collections import Counter
    flag_counts = Counter(r.get('eventFlag', 0) for r in db)
    flag_parts, flag_spots = {}, {}
    for r in db:
        part, spot = enemy_instance_keys(r)
        flag_parts.setdefault(r.get('eventFlag', 0), set()).add(part)
        flag_spots.setdefault(r.get('eventFlag', 0), set()).add(spot)

    def one_enemy(flag):
        return len(flag_parts[flag]) == 1 or len(flag_spots[flag]) == 1

    before = len(db)
    db = [r for r in db if r.get('source') != 'enemy'
          or flag_counts[r['eventFlag']] == 1
          or (r.get('guaranteed') and one_enemy(r.get('eventFlag', 0)))]
    print(f'  {len(db)} after filtering shared enemy flags (-{before - len(db)})')


    # First-match-wins: each record is claimed by the FIRST category (in dict
    # order) whose filter matches it, so a later catch-all (Quest - Progression)
    # only picks up what no specific category above took. The category filters are
    # otherwise disjoint (verified: 0 records match >1 specific filter), so this
    # changes nothing for them - it only makes the goodsType-1/12 Progression
    # catch-all skip the key items already routed to cookbooks/bell-bearings/etc.
    _assigned = set()  # id(record) of records already claimed by an earlier category

    for cat_name, cat_cfg in LOOT_CATEGORIES.items():
        if config.PROFILE != 'err' and cat_name in ERR_ONLY_CATS:
            print(f'\n=== {cat_name} === (skipped: ERR-only)')
            continue
        filter_fn = cat_cfg['filter']
        icon_id = cat_cfg['iconId']
        start_id = cat_cfg['startId']

        # Filter matching records (skip ones an earlier category already claimed)
        source_filter = cat_cfg.get('source_filter')
        matched = [r for r in db if id(r) not in _assigned
                   and filter_fn(r['items']) and (not source_filter or source_filter(r))]
        _assigned.update(id(r) for r in matched)
        print(f'\n=== {cat_name} ===')
        print(f'  Matched: {len(matched)}')

        # Deduplicate
        unique, dupes = deduplicate(matched)
        if dupes:
            print(f'  Deduplicated: {dupes} removed, {len(unique)} unique')

        # Sort by area, grid, position
        unique.sort(key=lambda r: (r['areaNo'], r['gridX'], r['gridZ'], r['x'], r['z']))

        # Write MASSEDIT
        massedit_path = OUT_DIR / f'{cat_name}.MASSEDIT'
        count = write_massedit(unique, massedit_path, icon_id, start_id, LOT_LINKAGE)
        print(f'  Written {count} entries to {massedit_path.name}')

        # Stats
        areas = defaultdict(int)
        with_flag = 0
        for r in unique:
            areas[r['areaNo']] += 1
            if r.get('eventFlag', 0) > 0:
                with_flag += 1
        print(f'  With event flags: {with_flag}/{len(unique)}')
        print(f'  By area: {dict(sorted(areas.items()))}')


    # ── Bosses category: from boss_list.json (vanilla 217 + MSB coords + text matching) ──
    print('\n=== World - Bosses ===')

    _boss_list_path = DATA_DIR / 'boss_list.json'
    if _boss_list_path.exists():
        with open(_boss_list_path, encoding='utf-8') as _f:
            boss_list = json.load(_f)
    else:
        boss_list = []
        print('  WARNING: boss_list.json not found')

    # model -> a NpcName id some placement of that model carries, for the unnamed ones below.
    boss_name_by_model = {}
    for _b in boss_list:
        if _b.get('npcNameId', 0) > 0 and _b.get('enemyModel'):
            boss_name_by_model.setdefault(_b['enemyModel'], _b['npcNameId'])

    lines = []
    row_id = _row_id_registry.base("World - Bosses")  # z-order slot; see row_id_registry
    boss_count = 0
    text_matched = 0
    for rec in sorted(boss_list, key=lambda r: (r['areaNo'], r.get('gridX', 0), r.get('gridZ', 0))):
        area = rec['areaNo']
        gx = rec.get('gridX', 0)
        gz = rec.get('gridZ', 0)

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif is_dlc_plane(area, gx):
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = {__import__("icon_registry").iconid("bosses")};')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')

        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gz > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')

        if rec['x'] != 0.0 or rec['z'] != 0.0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {rec["x"]:.3f};')
            if rec['y'] != 0.0:
                lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {rec["y"]:.3f};')
            lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {rec["z"]:.3f};')

        # textId1: enemy name via TutorialTitle (text-matched with vanilla PlaceName)
        enemy_model = rec.get('enemyModel', '')
        npc_param = rec.get('npcParamId', 0)
        vanilla_place_name = rec.get('vanillaPlaceName', '')
        tutorial_id = resolve_enemy_tutorial_id(enemy_model, npc_param, vanilla_place_name)
        if vanilla_place_name and tutorial_id != resolve_enemy_tutorial_id(enemy_model, npc_param):
            text_matched += 1
        # Never point a marker at a name nothing can resolve: the id comes from the model+variant
        # formula, and the entry may exist in neither the build's codex nor the enemy-name table
        # this mod injects. Both count as resolvable - the avatars (c4810 / c5230) have no codex
        # entry in any profile but ARE named by the injected table, so the guard must not drop them.
        if (tutorial_id > 0 and (TUTORIAL_IDS or ENEMY_NAMES_I18N)
                and tutorial_id not in TUTORIAL_IDS
                and str(tutorial_id) not in ENEMY_NAMES_I18N):
            tutorial_id = 0
        # Same enemy elsewhere: another placement of the SAME model usually has a proper NpcName
        # (the avatars above are named on their other spots), so borrow it before giving up.
        name_id = rec.get('npcNameId', 0)
        if tutorial_id <= 0 and name_id <= 0:
            name_id = boss_name_by_model.get(enemy_model, 0)
        if tutorial_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {tutorial_id + 900000000};')
        elif name_id > 0:
            # Vanilla: standard boss name from NpcName (every HP-bar boss has one)
            lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {name_id + 700000000};')
        else:
            # Fallback: PlaceName ID from ERR WorldMapPointParam, else the
            # generic BloodMsg word "boss" (vanilla, localized)
            wmp_tid = rec.get('wmpTextId1', 0)
            if wmp_tid > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {wmp_tid};')
            elif config.PROFILE != 'err':
                lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {950000000 + 30006};')

        # Kill flag for green checkmark AND hide-when-killed option
        kill_flag = rec.get('killEventFlagId', 0)
        cleared_flag = kill_flag if kill_flag > 0 else rec.get('clearedEventFlagId', 0)
        if cleared_flag > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: clearedEventFlagId: = {cleared_flag};')
            # Also set textDisableFlagId1 - C++ config chooses which to use:
            # green checkmark (clearedEventFlagId) or hide killed (textDisableFlagId1)
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {cleared_flag};')

        # textId2: location name for dungeons - nearest-grace lookup
        loc_id = resolve_location_id_at(
            rec.get('map', ''),
            float(rec.get('x', 0.0)),
            float(rec.get('y', 0.0)),
            float(rec.get('z', 0.0)),
        )
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
            if cleared_flag > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {cleared_flag};')

        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1
        boss_count += 1

    massedit_path = OUT_DIR / 'World - Bosses.MASSEDIT'
    with open(massedit_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'  Written {boss_count} entries ({text_matched} text-matched) to {massedit_path.name}')


    # ── Great Runes: dropped by story bosses ──
    print('\n=== Key - Great Runes ===')

    # No rune/boss table: extract_all_items derives the runes and their award flags from the
    # build's own EMEVD (common event 1100, "Defeat boss_obtain item" - a flag-triggered award
    # with no position, which is why these need the boss's spot). Here each award flag is matched
    # to the boss whose death event sets it, by the defeat flag set in the same event.
    # The old table matched by English boss name and lost runes wherever the names were not those
    # words (measured 2026-09-11: all six missing in Golden Age 3.6.1, which ships Chinese text
    # under msg/engus; Malenia in Throne, Radahn in VINS 1.9.1, each because the boss had no name).
    drops_path = DATA_DIR / 'boss_flag_drops.json'
    all_drops = []
    if drops_path.exists():
        with open(drops_path, encoding='utf-8') as f:
            all_drops = json.load(f)
    else:
        print('  WARNING: boss_flag_drops.json not found - run extract_all_items.py')
    # One entry per rune: the item to LABEL the marker with (the restored rune) and its award.
    rune_drops = []
    for drop in all_drops:
        for item in drop.get('items', ()):
            if item.get('greatRune'):
                rune_drops.append((item.get('labelItem', item['id']), item.get('name', ''), drop))

    lines = []
    row_id = _row_id_registry.base("Key - Great Runes")  # z-order slot; see row_id_registry
    gr_count = 0
    seen_runes = set()
    for rune_id, rune_name, drop in sorted(rune_drops, key=lambda d: d[0]):
        if rune_id in seen_runes:
            continue  # one marker per rune even if the award is called from two maps
        boss = boss_for_award_flag(drop, boss_by_flag)
        if not boss:
            print(f'  WARNING: no boss sets flag {drop.get("awardFlag")} for rune {rune_id} '
                  f'"{rune_name}" (awarded in {drop.get("map", "?")})')
            continue
        seen_runes.add(rune_id)

        area = boss['areaNo']
        gx = boss.get('gridX', 0)
        gz = boss.get('gridZ', 0)
        kill_flag = boss.get('killEventFlagId', 0)

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif is_dlc_plane(area, gx):
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = {_icon_registry.iconid_for_name("Key - Great Runes")};')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {boss["x"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {boss["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {boss["z"]:.3f};')

        # Text: Great Rune name - hide when boss killed (rune obtained)
        lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {500000000 + rune_id};')
        if kill_flag > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {kill_flag};')

        # Dungeon location text - nearest-grace lookup
        loc_id = resolve_location_id_at(
            boss.get('map', ''),
            float(boss.get('x', 0.0)),
            float(boss.get('y', 0.0)),
            float(boss.get('z', 0.0)),
        )
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
            if kill_flag > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {kill_flag};')

        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1
        gr_count += 1

    massedit_path = OUT_DIR / 'Key - Great Runes.MASSEDIT'
    with open(massedit_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'  Written {gr_count} entries to {massedit_path.name}')

    # Live-loot lot linkage (row_id -> [lotId, lotType]); generate_data.py joins
    # this onto MapEntry so the DLL can read the live ItemLotParam at runtime.
    linkage_path = DATA_DIR / 'loot_lot_linkage.json'
    with open(linkage_path, 'w', encoding='utf-8') as f:
        json.dump(LOT_LINKAGE, f)
    print(f'\n  Wrote {len(LOT_LINKAGE)} lot-linkage entries to {linkage_path.name}')

    # ── Live-loot icon/category table (consumed by generate_data.py) ──
    # Map every item to the iconId + MFG category it would get as a normal
    # marker, by running the SAME ordered LOOT_CATEGORIES classifier on it as a
    # singleton lot (first matching filter wins). At runtime the DLL reads the
    # live ItemLotParam item, looks it up here, and re-icons + re-gates the
    # marker so randomized loot shows the right icon under its own toggle.
    # Key = offset-encoded item id (identical to the marker textId encoding).
    def _encode_item(iid, cat):
        if cat == 1: return iid + 500000000          # goods
        if cat == 2: return iid if iid >= 50000000 else iid + 100000000  # ammo / weapon
        if cat == 3: return iid + 200000000          # protector
        if cat == 4: return iid + 300000000          # accessory
        if cat == 5: return iid + 400000000          # gem (ash of war)
        return None

    with open(DB_PATH, encoding='utf-8') as f:
        _raw_db = json.load(f)
    icon_table = {}  # encoded_key -> [iconId, category_name, english_name]
    for rec in _raw_db:
        for it in rec.get('items', []):
            cat = it.get('category', 0)
            iid = it.get('id', 0)
            key = _encode_item(iid, cat)
            if key is None or iid <= 0 or key in icon_table:
                continue
            single = [it]
            for cn, cc in LOOT_CATEGORIES.items():
                if config.PROFILE != 'err' and cn in ERR_ONLY_CATS:
                    continue
                try:
                    if cc['filter'](single):
                        # 3rd field = English item name (items_database is read from the
                        # engus msgbnd). Baked into goblin_item_fallback.cpp and injected as
                        # the lowest-priority PlaceName layer, so a marker whose item has no
                        # string in the player's language falls back to English instead of
                        # showing "?PlaceName?". Localized strings (added earlier) win the dedup.
                        icon_table[key] = [cc['iconId'], cn, it.get('name', '')]
                        break
                except Exception:
                    continue
    icon_table_path = DATA_DIR / 'item_icon_table.json'
    with open(icon_table_path, 'w', encoding='utf-8') as f:
        json.dump(icon_table, f)
    print(f'  Wrote {len(icon_table)} item-icon entries to {icon_table_path.name}')


if __name__ == '__main__':
    main()

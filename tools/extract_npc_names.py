#!/usr/bin/env python3
"""Extract the active profile's NPC name tables for the marker generators.

Outputs (data/<profile>/):
  npc_name_id_map.json    NpcParam row id -> nameId (the NpcName FMG id)
  npc_name_text_map.json  NpcName FMG id  -> English text (engus item_dlc02.msgbnd)

A pipeline bootstrap stage, so both tables are rebuilt from the profile's own regulation.bin and
text whenever those change. They used to be lazy caches inside generate_pieces.py: written once
if missing and never refreshed, and read by earlier stages before generate_pieces had written
them, so a first build from an empty workspace and a rebuild baked different names.
"""
import json
import os
import sys
import tempfile

import config
from pythonnet import load as _pyload

_pyload('coreclr')
from System import Array, Object, Type as SysType  # noqa: E402
from System.IO import File as SysFile  # noqa: E402
from System.Reflection import Assembly  # noqa: E402

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats  # noqa: E402

_STR = SysType.GetType('System.String')
_PARAM_READ = asm.GetType('SoulsFormats.PARAM').BaseType.GetMethod('Read', Array[SysType]([_STR]))
_FMG_READ = asm.GetType('SoulsFormats.FMG').BaseType.GetMethod('Read', Array[SysType]([_STR]))
_BND_READ = asm.GetType('SoulsFormats.BND4').BaseType.GetMethod('Read', Array[SysType]([_STR]))


def _read_via_tmp(reader, raw, suffix):
    tmp = os.path.join(tempfile.gettempdir(), f'{os.getpid()}_npcnames{suffix}')
    SysFile.WriteAllBytes(tmp, raw)
    try:
        return reader.Invoke(None, Array[Object]([tmp]))
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass


def npc_param_name_ids(src):
    defs = {}
    for xml in config.PARAMDEF_DIR.glob('*.xml'):
        try:
            d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
            if d and d.ParamType:
                defs[str(d.ParamType)] = d
        except Exception:
            pass
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(src / 'regulation.bin'))
    for f in bnd.Files:
        if 'NpcParam.param' not in str(f.Name):
            continue
        p = _read_via_tmp(_PARAM_READ, f.Bytes.ToArray(), '.param')
        p.ApplyParamdef(defs[str(p.ParamType)])
        out = {}
        for r in p.Rows:
            for c in r.Cells:
                if str(c.Def.InternalName) == 'nameId':
                    nid = int(c.Value)
                    if nid > 0:
                        out[int(r.ID)] = nid
                    break
        return out
    sys.exit('NpcParam not found in regulation.bin')


def npc_name_texts(src):
    out = {}
    bnd = _BND_READ.Invoke(None, Array[Object]([str(src / 'msg' / 'engus' / 'item_dlc02.msgbnd.dcx')]))
    for f in bnd.Files:
        if 'NpcName' not in str(f.Name):
            continue
        fmg = _read_via_tmp(_FMG_READ, f.Bytes.ToArray(), '.fmg')
        for e in fmg.Entries:
            t = str(e.Text) if e.Text else ''
            if t and t != '[ERROR]':
                out.setdefault(int(e.ID), t)
    return out


def main():
    src = config.require_err_mod_dir()  # profile-aware: ERR mod, vanilla game or the merged overlay
    out = config.DATA_DIR
    out.mkdir(parents=True, exist_ok=True)
    ids = npc_param_name_ids(src)
    texts = npc_name_texts(src)
    with open(out / 'npc_name_id_map.json', 'w', encoding='utf-8') as fp:
        json.dump({str(k): v for k, v in sorted(ids.items())}, fp)
    with open(out / 'npc_name_text_map.json', 'w', encoding='utf-8') as fp:
        json.dump({str(k): v for k, v in sorted(texts.items())}, fp, ensure_ascii=False)
    print(f'{len(ids)} NpcParam name ids, {len(texts)} NpcName strings -> {out}')


if __name__ == '__main__':
    main()

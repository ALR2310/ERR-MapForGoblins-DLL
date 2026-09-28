"""Extract MSB MapNameOverride (and related) regions -> data/map_name_regions.json.

These are the 3D volumes the GAME uses to decide the on-screen / map area name (containment,
not distance). Each MapNameOverride = a shape (usually Box) + TextID (a PlaceName id). A loot's
location can then be named by a point-in-volume test against these regions = the game's own logic.

Usage:  py extract_map_name_regions.py [map_glob]   (default: all m*.msb.dcx in the ERR mod)
"""
import os, sys, io, json, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pythonnet import load; load("coreclr")
import clr, config
from System import Array, Object, Type as SysType
from System.IO import File as SysFile
from System.Reflection import Assembly, BindingFlags

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL)); clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType("System.String")
_msbe_read = asm.GetType("SoulsFormats.MSBE").GetMethod(
    "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

def rfb(rm, data, suf=".bin"):
    t = os.path.join(tempfile.gettempdir(), str(os.getpid()) + "_mfg_tmp" + suf)
    SysFile.WriteAllBytes(t, data.ToArray() if hasattr(data, "ToArray") else data)
    r = rm.Invoke(None, Array[Object]([t])); os.unlink(t); return r

PLACE = json.load(open(os.path.join(config.TOOLS_DIR if hasattr(config,'TOOLS_DIR') else '.', "..", "data", "err", "PlaceName_engus.json"), encoding="utf-8")) \
    if os.path.exists(os.path.join("..", "data", "err", "PlaceName_engus.json")) else \
    json.load(open(os.path.join(os.path.dirname(__file__), "..", "data", "err", "PlaceName_engus.json"), encoding="utf-8"))

def prop(o, name):
    p = o.GetType().GetProperty(name)
    return p.GetValue(o) if p else None

def vec3(v):
    return None if v is None else [float(v.X), float(v.Y), float(v.Z)]

def shape_dims(sh):
    if sh is None: return ("None", {})
    t = sh.GetType().Name
    dims = {}
    for n in ("Width", "Height", "Depth", "Radius"):
        p = sh.GetType().GetProperty(n)
        if p: dims[n] = float(p.GetValue(sh))
    return (t, dims)

WANT = {"MapNameOverride", "MapPoint", "MapPointDiscoveryOverride", "PlayArea"}

def main():
    mod = config.require_err_mod_dir() if hasattr(config, "require_err_mod_dir") else None
    msb_dir = (mod / "map" / "MapStudio") if mod else \
        (config.GAME_DIR / "map" / "mapstudio") if config.GAME_DIR else None
    paths = sorted(msb_dir.glob(sys.argv[1] if len(sys.argv) > 1 else "m*.msb.dcx"))
    out = {}
    total = 0
    for mp in paths:
        mapname = mp.name.replace(".msb.dcx", "")
        try:
            msb = rfb(_msbe_read, SoulsFormats.DCX.Decompress(str(mp)), ".msb")
            regions = list(msb.Regions.GetEntries())
        except Exception as e:
            continue
        recs = []
        for r in regions:
            tn = type(r).__name__
            if tn not in WANT: continue
            textId = prop(r, "TextID")
            wmpId = prop(r, "WorldMapPointParamID")
            playId = prop(r, "PlayRegionID")
            shp, dims = shape_dims(prop(r, "Shape"))
            rec = {
                "kind": tn,
                "pos": vec3(prop(r, "Position")),
                "rot": vec3(prop(r, "Rotation")),
                "shape": shp, "dims": dims,
            }
            if textId is not None and int(textId) >= 0:
                rec["textId"] = int(textId)
                rec["name"] = PLACE.get(str(int(textId)))
            if wmpId is not None and int(wmpId) >= 0:
                rec["wmpId"] = int(wmpId)
            if playId is not None and int(playId) >= 0:
                rec["playRegionId"] = int(playId)
            recs.append(rec)
        if recs:
            out[mapname] = recs
            total += len(recs)
            mno = sum(1 for x in recs if x["kind"] == "MapNameOverride")
            print(f"  {mapname}: {len(recs)} regions ({mno} MapNameOverride)")
    dst = os.path.join(os.path.dirname(__file__), "..", "data", "err", "map_name_regions.json")
    json.dump(out, open(dst, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"\n{total} regions across {len(out)} maps -> data/map_name_regions.json")

if __name__ == "__main__":
    main()

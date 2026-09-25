// Expanding the packed map table. See goblin_map_blob.hpp for why it is packed at all.
//
// THE LAYOUT LIVES IN TWO PLACES and they have to agree: this file and tools/mapblob.py. The
// version word is the guard - the packer writes it, this refuses anything else rather than reading
// a differently-shaped record as if it were this one.
//
//   magic      'MFGD'                      4
//   version    u16                         2
//   count      u32                         4
//   strtab_len u32                         4
//   strtab     NUL-separated names         strtab_len   (index 0 is the empty name = nullptr)
//   count x record:
//     row_id        u32
//     mask          u32     bit i set = FIELD_ORDER[i] present, in that order
//     category      u8
//     lot_type      u8
//     lot_aggregate u8
//     state_show    u8      StateShow (v3; the pad byte in v2)
//     geom_slot     i16
//     name_suffix   i16
//     name_index    u16     index into strtab; 0 = none
//     (pad)         u16
//     lot_id        u32
//     real_posX     f32
//     real_posZ     f32
//     state_flag    u32     the world-state rule's event flag, 0 = none (v3)
//     then, for each set mask bit in order, 4 bytes: f32 for a position field, u32 otherwise.

#include "goblin_map_blob.hpp"

#include "miniz.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <string>
#include <vector>

namespace goblin::generated
{

// Supplied by the generated translation unit (src/generated*/goblin_map_blob_data.cpp).
extern const unsigned char MAP_BLOB[];
extern const unsigned int MAP_BLOB_SIZE;
extern const unsigned int MAP_BLOB_RAW_SIZE;

const MapEntry *MAP_ENTRIES = nullptr;
size_t MAP_ENTRY_COUNT = 0;

namespace
{

// 3: the world-state rule (state_show, state_flag) joined the record head.
constexpr uint16_t kBlobVersion = 3;
constexpr size_t kRecordHead = 36;  // tools/mapblob.py RECORD_HEAD_SIZE

// The param fields any generator writes, in the packer's bit order. Adding one goes on the END in
// both this list and tools/rowsink.py, or every record after it decodes one field out of step.
enum class FieldKind : uint8_t { U32, F32 };

using Param = from::paramdef::WORLD_MAP_POINT_PARAM_ST;

struct FieldSlot
{
    // Writing through a lambda rather than a member offset: three of these are bit-fields, which
    // have no address, and the signed/unsigned width differs per field (see the paramdef).
    void (*apply)(Param &, uint32_t, float);
    FieldKind kind;
};

#define U32_FIELD(member, type)                                                                    \
    FieldSlot                                                                                      \
    {                                                                                              \
        [](Param &p, uint32_t v, float) { p.member = static_cast<type>(v); }, FieldKind::U32        \
    }
#define BIT_FIELD(member)                                                                          \
    FieldSlot                                                                                      \
    {                                                                                              \
        [](Param &p, uint32_t v, float) { p.member = (v != 0); }, FieldKind::U32                    \
    }
#define F32_FIELD(member)                                                                           \
    FieldSlot                                                                                       \
    {                                                                                               \
        [](Param &p, uint32_t, float f) { p.member = f; }, FieldKind::F32                           \
    }

// EXACTLY tools/rowsink.py's FIELDS, in that order: the packer writes bit i for FIELDS[i]. A field
// added there goes on the END here. `pad2_0` is rowsink's paramdef name for what our struct calls
// dispMask02 (the DLC map plane), which is the one name that differs between the two lists.
const FieldSlot kFields[] = {
    U32_FIELD(iconId, unsigned short),
    BIT_FIELD(dispMask00),
    BIT_FIELD(dispMask01),
    BIT_FIELD(dispMask02),
    U32_FIELD(areaNo, unsigned char),
    U32_FIELD(gridXNo, unsigned char),
    U32_FIELD(gridZNo, unsigned char),
    F32_FIELD(posX),
    F32_FIELD(posY),
    F32_FIELD(posZ),
    U32_FIELD(textId1, int),
    U32_FIELD(textId2, int),
    U32_FIELD(textId3, int),
    U32_FIELD(textDisableFlagId1, unsigned int),
    U32_FIELD(textDisableFlagId2, unsigned int),
    U32_FIELD(textDisableFlagId3, unsigned int),
    U32_FIELD(textEnableFlag2Id1, int),
    U32_FIELD(textEnableFlag2Id2, int),
    U32_FIELD(clearedEventFlagId, unsigned int),
    U32_FIELD(selectMinZoomStep, unsigned char),
};
constexpr size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

#undef U32_FIELD
#undef BIT_FIELD
#undef F32_FIELD

// The expanded table owns its storage for the process's life: MapEntry::object_name points into
// g_names, and goblin_inject hands &entry.data straight to the param rebuild.
std::vector<MapEntry> g_entries;
std::vector<char> g_names;
bool g_loaded = false;

template <typename T> T take(const unsigned char *&p)
{
    T v{};
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}

} // namespace

bool load_map_data()
{
    if (g_loaded)
        return MAP_ENTRY_COUNT != 0;
    g_loaded = true;

    if (MAP_BLOB_SIZE == 0 || MAP_BLOB_RAW_SIZE == 0)
    {
        spdlog::error("[mapdata] the packed table is empty; no markers this session");
        return false;
    }

    std::vector<unsigned char> raw(MAP_BLOB_RAW_SIZE);
    mz_ulong out_len = MAP_BLOB_RAW_SIZE;
    if (mz_uncompress(raw.data(), &out_len, MAP_BLOB, MAP_BLOB_SIZE) != MZ_OK ||
        out_len != MAP_BLOB_RAW_SIZE)
    {
        spdlog::error("[mapdata] the packed table would not expand ({} -> {} of {} bytes); "
                      "no markers this session",
                      MAP_BLOB_SIZE, static_cast<unsigned>(out_len), MAP_BLOB_RAW_SIZE);
        return false;
    }

    const unsigned char *p = raw.data();
    const unsigned char *end = raw.data() + raw.size();
    if (std::memcmp(p, "MFGD", 4) != 0)
    {
        spdlog::error("[mapdata] the packed table has the wrong magic; no markers this session");
        return false;
    }
    p += 4;
    const uint16_t version = take<uint16_t>(p);
    if (version != kBlobVersion)
    {
        spdlog::error("[mapdata] packed table version {}, this build reads {}; no markers "
                      "this session",
                      version, kBlobVersion);
        return false;
    }
    const uint32_t count = take<uint32_t>(p);
    const uint32_t strtab_len = take<uint32_t>(p);
    if (p + strtab_len > end)
    {
        spdlog::error("[mapdata] the packed table's name block runs past its end; no markers");
        return false;
    }
    g_names.assign(reinterpret_cast<const char *>(p), reinterpret_cast<const char *>(p) + strtab_len);
    p += strtab_len;

    g_entries.clear();
    g_entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (p + kRecordHead > end)
        {
            spdlog::error("[mapdata] the packed table ends mid-record at {} of {}; no markers",
                          i, count);
            g_entries.clear();
            return false;
        }
        MapEntry e{};
        e.row_id = take<uint32_t>(p);
        const uint32_t mask = take<uint32_t>(p);
        e.category = static_cast<Category>(take<uint8_t>(p));
        e.lotType = take<uint8_t>(p);
        e.lotAggregate = take<uint8_t>(p);
        const uint8_t state_show = take<uint8_t>(p);
        e.geom_slot = take<int16_t>(p);
        e.name_suffix = take<int16_t>(p);
        const uint16_t name_index = take<uint16_t>(p);
        (void)take<uint16_t>(p); // pad
        e.lotId = take<uint32_t>(p);
        e.real_posX = take<float>(p);
        e.real_posZ = take<float>(p);
        e.state_flag = take<uint32_t>(p);
        // An unknown polarity is read as no rule: showing a marker is the recoverable mistake.
        e.state_show = (e.state_flag != 0 && state_show <= static_cast<uint8_t>(StateShow::WhileOff))
                           ? static_cast<StateShow>(state_show)
                           : StateShow::Always;
        // Index 0 is the empty name: a row with no MSB object, which is most of them.
        e.object_name = (name_index != 0 && name_index < g_names.size())
                            ? &g_names[name_index]
                            : nullptr;

        for (size_t f = 0; f < kFieldCount; ++f)
        {
            if (!(mask & (1u << f)))
                continue;
            if (p + 4 > end)
            {
                spdlog::error("[mapdata] the packed table ends mid-row at {} of {}; no markers",
                              i, count);
                g_entries.clear();
                return false;
            }
            if (kFields[f].kind == FieldKind::F32)
                kFields[f].apply(e.data, 0, take<float>(p));
            else
                kFields[f].apply(e.data, take<uint32_t>(p), 0.0f);
        }
        g_entries.push_back(e);
    }

    MAP_ENTRIES = g_entries.data();
    MAP_ENTRY_COUNT = g_entries.size();
    size_t ruled = 0;
    for (const MapEntry &e : g_entries)
        if (e.state_show != StateShow::Always) ++ruled;
    spdlog::info("[mapdata] {} markers expanded from {} KB packed ({} KB raw), {} of them shown by "
                 "world state",
                 MAP_ENTRY_COUNT, MAP_BLOB_SIZE / 1024, MAP_BLOB_RAW_SIZE / 1024, ruled);
    return MAP_ENTRY_COUNT != 0;
}

} // namespace goblin::generated

#pragma once

// The enemy names and the English item-name fallback, unpacked at startup from compressed blobs
// instead of compiled in.
//
// Both used to be arrays of wide string literals: 520 enemies in fifteen languages and ~2500
// English item/place names, about 370 KB of UTF-16 text in every non-ERR DLL. What ships now is
// the same text packed and deflated as byte arrays - the map table's pattern (goblin_map_blob.hpp),
// inflated with the same miniz. load_name_tables() expands both once, early in setup, into heap
// arrays that ENEMY_NAMES / ITEM_NAME_FALLBACK then point at, so every reader keeps indexing and
// binary-searching exactly as before.

#include "goblin_enemy_names.hpp"
#include "goblin_item_fallback.hpp"

namespace goblin::generated
{

// Expand both blobs. Idempotent; call it before anything reads either table. A table that is
// missing or will not inflate stays at count 0 - the mod then runs without those names (markers
// keep their generic labels) rather than with half a table - one log line names it, and this
// returns false.
bool load_name_tables();

} // namespace goblin::generated

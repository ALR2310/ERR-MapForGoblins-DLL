#pragma once

// Hand-maintained declarations. Each profile's baked src/generated*/goblin_enemy_names.cpp defines
// ENEMY_NAME_LANGS and the packed table; src/goblin_names_blob.cpp expands that into ENEMY_NAMES.

#include <cstddef>
#include <cstdint>

namespace goblin::generated
{
    // Localized enemy names used by the non-ERR builds to label enemy-drop
    // markers (the marker's enemy-name textId is `id + 900000000`). The ERR
    // build ships an empty table and resolves names from its own runtime data
    // instead. Strings are FromSoft / community-wiki enemy names.
    constexpr int ENEMY_NAME_LANG_COUNT = 15;

    // msgbnd language codes, in the order the per-language `names[]` are stored.
    // Index 0 is "engus" and is used as the fallback. (Definition in the .cpp.)
    extern const char *const ENEMY_NAME_LANGS[ENEMY_NAME_LANG_COUNT];

    struct EnemyName
    {
        int32_t id;                                    // marker textId = id + 900000000
        const wchar_t *names[ENEMY_NAME_LANG_COUNT];   // by ENEMY_NAME_LANGS index
    };

    // Filled by goblin::generated::load_name_tables() (src/goblin_names_blob.cpp) at startup. A
    // POINTER, not an array: every use is ENEMY_NAMES[i] or ENEMY_NAMES + n, which read the same.
    extern size_t ENEMY_NAME_COUNT;
    extern const EnemyName *ENEMY_NAMES;  // sorted ascending by id
}

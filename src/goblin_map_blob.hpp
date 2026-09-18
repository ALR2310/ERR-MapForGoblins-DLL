#pragma once

// The baked map table, unpacked at startup from a compressed blob instead of compiled in.
//
// It used to be a `const MapEntry[]` literal: 7376 brace initialisers, 3.34 MB of generated C++ per
// profile that MSVC parsed on every build of all nine, and 2.14 MB of .rdata in the shipped DLL -
// about 30% of the binary. Most of each 304-byte record was zeros: the param struct is 256 bytes
// and only 20 of its fields are ever set.
//
// What ships now is the same data packed to ~40 bytes a row and deflated, as a byte array - the
// pattern the icon tags already use (src/generated_shared/goblin_map_icons.cpp, inflated with the
// miniz that is already linked in). load() expands it once, early in setup, into heap arrays that
// MAP_ENTRIES/MAP_ENTRY_COUNT then point at, so every consumer keeps indexing exactly as before.
//
// MAP_ENTRIES is a POINTER now, not an array. Every use is `MAP_ENTRIES[i]`, which is unchanged;
// nothing takes sizeof it.

#include "goblin_map_data.hpp"

namespace goblin::generated
{

// Expand the blob. Idempotent, and safe to call before anything reads the table; returns false if
// the blob is missing or will not inflate, in which case MAP_ENTRY_COUNT stays 0 and the mod runs
// with no markers rather than with half a table.
bool load_map_data();

} // namespace goblin::generated

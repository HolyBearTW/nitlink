#pragma once

// Game database for NitLink's manual game selector.
//
// This is a static, compiled-into-the-binary list of well-known PS5 titles.
// Each entry has:
//   - id           : stable internal key (e.g. "spider-man-2"). Used as the
//                    map key for per-game settings AND as the Discord Rich
//                    Presence art asset key (must match the asset uploaded
//                    on discord.com/developers/applications → Art Assets).
//   - title        : human-readable display name shown in the dropdown.
//
// Why static and not a JSON file on disk?
//   - Zero file I/O, zero parse cost on startup.
//   - Can't be corrupted or deleted by the user.
//   - Ships in the .exe: no external asset to forget.
//   - Roughly 100 entries x ~50 bytes each = ~5KB. Negligible.
//
// Adding games:
//   - Edit the kBuiltInGames table in game_database.cpp.
//   - Use lowercase-hyphenated ids that match Discord asset key rules:
//     2..32 chars, [a-z0-9_-] only. The Discord asset upload validates
//     this automatically: if it accepts the key when you upload the cover art,
//     it's a valid id.
//
// Eventually:
//   - Pro tier will add PSN API-driven detection and auto-train hashes
//     of the dashboard art. At that point this list becomes the *fallback*
//     for users without PSN connected. For v1.0 it's the primary mechanism.

#include <string>
#include <string_view>
#include <vector>

namespace NitLink {

struct GameEntry {
    std::string id;         // e.g. "spider-man-2"     (Discord asset key)
    std::string title;      // e.g. "Marvel's Spider-Man 2"
};

// Returns the built-in catalog. Order is curated so the most popular titles
// surface first when the user just clicks the dropdown without typing.
const std::vector<GameEntry>& GetGameDatabase();

// Returns entries whose title contains `query` as a case-insensitive
// substring. Empty query returns the full catalog. Max 50 results so the
// dropdown stays snappy.
std::vector<const GameEntry*> SearchGames(std::string_view query);

// Look up by id. Returns nullptr if not found.
const GameEntry* FindGameById(std::string_view id);

} // namespace NitLink

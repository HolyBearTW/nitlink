#include "game_database.h"

#include <algorithm>
#include <cctype>

namespace NitLink {

// Curated PS5 game catalog. Order matters: most popular / most likely to be
// played by the launch audience surface first. The dropdown shows all of
// these when the search box is empty.
//
// IDs must be:
//   - lowercase
//   - hyphen-separated (no spaces, no underscores)
//   - 2..32 chars
//   - matchable as Discord Rich Presence art asset keys
//
// Titles are the official display name as shown on the PS Store, with
// the apostrophe stripped where it appears in possessives ("Marvels"
// instead of "Marvel's") because Discord's "details" field renders
// straight strings (apostrophes are fine, but consistency is easier
// to maintain).
static const std::vector<GameEntry> kBuiltInGames = {
    // First-party PS5 exclusives: these are the showcase titles
    { "astros-playroom",       "Astro's Playroom"                    },
    { "astro-bot",              "Astro Bot"                           },
    { "spider-man-2",          "Marvel's Spider-Man 2"               },
    { "spider-man-miles",      "Spider-Man: Miles Morales"           },
    { "god-of-war-ragnarok",   "God of War Ragnarok"                 },
    { "horizon-forbidden-west","Horizon Forbidden West"              },
    { "gran-turismo-7",        "Gran Turismo 7"                      },
    { "ratchet-rift-apart",    "Ratchet & Clank: Rift Apart"         },
    { "returnal",              "Returnal"                            },
    { "demons-souls",          "Demon's Souls"                       },
    { "ghost-of-tsushima",     "Ghost of Tsushima Director's Cut"    },
    { "ghost-of-yotei",        "Ghost of Yotei"                      },
    { "tlou-part1",            "The Last of Us Part I"               },
    { "tlou-part2",            "The Last of Us Part II Remastered"   },
    { "death-stranding-2",     "Death Stranding 2: On the Beach"     },
    { "stellar-blade",         "Stellar Blade"                       },
    { "saros",                 "Saros"                               },

    // Major third-party AAA
    { "ff7-rebirth",           "Final Fantasy VII Rebirth"           },
    { "ff7-remake",            "Final Fantasy VII Remake Intergrade" },
    { "ff16",                  "Final Fantasy XVI"                   },
    { "elden-ring",            "Elden Ring"                          },
    { "elden-ring-shadow",     "Elden Ring: Shadow of the Erdtree"   },
    { "black-myth-wukong",     "Black Myth: Wukong"                  },
    { "baldurs-gate-3",        "Baldur's Gate 3"                     },
    { "cyberpunk-2077",        "Cyberpunk 2077"                      },
    { "hogwarts-legacy",       "Hogwarts Legacy"                     },
    { "alan-wake-2",           "Alan Wake 2"                         },
    { "starfield",             "Starfield"                           },
    { "diablo-4",              "Diablo IV"                           },
    { "lies-of-p",             "Lies of P"                           },
    { "wo-long",               "Wo Long: Fallen Dynasty"             },
    { "armored-core-6",        "Armored Core VI: Fires of Rubicon"   },
    { "monster-hunter-wilds",  "Monster Hunter Wilds"                },
    { "monster-hunter-rise",   "Monster Hunter Rise"                 },
    { "dragons-dogma-2",       "Dragon's Dogma 2"                    },
    { "rise-of-the-ronin",     "Rise of the Ronin"                   },
    { "like-a-dragon-8",       "Like a Dragon: Infinite Wealth"      },
    { "yakuza-pirate",         "Like a Dragon: Pirate Yakuza in Hawaii" },

    // Resident Evil family
    { "re4-remake",            "Resident Evil 4"                     },
    { "re-village",            "Resident Evil Village"               },
    { "re2-remake",            "Resident Evil 2"                     },
    { "re3-remake",            "Resident Evil 3"                     },

    // Fighting games (FGC matters at launch: frame-perfect latency)
    { "tekken-8",              "Tekken 8"                            },
    { "street-fighter-6",      "Street Fighter 6"                    },
    { "mortal-kombat-1",       "Mortal Kombat 1"                     },
    { "guilty-gear-strive",    "Guilty Gear Strive"                  },

    // Multiplayer / competitive
    { "cod-bo6",               "Call of Duty: Black Ops 6"           },
    { "cod-mw3",               "Call of Duty: Modern Warfare III"    },
    { "cod-warzone",           "Call of Duty: Warzone"               },
    { "fortnite",              "Fortnite"                            },
    { "apex-legends",          "Apex Legends"                        },
    { "valorant",              "Valorant"                            },
    { "rainbow-six-siege",     "Tom Clancy's Rainbow Six Siege"      },
    { "destiny-2",             "Destiny 2"                           },
    { "marvel-rivals",         "Marvel Rivals"                       },
    { "overwatch-2",           "Overwatch 2"                         },
    { "rocket-league",          "Rocket League"                       },

    // Sports
    { "fc-25",                  "EA Sports FC 25"                     },
    { "fc-26",                  "EA Sports FC 26"                     },
    { "madden-25",              "Madden NFL 25"                       },
    { "nba2k25",                "NBA 2K25"                            },
    { "mlb-the-show-24",        "MLB The Show 24"                     },
    { "ufc-5",                  "UFC 5"                               },
    { "f1-24",                  "F1 24"                               },

    // Open world / sandbox
    { "gta-5",                  "Grand Theft Auto V"                  },
    { "rdr2",                   "Red Dead Redemption 2"               },
    { "ac-shadows",             "Assassin's Creed Shadows"            },
    { "ac-mirage",              "Assassin's Creed Mirage"             },
    { "ac-valhalla",            "Assassin's Creed Valhalla"           },
    { "watch-dogs-legion",      "Watch Dogs: Legion"                  },
    { "cyberpunk-pl",           "Cyberpunk 2077: Phantom Liberty"     },

    // Souls-likes / action RPG
    { "sekiro",                 "Sekiro: Shadows Die Twice"           },
    { "dark-souls-3",           "Dark Souls III"                      },
    { "nioh-2",                 "Nioh 2"                              },
    { "stranger-of-paradise",   "Stranger of Paradise"                },

    // Indie / mid-tier loved by enthusiasts
    { "sifu",                   "Sifu"                                },
    { "stray",                  "Stray"                               },
    { "kena",                   "Kena: Bridge of Spirits"             },
    { "deathloop",              "Deathloop"                           },
    { "sackboy",                "Sackboy: A Big Adventure"            },
    { "ghostwire",              "Ghostwire: Tokyo"                    },
    { "hi-fi-rush",             "Hi-Fi Rush"                          },
    { "control",                "Control: Ultimate Edition"           },

    // Racing
    { "forza-horizon-5",        "Forza Horizon 5"                     },
    { "wrc-24",                 "EA Sports WRC"                       },

    // Korean / niche
    { "crimson-desert",         "Crimson Desert"                      },
    { "blue-protocol",          "Blue Protocol"                       },
    { "the-day-before",         "The Day Before"                      },

    // VR (PSVR2)
    { "gt7-vr",                 "Gran Turismo 7 (PSVR2)"              },
    { "horizon-cotm",           "Horizon Call of the Mountain"        },
    { "re-village-vr",          "Resident Evil Village (PSVR2)"       },
    { "re4-vr",                 "Resident Evil 4 (PSVR2)"             },

    // Fallback / catch-all
    { "other",                  "Other / Custom Game"                 },
};

const std::vector<GameEntry>& GetGameDatabase() {
    return kBuiltInGames;
}

static char ToLowerASCII(char c) {
    // Locale-independent: guards against weird Windows locale settings.
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static bool ContainsICase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (ToLowerASCII(haystack[i + j]) != ToLowerASCII(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

std::vector<const GameEntry*> SearchGames(std::string_view query) {
    std::vector<const GameEntry*> results;
    results.reserve(50);

    for (const auto& g : kBuiltInGames) {
        if (ContainsICase(g.title, query)) {
            results.push_back(&g);
            if (results.size() >= 50) break;
        }
    }

    return results;
}

const GameEntry* FindGameById(std::string_view id) {
    for (const auto& g : kBuiltInGames) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

} // namespace NitLink

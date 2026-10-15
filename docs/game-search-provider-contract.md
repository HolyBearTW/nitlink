# Games, artwork and preview integration

## Review entry point

Run `scripts/open-game-preview.ps1`, or open
`http://127.0.0.1:8765/nitlink-menu.html?preview=games` while the host is running.
The preview uses the actual HTML, menu.js and three locale resources. HTTP preview
defaults to Real API. Full, 420px Docked, language and data source are selectable.
Explicit `&mode=fixture` selects the offline catalogue; direct-file preview only
supports Fixture. An actual Windows WebView2 bridge always takes precedence.

Real API community games and covers come from the public Worker v5 at
`https://holybear-game-api.nitlink.workers.dev`. The controlled localhost host
performs HTTPS requests and serves validated local thumbnails. HDMI, capture
hardware and presence state in this browser preview remain simulated. The actual
Nintendo Taiwan provider supplements the catalogue and artwork separately. Browser
checks do not validate the unbuilt Windows integration or Discord visual layout.

## Catalogue and pagination

`/games/search?q=...&lang=...` receives NitLink's effective locale, including when
the preference is system. SteamGridDB id identifies the game. displayName appears
above the preserved original name. Input uses a 350ms debounce and stale request
cancellation. All validated records appear in a scrolling list with source labels.
The native parser rejects responses above 1000 records rather than truncating
them; JSON is capped at 256 KiB.

Observed on 2026-10-15: real 薩爾達 search returned 27 records. It did not include
Tears of the Kingdom (5303117); searching the full English title finds it. Missing
fixture entries are never injected into real results. Insufficient primary
responses may use optional expansions in external game-search-expansions.json.
A sufficient primary response is preserved. The official artwork provider adds
the missing Tears of the Kingdom as a separately labelled local mapping, so the
current combined result contains 28 games: 27 Worker records plus one official
mapping. It never claims that mapping was returned by the Worker. No game list is
compiled into the EXE; future Worker improvements need no UI update.

Selecting a new game opens `/games/<id>/covers?shape=square|portrait|all&page=0`.
page, hasMore and total drive paging. Every image in a response page is available.
Shape buttons change the community-image request and filter official candidates
by their verified shapes. Official artwork has a compact first group, independent
of community page number. Loaded image dimensions also enforce gallery filtering. A manual choice
always wins; automatic selection prefers official art and then community art.
Native search optionally enriches eight thumbnails and the browser host twelve;
both retain all result metadata. Search caches for 10 minutes and covers for 30.

## Language and genuine localized art

Worker v5 returns language:null for the inspected covers. Artwork details display Unknown.
Missing metadata does not establish that localized covers do not exist. Filename,
author and translated game titles never count as image-language evidence.
Future verified language ranking accepts all three validated fields:

```json
{"language":"zh-TW","languageVerified":true,"languageSource":"verified upstream identifier"}
```

Matching verified language ranks first, then untagged covers, then other verified
languages. Manual choices win. Author and actual image orientation are separate.

Genuine Traditional Chinese publisher art was visually confirmed:

- [Age of Calamity](https://www.nintendo.com/tw/switch/zelda-yakusai/products/) and
  [its actual box art](https://www.nintendo.com/tw/switch/zelda-yakusai/img/product/package.jpg).
- [Tears of the Kingdom](https://www.nintendo.com/tw/zelda/totk/products/soft.html) and
  [its actual box art](https://www.nintendo.com/tw/zelda/totk/products/img/soft/img__package.png).

All 66 Age of Calamity candidates returned by the Worker were inspected without
finding a clearly Traditional Chinese image. This is a bounded observation about
that response, not all of SteamGridDB or other games. Some images are successful
HTTP responses containing takedown placeholders. Exact fingerprints now reject
the inspected notices; unknown future notice variants remain a limitation.

The Nintendo Taiwan Artwork Provider is implemented in native code and in the
controlled browser host. Both read `official-artwork.json`, a versioned seed file
with SteamGridDB identity, names, search terms and artworkSources. Each candidate
has provider, sourceLabel, language, region, isOfficial, kind, shape and sourcePage.
The provider interface supports catalogue lookup and separate artwork candidates;
the MVP does not crawl the website. Initial mappings are:

| Game ID | Game | Official material |
| --- | --- | --- |
| 5303117 | Tears of the Kingdom | Traditional Chinese box art |
| 5267007 | Age of Calamity | Traditional Chinese box art |
| 5267007 | Age of Calamity | Verified 1920×1080 Traditional Chinese key art on Nintendo eShop CDN |
| 5483959 | Age of Imprisonment | Traditional Chinese Logo, not box art |

Taiwan sourceLabel is `Official Nintendo Taiwan`, language `zh-TW`, region `TW`,
isOfficial true. The eShop candidate is labelled `Official Nintendo eShop CDN`,
with the user-supplied HK region context; the Hong Kong product-page association
was not independently verified. The logo URL is
`https://www.nintendo.com/tw/games/switch2/aagab/assets/img/kv_logo.png`.
Fixture caches validated, normalized copies of the same genuine images for offline use; regenerate
those copies with `python scripts/prepare-game-preview.py --official-only`.

The gallery presents images, blue selection borders and checks, verified official
Chinese badges, and Logo markers. Attribution, language, preview dimensions and
provenance are in a keyboard-accessible information dialog. API diagnostics only
appear in developer previews. The wide gallery has 5-6 columns and Docked adapts
to available width. Single-page controls are hidden.

Search result and library images keep their existing DOM nodes during selection,
rename, favorites and state synchronization. Cancelled enrichment settles its
placeholders immediately; gallery batches and actual local image loads have
bounded timeouts. Reselecting the same game does not request the gallery again.

Synthetic language samples require an explicit checkbox and remain clearly marked.
They never become an automatic cover, even with the option enabled. Old automatic
synthetic choices migrate to genuine cached art without losing names, favorites,
recent history or activity time. Manual user choices remain intact. Fixture has
eight Zelda titles and Sonic/Mega Man examples; it is not real API evidence.

## Persistence and presence

Native Config stores each game's original/localized/custom names, chosen cover
URL/thumb/id, provider/sourceLabel/language/region/isOfficial/kind,
automatic/manual state, favorite and last-use time. Existing
current_game and game.<key> quality settings remain compatible. Unfavoriting does
not delete quality settings or recent history. Startup restores saved identity
without catalogue search and checks validated local artwork cache first.

Localhost preview uses separate Fixture/Real API host JSON profiles under
out/game-preview-api. Checks use profile=checks, separate from the review profile.
This exercises persistence through a host substitute, not Windows Config. Only
direct-file Fixture uses localStorage.

Presence name uses the effective HDMI label, details the selected game, and state
confirmed signal resolution/FPS plus reliable HDR/VRR. Unknown HDMI identity falls
back to NitLink. Capture pixel formats are omitted. Changing game identity starts
a new timer; cover, language, source, signal and settings synchronization preserve
it. Reopening the UI cannot restart it.

Existing IPC SET_ACTIVITY was exercised against the local Discord client using
two names and the same timestamp. Both names were echoed in accepted replies.
The probe cleared its own activity. Bounded evidence: out/discord-name-probe.json.
No Social SDK was added. Native acknowledgements control support status and
fallback. All three approved Nintendo URLs were additionally exercised against
the actual IPC client, which accepted them and resolved `mp:external/...` assets
(out/discord-artwork-probe.json). The native path now passes these URLs after
local validation. ACK changes refresh the UI independently of activity updates.
Rejected assets fall back with an explicit reason; failed local images cannot
be selected. The browser labels the separate, URL-specific probe evidence and
never claims to be connected to Discord. Name/image URL acceptance is
not client visual proof. 84px and 64px
are explicitly local preview sizes, not measured Discord sizes.

The TOTK publisher image has explicit, bounded whitespace crop coordinates in
the provider seed. WIC and Pillow apply the same crop before caching; no box art
is stretched or relabelled square. Local activity previews contain the whole image
in a square. Discord receives the original public URL, so its cropping is unverified.
HTTP/decode failures are excluded from selection. Exact SHA-256 fingerprints in
unavailable-artwork.json additionally reject inspected HTTP 200 takedown notices;
new upstream notice variants are not assumed detectable. Cache version v3 prevents
reuse of older normalized placeholders or uncropped copies.

## Resource boundaries and verification stage

The general Hong Kong store source is documented in `NINTENDO-STORE-REVIEW.md`.
It reads only fixed public search/category pages on store.nintendo.com.hk, with
2 MiB HTML limits and real 24-item pagination. Only published HTTPS image paths
under that host's `/media/catalog/product/` are accepted. It requires no game
list, does not execute page JavaScript and does not bypass access controls.
The renderer submits a provider enum and query, never a host or image URL.
Unknown image languages remain unknown. Generic catalogue products need explicit
selection so same-name editions are not silently applied to another game.

Native networking accepts only the fixed Worker and
https://cdn2.steamgriddb.com/{grid,thumb}/<32 lowercase hex>.(jpg|png|webp), with an
optional legacy trailing ?. Official downloads additionally permit HTTPS on
www.nintendo.com and www.nintendo.com.tw, with strict ASCII image paths and exact
membership in the validated official mapping. Credentials, ports, query strings,
fragments, traversal, encoded paths and redirects are rejected. Saved approved
art can use its validated disk cache even if a later mapping removes its URL.
No renderer message accepts arbitrary URLs. No API key is requested or shipped. Redirects/cookies
are disabled. API requests use 12-second deadlines; images 8 seconds / 8 MiB.
WIC validates format, frame count and dimensions, strips metadata and emits PNG
thumbnails up to 512px / 2 MiB. Inputs above 4096px per axis / 8M pixels fail.

Memory artwork cache: 128 images / 64 MiB, with current/source images pinned and
cache hits promoted, keeping a complete 50-cover page available for selection.
Native disk cache under
%LOCALAPPDATA%/NitLink/GameArt uses SHA-256 names and 96 files / 32 MiB. The WebView
sees opaque local resources only. connect-src 'none', img-src 'self' and denied
unknown resources remain enforced; no virtual folder mapping is added.

Evidence consists of JavaScript/Python syntax checks, interactive browser checks,
real Worker responses and the separate IPC compatibility probe. No Windows build,
CTest or capture/Discord visual validation has run. Full build/CTest remain
deferred until user approval. No commit, push or PR is part of this stage.

# Official artwork and square-cover review

The newer general Hong Kong store provider, source priority and catalogue
pagination are described in `NINTENDO-STORE-REVIEW.md`. The four original
materials below remain curated fallbacks. Ocarina of Time now also has two
verified text-free original-game artworks from Nintendo's history page.

Open `http://127.0.0.1:8765/nitlink-menu.html?preview=games` and refresh the page.
If local web protection interrupts document loading, use
`scripts/open-game-preview.ps1`. That review browser supplies exact workspace
HTML/scripts under the same URL and original CSP; API/images still use the local
bridge. It does not disable protection or use a separate mockup.

Search 薩爾達, choose **Hyrule Warriors: Age of Calamity**, and open **All** covers
if the library restored a previously saved choice. Select the official portrait,
then **Adjust cover**. Fit keeps the entire image over a blurred background.
Fill/crop supports dragging, arrow keys and zoom. Original retains the selected
source proportions without an additional editor crop. Preview at 84/64px, save,
switch games, switch back and reload. Each game's recipe and files are retained.

The original mode uses the validated provider image: metadata is stripped and
large images are resized to at most 512px. TOTK's known source-side whitespace
normalization remains disclosed in artwork details. It is not a claim that the
remote file has different original dimensions.

## Verified provider data

| Game | Material | Actual remote dimensions | Provider |
| --- | --- | --- | --- |
| Tears of the Kingdom | Box art inside side whitespace | 247×197; validated box area 121×197 | Nintendo Taiwan |
| Age of Calamity | Traditional Chinese box art | 280×456 | Nintendo Taiwan |
| Age of Calamity | Traditional Chinese landscape key art | 1920×1080 JPEG | Nintendo eShop CDN |
| Age of Imprisonment | Traditional Chinese logo | 967×658 before normalization | Nintendo Taiwan |

The added eShop image was fetched from the exact user-supplied URL, returned
HTTP 200 / image/jpeg, and its title was visually checked. It is `keyArt`,
`language: zh-TW`, `region: HK`, `provider: nintendo-hk-eshop`; it is not labelled
as Taiwan material or a native square icon. The HK region reflects the supplied
candidate context; an association with a Hong Kong product page was not separately
verified. All four material kinds (`boxArt`,
`squareIcon`, `keyArt`, `logo`) are supported. No native square icon has been
verified or added. No crawling or Worker v5 changes were made.

Original attribution stays in artwork details. Adjusted art is labelled
“Made from official artwork”, never an official native square. `isOfficial`
describes the original provider candidate; `artworkMode` describes local edits.

## Native implementation and persistence

`artwork_renderer.cpp` uses WIC for bounded raster decoding/PNG encoding and CPU
composition on the existing background game-service worker. Fit uses proportionate
foreground art over a blurred, darkened enlargement; crop uses bounded x/y/zoom.
Fit/crop output real 512×512 PNGs, original mode a validated source PNG.
The WebView submits IDs and bounded numbers, never image bytes, URLs or paths.

Config retains original URL/provider/type and `artwork_mode`, x/y/zoom,
`artwork_source`, `artwork_final`, plus the derived Discord availability rule.
Hash identities resolve to `%LOCALAPPDATA%\NitLink\CustomGameArt\<sha256>.png`.
Original and final copies are durable and excluded from ordinary thumbnail LRU
eviction. No chosen image is deleted if its URL expires. Canonical filenames,
HTTPS/provider membership, MIME/decode limits, link checks and bounded storage
remain enforced. An image-storage failure leaves the previous choice in place.

Browser review uses the real UI and bridge protocol, Pillow output under
`out/game-preview-api/generated`, and host JSON profiles. It is not Windows Config
validation. A focused test also restores/edits saved artwork while all API image,
search and cover requests are blocked. Preview activation saves the record and
current selection atomically before displaying the confirmed selection, so quick
switch/reload cannot leave a displayed choice unwritten.

## Discord: completed path and explicit limit

All four original HTTPS URLs passed a bounded live `SET_ACTIVITY` IPC probe.
Discord returned `mp:external/...` assets and the same timestamp. The probe used
its own PID and cleared its activity. This proves IPC acceptance, not client pixels
or a new Windows EXE. Evidence: `out/discord-artwork-probe.json`.

Edited local PNGs have no public HTTPS URL. NitLink sends no `file://` or local
path, performs no upload and does not claim the edited picture is on Discord.
The 84/64px adjusted-image preview is explicitly labelled local only. A separate
original candidate and its actual RPC status remain visible. Native RPC continues
using the original allowed HTTPS URL and preserves the game's timestamp.

### Reviewable hosting proposal — not implemented or deployed

Use a separate upload service and storage controlled by the user, leaving Worker
v5 unchanged. Publish only images the user has approved sharing. Require a short
lived, scoped upload credential stored in Windows Credential Manager; embed no
write secret in the EXE. Each upload is an explicit action after reviewing the
512px result and attribution. Send only the PNG and necessary source attribution,
without the game library, hardware identity or play history.

The service validates PNG signature/dimensions/size and sanitizes pixels before
storage. It returns an immutable HTTPS URL on a configured owned host with a
canonical hash path. Reads must be public for Discord; upload credentials are
never part of that URL. Support explicit deletion and retention controls.
Only a successful upload followed by Discord's acknowledgement may change the
game's edited-artwork status from local-only to accepted. Actual client rendering
still needs visual verification. An owner of the Discord application may instead
manually publish a reviewed image as an application asset; no automatic Portal
upload is included.

## Validation boundary

Browser/JS/Python, public-image validation and isolated IPC checks only.
No full Windows build, no CTest, no commit, push or PR. Native code and regression
test sources are implemented but remain uncompiled/unexecuted until UI approval.

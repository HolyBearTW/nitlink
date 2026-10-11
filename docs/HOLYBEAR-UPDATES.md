# Optional HolyBear update checks

F1 → About shows the running HolyBear version once, a compact update control,
and easy-to-find feedback/support links. The update section shows the last check
time in the selected language's date format, or “Not yet checked.” A newer
version is displayed without the leading `v`; its release link retains the
actual GitHub tag. Technical information and contributor credits have separate
keyboard-accessible disclosures, both expanded by default. Technical details,
the community edition notice and source/license links stay in the first;
the original contributor names and links stay in the second. The shared
capture/signal strip is hidden only while About is selected and returns on the
other tabs. English, Traditional
Chinese and Simplified Chinese use the existing localization and settings
layout. An unread badge is cleared when About is opened; no startup popup, toast
or forced update is used.

Automatic checks are **off by default**. Enabling them allows a background
check on startup and when enabled in F1, at most once per 24 hours after the
last attempt (including failed attempts). Manual checks bypass this interval,
but concurrent requests are rejected. Disabling the toggle cancels a pending
check and prevents subsequent automatic requests. Settings and the last viewed
release tag are saved with the application's existing atomic configuration.

All HTTP and JSON work runs on an owned C++ worker, using WinHTTP and Windows'
JSON implementation. The capture/Present loop only polls a completion flag;
it never waits for the network. Only this unauthenticated HTTPS GET is allowed:

```text
https://api.github.com/repos/HolyBearTW/nitlink/releases/latest
```

The request sends a generic NitLink-HolyBear user agent and GitHub JSON Accept
header, with no token, configuration, device identifiers, game data or telemetry.
GitHub sees the usual network address/request metadata; Windows may use the
system-configured proxy. Requests have phase timeouts, a 15-second total wait
budget and a 256 KiB response limit. Redirects are disabled and Windows' normal
TLS certificate checks remain enabled. Failures display a short F1 status and
never affect capture or open an error window. Connection/DNS/timeouts are
reported as connection failures. HTTP 404 means no published latest-release
record was found; HTTP 429 is a temporary request limit. HTTP 403 is called
rate-limited only when the response includes `X-RateLimit-Remaining: 0`;
otherwise the UI reports a generic API failure. GitHub 5xx responses are server
outages, and malformed JSON or release fields are rejected as unverifiable
version information. No token or account data is sent, and no response body or
release text is inserted into HTML.

Only a typed, non-draft, `prerelease=false` release from HolyBearTW/nitlink with
a valid `tag_name` and matching GitHub release URL is accepted. Version order
compares three/four numeric components, with missing revision zero, followed by
the exact `-holybear` edition suffix. `1.2.4.2-holybear > 1.2.4.1-holybear` and
`1.2.5-holybear > 1.2.4.2-holybear`. The suffix does not override GitHub's formal
release metadata. Local newer/equal versions show “up to date.”

The frontend receives only bounded validated state, rendered as text. It does
not fetch GitHub, insert API HTML, or relax WebView CSP/navigation restrictions.
A deliberate click opens only the corresponding GitHub Release page in the
browser. There is no automatic download, executable launch or installation.

## Verification

`holybear_update_checker` uses mock responses for versions, prerelease/invalid
JSON, HTTP errors, timeout, opt-in, 24-hour gating, cancellation and request
deduplication. Configuration and WebView tests cover persistence, all three
languages and displayed notifications; they do not depend on GitHub availability.

After a future official release is published, manually enable automatic checks
or press Check for Updates in an older version. Verify the newer tag, localized
message and correct release-page link. Opening About marks it read; reopening
the application must not re-show the unread badge for that same tag. Turn the
toggle off to restore offline startup behavior. A failed API call remains a
non-disruptive status; it is not proof that the installed version is current.

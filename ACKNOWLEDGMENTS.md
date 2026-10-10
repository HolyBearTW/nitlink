# Acknowledgments

HolyBear Edition includes CN_115's low-latency audio and Simplified Chinese
work from [CN115/nitlink-lowlatency v0.1.2](https://github.com/CN115/nitlink-lowlatency/tree/v0.1.2),
under the original MIT license. Exact source commits and original authors are
recorded in `docs/cn115/SOURCE.txt` and `docs/cn115/FEATURE_COMMITS.tsv`.
HolyBearTW maintains this unofficial integration and release workflow.

This file credits external work that informed NitLink's development but
is not bundled, statically linked, or otherwise redistributed in the
NitLink binary, and the people who contributed code directly. Bundled
and linked third-party software is tracked separately in `LICENSES.md`.

---

## Protocol research reference

NitLink's Elgato 4K S HID tone-mapping control was implemented with
reference to public protocol research from:

- 13bm/elgato4k-linux
- https://github.com/13bm/elgato4k-linux
- Upstream license: GPL-3.0 (GNU General Public License v3.0)

No source code from elgato4k-linux is included in NitLink. The project
was used only as a protocol reference for the Elgato 4K S HID report
format and tone-mapping control behavior. NitLink's Windows HID
implementation in `src/capture/elgato_hid_4ks.{h,cpp}` is independent
and written from scratch against the Win32 SetupAPI and HID Class API.

For the engineering context (why two card-specific control paths exist,
the byte layout of the HID Output Report, and the implemented HDR/SDR
state machine), see [`docs/4ks-hdr-tonemap.md`](docs/4ks-hdr-tonemap.md).

---

## Contributors

Code contributions merged into NitLink:

- Nathan K. (@n810K), NitLink 1.1.0: audio endpoint recovery and
  capture-format rendering in the WASAPI audio router
  (https://github.com/nitlink-dev/nitlink/pull/5). The router now
  survives default playback device changes, rebuilds a lost endpoint
  on its worker thread, and plays through playback devices whose sample
  rate or channel layout differs from the capture card's.

- HolyBear (聖小熊), [@HolyBearTW](https://github.com/HolyBearTW), NitLink 1.2.2 and 1.2.3:
  HDR-to-SDR tone mapping, Traditional Chinese localization, GC553Pro
  capture and automatic HDR source detection, native fractional frame rates,
  startup and placeholder handling, custom No Signal images, settings
  responsiveness, and GC553Pro hardware validation
  ([#9](https://github.com/nitlink-dev/nitlink/pull/9),
  [#11](https://github.com/nitlink-dev/nitlink/pull/11),
  [#12](https://github.com/nitlink-dev/nitlink/pull/12),
  [#14](https://github.com/nitlink-dev/nitlink/pull/14),
  [#16](https://github.com/nitlink-dev/nitlink/pull/16),
  [#19](https://github.com/nitlink-dev/nitlink/pull/19),
  [#20](https://github.com/nitlink-dev/nitlink/pull/20),
  [#21](https://github.com/nitlink-dev/nitlink/pull/21)).

---

## Localization contributors

Localization contributions are credited by language and do not imply
project authorship or maintainership.

- **Traditional Chinese (zh-TW):** [HolyBear（聖小熊）](https://github.com/HolyBearTW)
  (`@HolyBearTW`) — translation and localization review with real-device
  validation.

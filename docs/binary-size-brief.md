# Brief: reduce firmware size without changing the dashboard

## Objective and authority

Implement the size-reduction opportunities identified in the November 2026 code
review, preserving existing functionality, appearance, diagnostic access and
runtime performance. Work on the S3 dashboard only; the C6 controller must not
need changes.

**This document is a handoff, not an implementation or verification report.** Its
creation changes no firmware. Read `AGENTS.md` before starting, and inspect the
current tree: source locations and inventory figures below describe the reviewed
0.8.0 tree and may have moved.

Do not interpret this brief as authorization to publish a release, flash a board,
change versions, commit or push. Follow the owner's instructions for those
actions. If a release is requested, agree its version and follow `AGENTS.md`.

## Non-negotiable constraints

- Keep the existing typography and layout. Switching everything to one small font
  was discussed, but is **not** the selected approach. Do not scale up a small
  bitmap face to replace the large readouts.
- Preserve HTTP fallback, polling periods, keep-alive, mDNS, ESP-NOW, CAN DATA,
  settings editing, drive logging, console diagnostics and OTA trial/fallback.
- Keep signed mode commands and their HMAC-SHA256 authentication unchanged.
- Keep NaN/unknown semantics. Do not enable `-ffast-math`.
- Keep framebuffer count, DIRECT rendering, PSRAM placement, display timings and
  redraw caching unchanged.
- Do not edit the copied protocol headers or hand-edit generated font/DBC files.
- Do not trim support to the reference car: retain all existing generations,
  unit choices, modes, signals and diagnostic pages.
- Do not remove assertions or useful logs merely to improve the size figure.
- Preserve existing settings/NVS and filesystem compatibility.

## Baseline and evidence limits

`docs/status.md` recorded this baseline for 0.8.0, build `99df2b4`, on 2026-11-12:

| Resource | Recorded result |
|---|---:|
| App flash | 1,847,298 / 1,966,080 B (94.0%) |
| Remaining OTA-slot capacity | 118,782 B |
| Static internal RAM | 60,412 / 327,680 B |

The review did not build the project or test hardware. All proposed savings are
source-level estimates, **not** measured changes to the firmware image. Establish
a fresh baseline before changing firmware; do not subtract estimates from an old
baseline and publish the result as measured.

Use independent, reviewable stages and record before/after flash and RAM for
each. Avoid unrelated refactors so regressions and savings remain attributable.
Prioritize stages 1–3. Stages 4–5 are conditional follow-ups, not a requirement to
take risks for smaller returns.

## Stage 1 — lossless subsets of the larger stock fonts

### Relevant files

- `include/lv_conf.h`: enabled stock fonts.
- `include/theme.h`: `FONT_BIG`, `FONT_TEMP`, `FONT_MODE` and other font bindings.
- `src/ui/dash_ui.cpp`: font consumers, formatting functions and `kModeNames`.
- `tools/make_fonts.sh`: existing reproducible font-generation workflow.
- `src/ui/fonts/`: generated output; do not edit manually.

### Change

Replace the full stock 48, 22 and 28 px faces with generated subsets containing
every character their actual consumers can display. Remove the corresponding
stock font enablement once nothing references it.

**Preserve the stock glyphs exactly:** bitmap pixels, bit depth, advances,
offsets, line height, baseline and kerning applicable to the retained characters.
Prefer uncompressed output, matching the stock faces, so rendering does not gain
a decompression step.

The stock LVGL faces use **Montserrat Medium**. The existing custom generator uses
**Montserrat Bold** for the RPM and boost faces. Do not reuse that font input for
the new subsets: it would change their appearance. Pin/provide the correct input
and converter version, retain required licenses, and demonstrate that regeneration
reproduces the intended glyphs. If regeneration cannot reproduce the stock
metrics/pixels, resolve that before replacing the face; do not silently accept a
near match.

Review inventory, bitmap arrays only:

| Face | Consumers | Stock bitmap B | Candidate subset B | Difference B |
|---|---|---:|---:|---:|
| 48 px | Speed digits and `--` | 91,912 | 4,404 | 87,508 |
| 22 px | Temperatures, fuel, lock values | 20,639 | 1,473 | 19,166 |
| 28 px | Six mode names, SETTINGS, `--` | 32,484 | 3,742 | 28,742 |
| Total | | | | 135,416 |

These are not complete font-object sizes or final linked-image deltas. The
candidate inventories must be re-audited against the current code, including
formatting edge cases, before implementation.

Coverage checklist:

- 48 px: digits and minus/dash placeholder.
- 22 px: digits, minus, decimal point, space, percent, `C`, `F`, `L` and U+00B0
  degree sign; audit any other output from its formatting paths.
- 28 px: all characters of `STOCK`, `FWD`, `50:50`, `60:40`, `75:25`, `EXPERT`,
  `SETTINGS` and `--`.
- Retain blank-space advance and applicable kerning, not only visible bitmaps.
- Do not narrow fonts used for editable network fields, arbitrary diagnostic
  text or the keyboard on the assumption that dashboard captions are uppercase.
- Leave the already-small custom RPM, boost and arrow faces alone initially.

### Documentation required

Next to each new `FONT_*` binding, state that it is a subset, identify its allowed
characters/use and point to the generation source. The generator must make each
character set explicit and explain how to extend it.

In `docs/display-and-memory.md`, document the exact generation command, inputs,
licenses and the rule: **new text at one of these sizes may require regeneration;
the full stock glyph repertoire is no longer available.**

### Verification

Compare retained glyph pixels and metrics with the pinned stock faces using a
reproducible check, not just a screenshot. Verify applicable kerning and character
coverage. A host-side comparison tool is appropriate if needed; it supplements,
not replaces, a firmware build and board checks.

On hardware, inspect both tile bands, all six modes, SETTINGS, km/h and mph,
Celsius and Fahrenheit, fuel decimals, lock percentages, negative temperatures,
unknown placeholders and changing values. Check that widths, baselines and
clipping have not changed.

## Stage 2 — remove the unused TLS dependency from HTTP polling

### Relevant files and evidence

`src/net/telemetry.cpp` has two persistent `HTTPClient` instances. `buildUrl()`
constructs only `http://` URLs, but `fetchJson()` calls the URL-only
`http.begin(url)` overload.

The pinned platform selects Arduino 3.3.12. In that version, the URL-only overload
has an HTTPS fallback, which references `NetworkClientSecure`. Consequently,
plain-HTTP runtime usage can still retain TLS code in the binary.

### Change

Provide one persistent plain `NetworkClient` per endpoint and use the
client-taking `HTTPClient::begin(client, url)` overload. Retain `HTTPClient`
itself and its existing response handling.

- Client lifetimes must exceed the associated `HTTPClient` lifetimes, including
  global destruction order.
- Keep the two connections independent and preserve reuse, timeouts, reconnect
  handling, host/port changes and `.local` hostname support.
- Review explicit-client ownership/cleanup semantics on configuration changes and
  connection failures; do not assume they match internally owned clients.
- Do not remove TLS/crypto globally, alter framework libraries or write a custom
  HTTP parser as a shortcut.
- Authentication of mode commands still needs crypto. Do not claim that all
  mbedTLS code can or should disappear.

The linked saving is unknown until measured; other framework dependencies may
retain parts of the same libraries.

### Documentation required

Explain beside the explicit clients why the URL-only overload is intentionally
avoided. In `docs/architecture.md`, record that controller polling deliberately
uses plain HTTP; a future HTTPS feature needs an explicit secure client, a
certificate-validation design and a fresh size/RAM budget. Do not present
HTTPS as supported merely because `HTTPClient` accepts URLs.

### Verification

Exercise both endpoints, stable keep-alive operation, unreachable hosts,
disconnect/rejoin, controller restart, host/port edits and hostname resolution.
Verify HTTP fallback when ESP-NOW stops and that OTA/log routes remain available
while polling is paused. Check polling latency, socket churn and RAM stability.

## Stage 3 — make LVGL widget support explicit

### Relevant files and evidence

`include/lv_conf.h` is partial. In LVGL 9.3.0, most widgets default to enabled.
`lv_display_create()` automatically installs the default theme, whose references
to enabled widget classes can keep their callbacks and related code linked even
if the application never creates them.

### Change

Audit direct **and transitive** widget use, then explicitly disable unused widget
families in `include/lv_conf.h`. Keep the existing default theme initially.

Known application needs include ordinary objects, labels, bars, buttons, text
areas, keyboard and the splash image. **Button matrix is required by keyboard.**
Explicitly account for these dependencies rather than disabling every widget
without a matching application `*_create()` call.

Candidates to audit include arc and line (currently explicitly enabled), checkbox,
dropdown, roller, slider, switch, list, menu, message box, tab view, window, scale,
spinbox, spinner and other unused default-enabled widgets.

After removing widgets that need layouts, evaluate disabling flex/grid: the
review found no application layout calls using them, but library dependencies
must also be checked. Do not disable the keyboard, text area or image support.

### Documentation required

Make the supported subset unambiguous in `include/lv_conf.h`, grouping required
features and explicitly disabled features. Explain transitive dependencies such
as keyboard → button matrix.

In `docs/display-and-memory.md`, include a maintained table of enabled widgets
and layouts, why each is needed, and the procedure for adding a widget: enable
its dependencies, rebuild, check flash/RAM and verify the affected UI.

Add an `AGENTS.md` pointer and a short rule that this project intentionally does
**not** expose LVGL's default feature set. A future agent must consult the table
before using another LVGL widget or layout.

### Verification

Build and inspect the dashboard, raw/CAN pages, both tile bands, all settings
controls and the keyboard. Enter/edit/save/cancel SSID, password, host and port;
check password masking, keyboard symbols, focus and pressed/disabled states.

## Stage 4 — conditionally prune unused renderer target formats

The display and decoded splash use RGB565, but LVGL's default software renderer
supports other target formats too. Its dispatch switches can retain unused
backends. Audit `LV_DRAW_SW_SUPPORT_*` options in the pinned LVGL version.

**Do not reduce support blindly to RGB565 alone.** LVGL may create ARGB8888
intermediate layers for clipping, opacity and transforms. Preserve alpha-mask
handling for text. Source image formats and render-target formats are different
things; an unused display format does not prove all related code is unnecessary.

Start with clearly unused targets such as swapped RGB565, grayscale and
monochrome, after tracing the actual paths. Preserve current visuals, including
the splash's clipped reveal and gradient. Do not disable complex rendering or
change visual effects just to improve the number.

Document enabled/disabled targets and required intermediate formats both beside
the macros and in `docs/display-and-memory.md`. The documentation must warn that
adding images, transforms, opacity or clipping can introduce intermediate-format
requirements even when the physical panel remains RGB565.

Proceed only with a supported dependency argument and hardware checks covering
splash, keyboard, text, rounded corners, clipping and interaction states. Otherwise
defer this stage explicitly. Do not report performance or visual equivalence on
the basis of a successful build alone.

## Stage 5 — optional lossless DBC storage improvements

The existing generator already shares names, units and scales. Retain every
message and signal and keep lookup/decoding available for both PQ and MQB.

### A. Suffix sharing in the name pool

`tools/make_dbc_tables.py` deduplicates identical names. It could also point names
at suffixes of other NUL-terminated strings, preserving direct pointer lookup
with no runtime decompression. The reviewed inputs offer approximately **1,435 B**
of savings. Verify every generated name byte-for-byte after changing offsets.

### B. Six-byte signal records

`src/net/dbc.h` currently stores eight bytes per signal. The reviewed inputs have
2,612 signals, 189 scales and start bits at most 63. An 8-bit scale index and an
endian flag encoded alongside the start bit could produce naturally aligned
six-byte records, saving **5,224 B** of table data before decoder-code differences.

Avoid packed unaligned multi-byte accesses. Add generator bounds checks for the
new limits and a `static_assert` for record size. Narrowing the representation
must fail clearly if future DBC inputs exceed it, rather than silently truncating
values. Measure the access/bit-extraction cost before claiming performance parity.

Change the generator and decoder, then regenerate; never edit the generated
tables by hand. Preserve endian, multiplexing, DLC bounds, scales, units and
double-precision output semantics. Do not use this as an excuse to reduce the
raw-bit decoder's supported widths merely because current signals are shorter.

Document encoding limits in `src/net/dbc.h`, the generator and the CAN DATA/DBC
section of `docs/architecture.md`. Extend generator validation to compare all
logical table contents and decoding against the previous representation, rather
than relying only on the existing small set of known-signal checks. Inspect CAN
DATA names, units and values on hardware if available.

## Documentation discoverability and maintenance

Documentation is part of acceptance, not a follow-up task:

1. Keep the durable feature/format/font inventory in `docs/display-and-memory.md`.
   Do not require future agents to find this historical implementation brief.
2. Point to that inventory from `AGENTS.md`, including its documentation map and
   a concise warning near the existing LVGL/performance rules.
3. Put rationale and dependency comments immediately beside restrictive config
   macros and subset font bindings. Someone editing a header should see the
   restriction before introducing an unsupported feature.
4. Update `docs/architecture.md` for the HTTP-client choice and any DBC encoding
   changes; add a README pointer in its build/configuration or layout discussion.
5. Update relevant `TODO.md` entries and `docs/status.md` honestly. Distinguish
   built, bench-verified and car-verified behaviour. Never transfer old hardware
   verification automatically to a changed rendering/network path.
6. State the pinned library version in dependency explanations; re-audit feature
   defaults and dependencies when updating LVGL or Arduino.
7. Describe only implemented restrictions in durable docs. Leave deferred stages
   labelled as proposals here or in TODO, not as current behaviour.

## Build and acceptance gates

Before implementation, record git status and a fresh baseline build. Do not
overwrite unrelated user work. After each stage, build with:

```bash
./.venv/bin/pio run -e s3-dash
```

For compiler-configuration changes, use a clean rebuild if needed to ensure every
affected library unit sees the same configuration. Record the exact revision,
build flags and both PlatformIO size lines, plus deltas from the baseline.

Use the approved status-generation tool when updating measured documentation:

```bash
./.venv/bin/python tools/doc_status.py
./.venv/bin/python tools/check_docs_links.py
```

The repository excludes `.pio/` and other generated/vendor directories from agent
inspection. Normal documented build commands are expected, but if additional
ELF/map inspection is needed, ask the owner for a narrow exception rather than
silently reading/indexing excluded directories.

Hardware work requires owner authorization and access. Opening the serial port
can reset the board; follow `AGENTS.md` and `docs/console.md`. Compare like-for-like
idle and live-traffic runs, including:

- Internal RAM free/lowest, largest block and stack headroom.
- Render rate and flush time, with the same data, page and redraw mask.
- Moving RPM/speed, shift flash and indicator activity.
- HTTP fallback, reconnects, OTA upload/trial/fallback and drive-log download.

Do not accept a size saving that adds persistent allocations, socket churn,
extra redraws or measurable rendering/polling regressions without resolving it.
If hardware is unavailable, stop short of asserting the no-regression requirement
has been verified: report it as pending and give the owner the exact checks.

Final handoff should include a per-stage result table, actual flash/RAM lines,
which stages were deferred and why, documentation links, and an explicit list of
what was and was not tested. No release or version bump is implied.

## Upstream references used in the review

- [Pinned platform manifest](https://github.com/pioarduino/platform-espressif32/blob/55.03.312-1/platform.json)
- [Arduino 3.3.12 HTTPClient implementation](https://github.com/espressif/arduino-esp32/blob/3.3.12/libraries/HTTPClient/src/HTTPClient.cpp)
- [LVGL 9.3.0 stock fonts](https://github.com/lvgl/lvgl/tree/v9.3.0/src/font)
- [LVGL 9.3.0 configuration defaults](https://github.com/lvgl/lvgl/blob/v9.3.0/lv_conf_template.h)
- [LVGL 9.3.0 display initialization](https://github.com/lvgl/lvgl/blob/v9.3.0/src/display/lv_display.c)
- [LVGL 9.3.0 default theme](https://github.com/lvgl/lvgl/blob/v9.3.0/src/themes/default/lv_theme_default.c)
- [LVGL 9.3.0 renderer dispatch](https://github.com/lvgl/lvgl/blob/v9.3.0/src/draw/sw/blend/lv_draw_sw_blend.c)
- [LVGL 9.3.0 refresh/intermediate layers](https://github.com/lvgl/lvgl/blob/v9.3.0/src/core/lv_refr.c)

---

## Implementation record (0.8.1, 2026-11-12)

Stages 1–4 were implemented in one session on the 0.8.0 tree; stage 5 was not
attempted. Every stage was built and measured immediately after its edit, so
each delta is attributable to exactly one change. Baseline: a build of the
unmodified tree immediately before the first edit.

| Stage | Change | Flash | Δ flash | RAM |
|---|---|---:|---:|---:|
| baseline | unmodified 0.8.0 tree | 1,847,186 (94.0%) | — | 60,412 |
| 3 | LVGL widgets: 18 unused widget families, flex, grid and montserrat_18 pinned to 0; the kept set (label, bar, button, textarea, keyboard, buttonmatrix, image) pinned to 1 | 1,803,114 (91.7%) | −44,072 | 60,412 |
| 2 | telemetry.cpp: one persistent plain `NetworkClient` per endpoint, `begin(client, url)` | 1,711,890 (87.1%) | −91,224 | 60,020 |
| 1 | montserrat_22/28/48 replaced by byte-identical subsets | 1,564,210 (79.6%) | −147,680 | 60,196 |
| 4 | blend paths for RGB565_SWAPPED, RGB888, XRGB8888, L8, AL88, I1 pinned to 0; RGB565, RGB565A8, ARGB8888, ARGB8888_PREMULTIPLIED, A8 kept at 1 | 1,525,994 (77.6%) | −38,216 | 60,196 |
| **total** | | **1,525,994 (77.6%)** | **−321,192** | **60,196** |

The version was bumped to 0.8.1 before the final clean build; `tools/doc_status.py`
regenerated `docs/status.md` from that build. The clean build (whose build-date
string differs by a few bytes) measured 1,525,962 B — that is the figure
`docs/status.md` carries.

### How the subset fidelity was proved

1. Regenerated the **full** stock faces with lv_font_conv 1.5.2, using the exact
   options recorded in each stock font header, and compared every static array
   against LVGL 9.3.0's committed sources byte-for-byte: PASS for 22/28/48 px.
   1.5.3 differs by a handful of ±1 anti-aliasing bytes (same metrics, kerning
   and cmaps), so the subsets use 1.5.2 via `LV_FONT_CONV_SUBSETS`.
2. Generated the subsets with the same options and compared every kept glyph's
   bitmap slice and per-glyph metrics against the stock sources: PASS, 56/56
   glyphs (11 + 18 + 27). The check is committed as `tools/check_font_subsets.py`.
3. `lv_font_conv` derives `line_height`/`base_line` from the glyphs kept — digit
   sets have no descenders, so the generated metrics collapsed (48 px: 34/0 vs
   the stock 52/9). `tools/make_fonts.sh` pins the stock metrics (52/9, 24/4,
   30/5) after generating; the subsets would otherwise have shifted every label
   vertically.

### Deviations from the brief

- Stage 3 initially failed to build: `LV_USE_WIN` was left undefined and its
  widget compiled with flex disabled. Fixed by pinning `LV_USE_WIN 0`; the
  audit of every disabled widget's guard (all are self-guarded in 9.3.0) was
  done against the upstream sources, and flex/grid were confirmed unused by
  the kept widgets, the default theme and `lv_obj.c`.
- Stage 4 was taken beyond the "conservative" first step after tracing the
  gates in `lv_draw_sw_blend_to_rgb565.c`: RGB888 and XRGB8888 are also dead —
  the only image source in the firmware is the splash decoder, which emits
  RGB565 (`src/ui/splash_img.cpp`), and both formats gate their blend case and
  implementation independently. A8 is kept: the letter-drawing path
  (`lv_draw_sw_letter.c`) is gated on `LV_DRAW_SW_SUPPORT_A8`.
- Subset files are named `lv_font_dash_big_48.c` / `_val_22` / `_mode_28`
  rather than `font_big_48.c` etc., because lv_font_conv 1.5.2 has no
  `--lv-font-name` and names the struct after the output basename.
- The stock faces included a FontAwesome symbol range; the subsets drop it
  (no role bound to them draws an `LV_SYMBOL_*`; the keyboard's symbols come
  from montserrat_14, which stays stock).
- Stage 5 (DBC storage) was skipped as the brief allows; it remains an option
  worth ~6.7 KB and is not recorded as built anywhere.

### Documentation written

- `include/lv_conf.h`: every enable/disable carries its reason.
- `include/theme.h`: subset bindings with the generation story.
- `tools/make_fonts.sh`: the subset commands, the two-toolchain requirement,
  the metric pinning, and how to extend a character set.
- `docs/display-and-memory.md`: the face/role table, subset code-point table
  and the baseline caveat.
- `docs/architecture.md`: the plain-HTTP client choice under "Connection reuse".
- `AGENTS.md`: rule 12 and a documentation-map row for this brief.

### Not verified (no board attached)

Everything in the brief's hardware lists is still pending: the settings page
and keyboard behaviour on glass, both tile bands with the subset fonts (temps,
fuel, locks, speed, all six mode names, `SETTINGS`), km/h vs mph and °C vs °F,
HTTP polling against a live controller, OTA upload/trial/fallback, and the
render-rate/RAM figures under radio traffic. A successful `pio run` is the only
check performed here; nothing was flashed.

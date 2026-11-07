# Development

Apple's Command Line Tools (macOS 15 SDK or newer, from Xcode 16 or newer) and Make are sufficient.
The SDK requirement includes the optional own-window diagnostic recorder; the application's
deployment target remains macOS 14. Builds download nothing. C17 owns behaviour;
Objective-C is confined to the Mac shell/hardware adapter, and the system WebKit renderer needs
no frontend compilation or package manager.

```sh
make -j"$(sysctl -n hw.logicalcpu)"
make check
make -j"$(sysctl -n hw.logicalcpu)" SANITIZE=1 check fuzz
make analyze
```

The default arm64 build targets macOS 14 and M1-compatible instructions, without requiring the
development Mac's CPU. It uses `-O3 -flto=thin` at compile and link time. This choice prioritizes
runtime performance, not build speed. The measured advantage over plain O3 was small and not
statistically conclusive; it implies neither faster inference in external engines nor a universal
speedup. No fast-math assumptions are enabled. Debug objects remain in build; installation strips
symbols from the shipped executable.

Own C uses `-Weverything -Werror`, except padding, C++ compatibility and declarations after statements.
Objective-C/vendor warnings are scoped separately. The SDK is explicit and compiler-generated
header dependencies support incremental builds. Changing UI files requires installation, not C
compilation. Optional `.clang-format` describes formatting without adding a build dependency.

`SANITIZE=1` uses O1, ASan/UBSan and `build/sanitize`, leaving the normal outputs separate. The first
finding fails the run. Clang's analyzer uses the same Command Line Tools. LeakSanitizer is unavailable
on the current Apple-silicon test host; do not report an ASan pass as proof of leak freedom.
An absolute `BUILD=/path/without/spaces` can move build outputs outside a checkout. The checkout
itself may contain spaces; Make's custom output directory may not.

## Checks and native verification

`tests/checks.c` exercises real temporary databases, files and supervised child processes. Its cases
cover commit failure/crash recovery, branches/context, paging/search, archive/trash, preferences,
backup/restore, protocol boundaries, cancellation/late callbacks, GGUF bounds, acquisition/resume,
file identity and process cleanup. No private profile, model or network service is required.
Setup checks also cover consent, readiness, offline reuse, dependency ordering and malformed
package inputs. The actual engine/model downloads require separate native integration checks.
Advice checks cover variant tradeoffs, incompatible evaluation setups, optional metadata, bounded
retention, archive isolation, execution scopes, opt-out and reset during an active request.
The cache scan is redirected to its temporary directory.

`tests/fuzz.c` makes 10,000 deterministic mutations to streams, settings and engine observations,
plus 1,000 GGUF mutations. Run it under sanitizers. This is targeted reproducible mutation testing,
not an exhaustive fuzzing claim. Add a regression for a distinct failure mechanism, without
mirroring every implementation step or imposing a test count.

Tests respect `TMPDIR`; point it at an existing absolute directory to keep experiments together.
Successful runs clean up. Inspect and remove the run's private `overtone-checks-*` directory after
an interrupted/failing run. Do not turn temporary profiles, screenshots or traces into build inputs.

Native changes need native checks: model selection, send/stop, parameter reload, engine switch,
file panels, close/reopen, or the affected path. Confirm persisted data and `--inspect` state as well
as visible output. Process tests distinguish owned groups from already running external servers.
A renderer fixture can verify layout/formatting but cannot certify backend integration, AppKit
behaviour or presented frame rate.

Measure optimized builds separately from instrumentation. Record workload, cache state, build mode,
sample distribution and whether a time is UI, application CPU, I/O or inference. Count application,
renderer and engine memory separately. Compare equivalent work and include write/storage costs
when an index accelerates reads. The development Mac is not a minimum hardware requirement.

## State and extension rules

Start with the user-visible result, data ownership and operation lifetime. The serial owner is the
only writer. Put blocking file/CPU work on a bounded worker and apply its result back on that owner.
Copy borrowed strings before asynchronous use. Returned C JSON buffers belong to the caller.
Cancellation must make late callbacks harmless as well as stop the underlying resource.

New operations should reuse the existing command/validation boundary and explicit lifecycle.
Keep an independent adapter only for a replaceable resource or independent lifetime. Do not add
forwarding hierarchies, universal task frameworks, custom allocators or generated manifests where
small direct code and Make express the requirement. No migration code or historical schema fixtures
are maintained before the first release. Use fresh disposable data when the schema changes.

[Architecture](architecture.md) describes persistence, resources, process ownership and limits.
Keep native test profiles in a private runtime directory under `~/Library/Caches`, outside
Desktop, Documents, Downloads and the source checkout. A custom `--profile` inside a protected
folder legitimately triggers macOS consent; repeated ad-hoc builds can invalidate that consent.
Store only final observations with your development notes, and remove runtime profiles after use.
The GUI starts with `/` as its working directory; short helper probes do the same. Managed engines
use their explicit profile directory. No Files and Folders permission or Full Disk Access is
required for the default Application Support profile. Files chosen in system dialogs remain
subject to macOS privacy policy; never reset or bypass TCC for a test.

[Diagnostics](diagnostics.md) documents content-free local inspection. Keep those responsibilities
there instead of repeating their implementation in every README.

## Presentation conventions

Preserve interaction meaning before making controls uniform. Remove redundant actions; choose
one predictable behaviour for equivalent operations. Shared primitives live in `app.css` and
`shell.js`; feature files retain only feature-specific decisions.

- Use the shared radius and inset geometry. AppKit owns the actual outer window mask. Align related
  controls on rows/columns only when their internal spacing stays balanced. Scrollable category
  navigation must not compress multiline labels against the rounded edge.
- Searches keep the icon on the left and one container focus ring. Quiet controls use shared hover
  fills; selection is stronger and persists under hover. Destructive actions retain red semantics
  rather than inheriting the current accent. Nested actions must remain visible over selected rows.
- `appearance.js` defines paired fills/ink in both themes, separate from decorative tints. Validate
  actual surfaces, code, disabled states and nested controls in all six tones. Text needs 4.5:1
  contrast and essential icons 3:1. Do not fix contrast by removing the accent from the product.
- Purple UI uses #5f55ec. The approved OverTone mark is independent of UI tones: slate #627FA3,
  steel #8A9DB6 and copper #BF8B52 in every theme/accent. Keep the SVG viewBox, paths and transforms.
  The signed OS icon uses the supplied Assets.car and OverTone.icns fallback; its editable source
  is OverTone.icon. No icon extension, runtime override or Finder cache mutation is used.
- Author short, direct labels. Icon-only actions need accessible names and delayed tooltips; visible
  duplicate labels do not need another tooltip. Open popovers suppress the invoker's tooltip.
  Keyboard focus stays visible; pointer opening must not manufacture a focus highlight.

Application menus use AppKit key equivalents: Cmd-N new conversation, Cmd-F search in the
current workspace, Cmd-comma Settings, Cmd-1…5 sections, Shift-Cmd-I Details and Cmd-period Stop.
Model parameters and the guide remain discoverable without assigning extra shortcuts. Text editing
keeps the standard responder-chain actions. Modifiers are part of each binding; do not intercept
plain letters, input-method composition or the configurable Enter-to-send combinations.

The renderer publishes a small, deduplicated menu projection on state/view/dialog changes. Native
validation also respects file panels, startup and other native windows; execution rechecks the
current UI policy. Use real controls rather than a second implementation of their behaviour.
Context menus are disabled through the shared document event registration, including the isolated
reader. Mouse menu actions keep pointer focus styling; keyboard actions retain visible focus.

Global sections remain in the unified titlebar. Native control clearance is measured, not guessed;
WebKit reports only bounded hit rectangles. Empty chrome drags the window; interactive controls and
full-screen mode do not. System theme preview uses the independent OS appearance, without reading
private preference keys or making a network request.

The same Details command works in every workspace. At 1,568 CSS pixels and above it docks; compact layouts
use the same outside-click/Escape dismissal as History, with focus return and inert background.
One animated layout edge moves the workspace and inspector together. Inspector content keeps its
inner width; do not reserve the final column before entry or leave its width behind after exit.
The composer and conversation share a bounded frame centred on the window, with symmetric clearance
for the larger visible side panel. User bubbles remain narrower and right-aligned to that frame.

Motion uses shared durations/easing and visibility lifecycle, with reduced-motion and inactive-window
handling in `ui/motion.js`. Submit coordinated effects in one task and let the browser start
their pending plays together; do not backdate them to an earlier callback or add a
pause/ready/play cycle. Prefer transforms/opacity for independent travel. In-flow expansion shares one edge with
its neighbours, including updates arriving during movement. Review entry, exit, interruption,
reversal, resize and simultaneous navigation. Finish or cancel stale effects and remove temporary
layer hints. Do not use a fixed frame timer or equate a 120 Hz display with 120 presented FPS.
Details and flow expansion use 420/340 ms entry/exit, workspace/selection arrival 300 ms,
ordinary exits and menus 240 ms, and feedback 160 ms;
changing these is a product decision, not an incidental optimization. Continuous indicators use
linear timing; flow uses the shared cubic-bezier(.42,0,.58,1), with zero endpoint velocity.

For surfaces that resize beside docked Details, use `flow-surface` on the painted rectangle and
`flow-anchor` on independently aligned contents. The coordinator reads visible geometry in two
batches and animates paint separately, preserving corners and text size. Shared metadata rows
retain their trailing values and separators. Do not animate layout width each frame or add
component-specific selector lists. Use these traits only when an element actually participates
in the shared flow; they are not required on every DOM node.

The main window and its WKWebView exist before data opening starts. A large, centred loading
surface in that same window reveals six stationary blades from their inner ends along curve masks.
`loading.m` uses Core Animation so this vector reveal does not inherit WebKit’s non-accelerated cadence.
The visible paths, transforms and palette come from `ui/OverTone.svg`; `ui/loading.svg` contains only the
six registered reveal guides. Its small parser accepts only the bundled absolute M/L/C/Z geometry and
translate/scale groups, not arbitrary SVG. Guides must have smooth, forward-moving terminal segments:
short backtracking curves can cut triangular holes in a wide stroke.
The staggered entry reverses as one synchronized group and loops until readiness, without a reset jump.
There is no separate splash window or replacement WebView. The first durable projection calls finish
immediately, even during entry; Reduce Motion shows the static master. Only the paint fades after ready;
input is already enabled. No minimum wait, frame timer or high-rate idle activity is added. Full-screen
preparation coordinates the brand movement before AppKit snapshots the content; a bounded native
timeout preserves the window command if the renderer cannot respond.

Stream projections are cumulative. Keep only the latest per frame and let full state supersede
pending fragments. Reuse code/message nodes, highlight complete neutral lexical prefixes and
invalidate them when language/source changes. Observe visibility before writing colours;
do not scan code bounds on scroll. Skip hidden
readers; a zero-size scroll event is not evidence the user reached the bottom. Returning to an
active conversation follows its response/reasoning; manual upward scrolling suspends following.
Historical prefetch waits for finite UI motion, while scroll-driven read-ahead remains adaptive.

Markdown is a bounded subset, not full CommonMark. Preserve raw text, copying and safety when
extending it. No model-authored HTML/scripts/SVG or automatic remote images are allowed. Malformed
application figures remain code. Selection/copy covers message text and original code whitespace,
excluding toolbars and decorative layout gaps. Oversized blocks remain readable plain text.

Keep action/status DOM identities during updates. Catalog pagination preserves scroll and selected
variants. Download travel belongs only to initial enqueue. Scrolling settings, licenses and expanded
code keep their heading/close control outside the scroll body. App statuses are separate from
copyable model output; internal codes must not masquerade as generated prose.

## Localisation

Ten local catalogues in `ui/locales/` share Italian source keys. Native code injects only the selected
catalogue before UI scripts and reuses it for app-owned menus. OS-supplied controls follow macOS.
There are no remote fonts, translation calls or localization runtime packages.

Use `localLabel`/`localAccessible` for dynamic authored labels. Never translate model names, user titles
or generated text. Full quantity phrases use `Intl.PluralRules`; do not concatenate translated word
fragments. Add new keys to every catalogue, preserve placeholders and check narrow layouts. Let labels
wrap or use natural width instead of authored ellipses. Italian keeps common input/output terminology;
other languages follow their own technical usage.

Regional formatting is independent of interface language. OS locale and clock conventions provide
local defaults, with user overrides. Human timestamps include a date outside the current local day;
midnight/focus refresh handles timezone/calendar changes. Stored instants stay Unix seconds.

## Installation and publication

`make` builds ordinary executables. `make install` updates the single `/Applications/OverTone.app`;
`make run` also opens it. Quit before updating. For native sanitizer testing install into that same
location, then restore the normal build. No second Dev/Preview app is needed.

`make package` installs and archives that app using Apple's codesign, strip, ditto and shasum.
The staged bundle is checked before replacing the previous one, which is restored if the final move
fails. Power loss between renames can leave `previous` inside a hidden `.overtone-install.*` directory;
recover it to `OverTone.app`. Profiles never live in the installation directory.

Default ad-hoc signing seals the bundle without a trusted publisher identity, notarization or stable
privacy permissions across rebuilt identities. Normal builds use Hardened Runtime without exception
entitlements. Sanitizer builds omit it and cannot be packaged. An explicitly authorized distribution
build may use `SIGN_IDENTITY="Developer ID Application: …"`; no target uploads or publishes anything.
See Apple's [distribution signing](https://developer.apple.com/documentation/xcode/creating-distribution-signed-code-for-the-mac/)
and [notarization](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).

The current target is Apple silicon; macOS 14 is a deployment target, not certification on every OS
version. Windows/Linux remain future targets. Test supported systems before advertising a release.
Back up important profiles, install in one consistent location and verify data/selection after update.
Backup/restore covers only the current schema and model references, not weight files.

Before publication, verify a clean source copy builds/tests without personal files or internal tools.
Review licenses, notices, sensitive information and Git history separately from the working tree.
Exclude profiles, weights, screenshots, traces, benchmark outputs, internal notes and generated files.
The repository must be sufficient to build the product; developer evidence is not a dependency.

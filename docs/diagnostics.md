# Local diagnostics

The app executable is also a read-only inspection client:

```sh
/Applications/OverTone.app/Contents/MacOS/overtone --inspect "$HOME/Library/Application Support/OverTone"
```

Pass the same absolute profile used to launch the app. A planned data-location change is followed
through its current bootstrap; the active folder can also be passed directly. The command discovers the private local
socket through that profile, verifies ownership, and reads one JSON snapshot. The socket accepts
only the current OS user. There is no TCP listener, arbitrary SQL, JavaScript or shell execution.

With **Developer diagnostics** enabled in the target profile, the same executable provides
bounded window and UI controls. This avoids depending on pointer automation to recover a test
window after resizing or changing Spaces. It changes only OverTone, without macOS automation
permissions or a browser inspector:

```sh
overtone --control /absolute/test-profile show
overtone --control /absolute/test-profile observe 20
overtone --control /absolute/test-profile state
overtone --control /absolute/test-profile capture
overtone --control /absolute/test-profile resize 1280 800
overtone --control /absolute/test-profile press details,documents
overtone --control /absolute/test-profile fill query "SmolLM2 GGUF"
overtone --control /absolute/test-profile fullscreen
overtone --control /absolute/test-profile trace 10
overtone --control /absolute/test-profile record 3
overtone --control /absolute/test-profile trace-state
overtone --control /absolute/test-profile diagnostics off
overtone --control /absolute/test-profile quit
```

`show` restores a minimized/hidden window and activates its existing Space through public AppKit
APIs. Its reply includes the workspace window number and count, so loading transitions can
be checked without conflating two windows. It does not move the window into the caller's desktop or place it across other Spaces.
Use native full screen for an isolated test Space. Inspect `window.active`, `applicationActive`,
`onActiveSpace`, `visible` and `occluded`
before timing animations; a successful activation request alone does not prove presentation.
`observe SECONDS` holds a display-awake activity for 1–30 seconds; `observe 0` releases it.
Expiry, shutdown and disabling developer diagnostics also release the activity. It does not
activate or reposition windows, change system preferences, or unlock the session. Its reply
distinguishes accepting the activity from `visibilityReady`; wait for real visibility before measuring.
`resize` accepts window sizes from 640×480 up to the screen's visible frame, outside full screen.
`state` reads current renderer focus/visibility, workspace/panel intent, model tab/source and bounded download controls, scrolling/editing,
the last virtual history viewport, open popovers, temporary flow surfaces, finite animation count and up to sixteen active effect
targets/timelines, plus active bounded observers and up to sixteen feedback surfaces with their
severity and accessibility role, without message text. An open setup dialog also reports
bounded control geometry and resolved colours; checked checkboxes expose bounded rectangles.
`state.startup` reports the centred loading surface, native reveal progress and its pending/visible state.
The `branding` observation reports the master load state, fixed palette, filter and reveal state.
`state.composer` reports the input budget in bytes, field lengths, review intent and pending status,
overflow membership, editor/palette geometry and bounded command rectangles, without text.
These reads and animation enumeration may update style.
Use them to check settlement; the geometry
in `--inspect` is the last published layout observation, not a fresh renderer evaluation.
`fill suggestion TEXT` edits only the visible prompt revision through the same input handlers; it cannot edit a hidden or covered field.
`capture` uses WebKit's public snapshot API and overwrites `diagnostic-ui.png` in the same profile.
Unlike inspection, this explicit command includes visible UI content. It stays local and needs
no screen-recording permission; native titlebar buttons are outside the captured WebKit surface. During the native loading cover,
`capture` uses an own-window ScreenCaptureKit screenshot instead (macOS 14.4+), including native paint.
Take captures after measurements: capture/PNG encoding costs work and is not a physical display trace.
`fullscreen` toggles the native mode; `reload` reloads the existing renderer and its normal readiness path.
`quit` follows the normal draft/engine cleanup path.
`press` accepts at most four comma-separated named actions in one renderer task:
`chat`, `models`, `documents`, `projects`, `tools`, `details`, `history`, `dismiss`, `filters`,
`conversations`, `archive`, `trash`, `installed`, `search`, `activity`, `pause-download`, `resume-download`, `cancel-download`, `restart-download`, `settings`, `parameters`, `parameters-save`, `model`, `thinking`,
`hf`, `ollama`, `discover`, `oldest`, `latest`, `reasoning`, `wrap`, `expand`, `appearance`, `diagnostics`,
`link`, `back`, `preview`, `download`, `first-repository`, `startup`, `close`, `copy-code`,
`copy-answer`, `remove-installed`, `remove-reference`, `edit`, `cancel-edit`, `send`, `stop`, `improve`, `discard-suggestion`,
`accept-suggestion`, `compose-more`, `setup`, `setup-local`, `setup-existing`, `setup-skip`, `setup-back`,
`setup-prepare`, `setup-only-engines`, `setup-engine-consent`, `setup-engine-confirm`, `setup-consent`, `setup-server`, `setup-info`, `setup-first-existing`, `setup-pause`, `setup-resume`, `setup-cancel`,
`setup-first`, `setup-second`, `setup-third`, `setup-general`, `setup-code`, `install-engine`,
`setup-engines`, `setup-purposes`, `setup-size`, `setup-size-N` (0, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 256, 512), `setup-ollama`, `setup-llama`, `setup-existing-llama`,
`thinking-off`, `thinking-on`, `settings-privacy`, `network-master`, `network-expand`,
`network-catalogues`, `network-models`, `network-engines`, `network-servers`, `server-enable`, `developer`, `settings-models`, `settings-api`, `connection`, `connection-https`,
`connection-auth`, `connection-remember`, `connection-test`, `connection-save`,
`install-llama`, `engine-confirm`, `engine-detect`, `engine-detect-llama`, `engine-detect-ollama`,
`engine-verify-llama`, `engine-verify-ollama`, `engine-licenses-llama`, `engine-licenses-ollama`,
`licenses`, `catalogues-enable`, `advice`, `advice-refresh`, `advice-acquire`,
`advice-dismiss`, `advice-preferences`, `advice-info`, `advice-enable`,
`advice-learning`, `advice-notices`, `advice-reset`, `advice-reset-confirm`, `advice-restore`,
`guide`, `guide-next`, `guide-previous` and `guide-skip`. Advice reset affects only optional learning statistics.
Named actions scroll their existing control into view before the same modal-aware hit test.
`state.activities` reports bounded refresh/verification icon slots and phases; `engineScan` is the
completed startup/manual engine-detection sequence.
`state.advice` reports bounded choice/control geometry, source readiness, collection epoch, unread status, notices and opt-in state; `state.guidance`
reports the current anchor and placement, without conversation content. These operate the visible connection
and engine-installation controls; installation still requires its normal confirmation. Setup commands operate its existing visible controls;
they do not bypass consent or readiness checks. `startup` replays the
centred six-blade native vector loading surface in a continuous diagnostic loop; `fill logo done` ends it.
This replay does not delay actual app readiness. The repository action opens the
first visible catalogue result. `repository-N` activates result 1–100 in the visible store. Activating the open family again closes its variants.
These actions invoke the existing controls and respect the current modal.
The named target must also pass the centre-point hit test in its document (the visible name on a catalogue row);
`control_obscured` reports a covering surface instead of clicking through it.
`state.closingDialogs` distinguishes completed animation objects from dialogs whose close
callback has not yet run; `state.configuration` reports bounded setup control geometry.
`state.dialogs` reports up to eight open dialog identities and their modal/behind/closing state,
without their content, so nested close transitions can be checked without screenshots.
The developer-only `setup` action can reopen the guide after completion; the product has no
permanent setup button once configured. Model-choice actions follow the current adaptive order.
`library.suggestions` in inspection reports whether the guide is open, whether it is fetching
metadata, candidate counts per engine, collection time, request count, read-ahead hits and peak concurrent
requests, without model names or user activity. Setup geometry includes the canonical intent, selected
recommendation, held-plan status and consent action. Guide state includes passive-cue opacity and
elapsed time, allowing its action cue to be checked without changing the control. Startup reports real wait duration and whether the logo was revealed.
`menu` reads native menu labels, key equivalents, modifier masks, availability and checkmarks.
`menu <command>` invokes one named application command through its normal menu validation;
`shortcut <command>` dispatches its AppKit key equivalent locally. Names are `new-chat`, `find`,
`settings`, `chat`, `models`, `knowledge`, `projects`, `tools`, `details`, `parameters`, `stop` and
`guide`. Commands without a shortcut reject `shortcut`. These controls do not inject global
keyboard events or bypass dialog/guide restrictions. `context-menu` verifies cancellation of a
contextmenu event in each registered UI document; it is not a physical pointer measurement.

`hide` hides only OverTone, through its own AppKit instance; `show` restores it. These opt-in controls let
visibility changes be checked without controlling another application.
`state.components` distinguishes managed executables from external installations without paths;
this is separate from ownership of a running process.
Its bounded `coveredBy` identifier helps locate the covering UI layer, without message content.
Reader controls also require their frame to be exposed in the host document.
Dismissal scrims use an exposed corner, since their centre can belong to the open panel.
`filters` selects the visible search's filter control; model tabs also work in the compact layout.
`state.installedLibrary` reports bounded model IDs and rendered row count without names or file paths.
`state.filterPanels` reports all six facet states: selected values, contextual counts, visible-choice
geometry, overflow flags, active category, chips, actual active-filter count (excluding order/usage priority) and last local filtering cost (`workMs`). That cost
excludes sorting, row rendering, C projection, IPC and animation; it is not a frame-time measurement.
Open a category with `<select-id>-category`, then a visible value with `<select-id>-<value>`, for example
`installed-quant-category`, `installed-quant-q4` or `installed-size-4`. `installed-filters-reset` uses
the normal Clear action. `clear-<select-id>-<value>` removes its visible chip. The same rule covers
activity/catalogue choices. `variant-filters` opens the current family's own panel; `more-installed`
adds the next batch and `first-installed` opens the first visible row's Details. Disabled or covered options cannot be pressed. `fill installed-query <text>`
uses the real library search field. Removal actions operate the first visible model's normal confirmation;
`remove-reference` removes only its registry reference.
Native file dialogs and data-location work also block mutating commands; `show` and `state` remain available.
No arbitrary selectors or script payloads are accepted. Named actions can change the target profile,
install components or delete its test data through the normal UI confirmations. Use a disposable
profile. Unavailable controls fail rather than clicking elsewhere.
`fill` accepts only the named fields `query`, `ollama-query`, `url`, `checksum`, `prompt`, `suggestion`, `language`,
`server`, `server-model`, `protocol`, `server-key`, `context`, `input`, `output`, `temperature`,
`top-p`, `memory-budget`, `region`, `date-format`, `chat-query` and `chat-title`.
It applies the normal input/change path and requires an enabled visible field.
`fill logo P` freezes the native reveal at progress 0–1; `master` shows the complete approved logo
and `done` delivers the same finish used by real readiness. These are developer-only inspections;
the ordinary startup never waits for a replay or a minimum animation time.
Appearance buttons are available as `theme-light`, `theme-dark`, `theme-system` and
`accent-blue`, `accent-purple`, `accent-teal`, `accent-green`, `accent-amber`, `accent-rose`.
They require the Appearance settings to be open. `fill language CODE` uses its visible
language selector and accepts only the options actually offered by the application.
`focus NAME` focuses an existing named control, including `prompt` and `suggestion`, using the same visibility and modal checks.
`key ArrowLeft|ArrowRight|ArrowUp|ArrowDown|Home|End|Escape|Enter` dispatches one keyboard action to it.
These controls do not target another app. `new-chat`, `first-chat`, and `setup-more-existing` use the normal visible controls.
`fill purpose N` and `fill setup-existing-query TEXT` support the corresponding settings/setup fields.
`state.appearance` reports the stored theme choice (`system` stays `system`), accent and UI language.
`--inspect` reports the independent OS appearance as `window.systemDark`.
`state.parameters` reports the visible numeric draft and memory-segment geometry, without model names.
Chat lifecycle controls are `rename-chat`, `rename-save`, `archive-chat`, `restore-chat`, `trash-menu`,
`move-trash`, `delete-chat`, `restore-all`, `empty-trash` and `confirm-action`. The last one presses
the visible confirmation button; it does not bypass the dialog. Settings sections use
`settings-chat`, `settings-storage`, `settings-knowledge`, `settings-tools`, `settings-labs` and
`settings-about`, alongside the section names above. Visible checkboxes are available as
`pref-tooltips`, `pref-codeWrap`, `pref-titles`, `pref-summaries` and `pref-improve`.
The transport's valid-UTF-8/256-byte request limit also applies to these developer inputs;
it is separate from the full end-user UI. `preview` validates the link and `download` presses
only its dedicated model-download confirmation, including the existing checksum/network checks.
Replies contain `ok` and a bounded error code on failure; the client also exits unsuccessfully.
Wait for the movement to settle before the next observation when measuring isolated animations.
The protocol accepts one valid UTF-8 request line under 256 bytes, without control bytes, with a 200 ms absolute input
deadline; UI acknowledgement is bounded to two seconds. It has no idle polling loop.

`trace SECONDS [h:POINT[:START:END]|v:POINT[:START:END]]` records up to ten seconds of WindowServer frame metadata
for this process's own window. On macOS 14.4+, it uses ScreenCaptureKit's current-process
enumeration, without requesting screen-recording consent or enumerating other apps' contents.
Older systems report an unavailable feature; there is no permissioned fallback. No audio or
screen images are saved. The window buffer uses nominal dimensions, bounded to 4,096 pixels
on either axis. Only an eight-pixel strip (horizontal through the middle by default) is analysed
on the CPU. It produces display/arrival times, status, a hash, texture and pixel-change/edge
counts, capped at 2,560 samples. An optional START:END interval in window points limits the
edge detector along that strip. Buffer dimensions and scale are included in the result;
single-window capture must not rely on sourceRect cropping.
The capture and file write are asynchronous; poll `trace-state` for `done` or `failed`, then read
`diagnostic-motion.json` in the profile. Only one trace can run. Shutdown or disabling developer
diagnostics cancels it. The stream and sample queue are released after stopping.

For frame-by-frame visual inspection, `record` accepts the same duration/strip arguments
and, on macOS 15+, explicitly saves `diagnostic-motion.mp4` alongside the metadata. It
records only this app's window, without audio or a system-wide capture fallback. The
previous diagnostic video is replaced. `trace` never records video. Video finalization is
asynchronous and bounded; wait for `trace-state` to report `done` before reading it.
Recording contains visible UI content and adds encoding work, so use a disposable profile
and compare timing separately without video. Nothing is uploaded.

The trace includes idle/suspended frames and static periods. Correlate it with the actions and
their actual animations, reject hidden/occluded intervals, and distinguish new pixels from repeated
frames. Capture initialization and recording itself can perturb the system: compare with an
unrecorded run. WindowServer display timestamps are closer to presentation than JavaScript callbacks,
but do not measure panel scanout or certify every monitor, refresh rate, or hardware configuration.

The versioned snapshot includes:

- Active operation and revision, received/answer byte counts, commits and storage failure status.
  `errorRevision` identifies a new failure or reset; `errorChat` and `errorTurn` identify its
  owner when known. Zero denotes no chat/turn ownership, including a global storage failure.
- Up to 12 recent chat/annotation completions, with operation ID, kind, duration and result code.
  Kinds are `0` chat, `1` title, `2` summary and `3` prompt improvement.
  The logging preference filters these observations: errors, warnings, normal chat completions,
  or all completions in Debug. The bounded in-memory history contains no conversation text.
- UI state, sampled memory/CPU/disk observations and the calculated admission budget.
  A renderer fault records only the bundled script basename, line and column; exception
  text and user content are excluded.
- Engine readiness, ownership, PID and acquisition progress/error state.
  `server` separately reports verified API metadata, context, loaded context, residency and a
  bounded connection error code. `connectionDiscovery` distinguishes editing from background
  restoration. These fields contain no endpoint, model identifier or credential. The UI control
  snapshot's `connection` reports bounded form geometry, phase and presence flags, never key text.
  `engineError` retains the preparation failure across unrelated catalog or inventory work,
  until a new selection, connection, recovery or explicit stop replaces it.
- Model readiness separately from server readiness, and the acquisition helper's separate ownership/PID.
- A compact memory observation: draft model ID/context/GPU layers, estimate source and component
  byte counts, pending state, observed weight/cache/linear-cache/compute buffers, and the owned
  engine's physical footprint when known. An engine plan
  predicts allocation; a process footprint measures physical accounting. Neither is added to the
  system's used-memory count. External engines can leave the footprint unavailable.
  `source: observed` can be a compatible cached allocation or a context projection; it does not
  mean the model is loaded. `buffers` describes current parsed runner buffers. `windowCells`
  records the observed sliding-bank capacity when known. Cached identities/samples are not exposed.
- Reading-window turn count, pending read, last read latency, last renderer update duration and
  anchor displacement/loss. Update duration includes synchronous JS/layout, not decoding, paint
  or frame rate. A half-point displacement can be pixel rounding; anchor loss during a deliberate
  branch or conversation change is different from loss during paging.
  `following` records the tail-follow intent, and `tailGap` the local distance to the rendered
  bottom in points. Stream observations are limited to four per second and contain no text.
  `contentHeight` and `viewportHeight` distinguish document-size changes from scrolling. A hidden
  workspace can report zero geometry; that is not an empty conversation.
- Native window position/drag-event count, content and renderer dimensions in points, full-screen state and the last
  reported web viewport dimensions. These help diagnose clipping and resizing without exposing
  the desktop contents. Native geometry is sampled on the main thread and published immutably.
  `controlsLeft`/`controlsRight` record the reserved native-control space; horizontal brand and
  navigation bounds make initial overlap and full-screen restoration shifts measurable.
  The `measured` object adds the actual top-bar height, brand/navigation bounds and visible
  Chat/history/Details header baselines and command centers, so full-screen spacing and alignment
  between window commands and section actions can be compared in points.
  `newCenterX` and `chat-filters-toggleCenterX` expose the two history command axes.
  `logoLeft`/`logoWidth` describe the SVG viewport; `historyCardLeft` records the card edge.
  `historySearchLeft` records the outer edge of the history search box.
  `windowButtonsRight` is the observed right edge of the native buttons, before safety insets.
  `chromeReady` confirms that the renderer applied native chrome geometry; `chromeError`
  is the WebKit error code if that call failed.
  The measured context flags distinguish an open scrim from its computed visibility/opacity
  (opacity is scaled to 0–1000), History/Details intent and an inert model workspace.
  `rendererFocused` is the document's focus observation; `window.active` is AppKit's key-window
  state. Neither describes the physical presentation of every frame.

With developer diagnostics enabled, `network` retains at most twelve completed public-catalogue
or file-request observations: wire/decoded body bytes, elapsed milliseconds, HTTP protocol and
redirect count. No endpoint, URL, headers, cookies or body is included. It resets with the process.

Snapshots come from the same serial owner used by the UI. They do not reconstruct state from log
phrases. Replies are bounded to 32 KiB and have a timeout. Prompts, answers, reasoning, API keys,
model names and filesystem paths are excluded. Counts reset with the process; there is no persistent
telemetry or diagnostic upload. An unavailable owner produces a failure, not invented healthy state.

Useful distinctions: a ready server does not mean Ollama has already loaded weights; a completed
network response does not imply a committed conversation; a requested stop does not imply a reaped
process. Inspect the corresponding fields and terminal results. Engine failure details currently
use stable application error codes, rather than exposing potentially private engine stderr.

Rendering observations count code blocks, known languages, coloured blocks and pending highlighting,
bounded character counts and blocks whose cached source differs from their visible text,
plus the largest highlighter batch duration since the previous observation and Details visibility. Counts contain no
source text. A pending block may simply be outside the bounded visible highlighting
window; duration covers the JS pass, not GPU paint or display frame rate. Observations
are combined over 200 ms, outside the colour update, and do not force a new layout.

With developer diagnostics enabled, `motion` holds the latest bounded sample for each transition kind:
interval count, median/p95/p99/maximum, elapsed time and interruption/inactive flags.
One observer covers concurrent effects and stops on completion, blur or hidden content. Continuous
indicators are observed for at most 2.2 seconds; there is no permanent frame sampler.
`arrivalMaxMs` is the largest interval between callback arrivals; `lagMaxMs` compares their arrival
with the renderer's animation timestamp. `maximumAtMs` locates the largest gap within the sample,
so an initial delay can be distinguished from a pause mid-movement. `lateCallbacks` counts gaps above 1.5 times the sample's
median, and `callbackDebtMs` sums their excess over that median. This relative measure can miss a
consistently reduced cadence; it is not Apple's physical hitch metric or a display deadline count.
`chromeMs`, `readingMs`, `highlightMs`, `navigationMs` and `updateMs` record cumulative synchronous
work observed while the sample was active. They can overlap and must not be added as disjoint CPU
time. They help correlate a pause, without claiming to attribute paint/compositing costs.
`setupMs` measures synchronous preparation (and queued text preparation for a workspace arrival);
`firstFrameMs` measures time until the first sampling callback; `readyMs` until the animation is ready.
An interval maximum alone can miss
preparation before the animation starts. Repeated accessibility queries can also perturb a sample.
`startedAt` is the start time in Unix seconds, including its fractional part: an old sample
must not be mistaken for the transition just exercised. Sampling
runs only for a finite active-window transition and retains at most 240 intervals, with no history or
text. These are renderer callback intervals, not proof of compositor FPS or a 120 FPS guarantee.
The native window's `active` flag means key window; it alone does not prove visibility.
`applicationActive`, `visible` and `occluded` report AppKit's separate observations.
The measured renderer flags distinguish focus, native activation, document visibility
and the resulting motion policy. They refresh on focus/visibility changes as well as layout.
`rendererAnimations` counts unfinished finite animations created by the renderer; it excludes
CSS animations and transitions. The metric whitelist also bounds the incoming field count,
so adding a field cannot silently invalidate the larger snapshot of an open panel.
An absent motion sample in an inactive window is not a successful frame-rate measurement.
Compare combined state/stream updates and neighbour movements with the isolated transition;
callback timing alone cannot separate script, layout, paint and presentation costs.
The capability flags `rendererFrameRateAPI`, `rendererMaximumFrameRate` and `rendererHighRateCSS`
report public animation-rate APIs when exposed. Zero means unavailable/unknown, not a measured
display limit; `maximumRefreshHz` is the screen's reported maximum, not WebKit's achieved rate.
`rendererOverlayScrollbars` reports whether the reading viewport's scrollbars consume layout
space. Exact-height offscreen code containment is enabled only with overlay scrollbars and
supported CSS; ordinary layout handles space-taking scrollbars and wrapped or live code.

`library.catalogCache` reports retained bytes and lifetime hit/miss counters for the public
catalogue cache. It excludes URLs, search terms and model names. A hit means a parsed metadata
response was reused, not that inference output was reused. Developer metrics remain local.

`networkPolicy` reports the persisted master flag, capability mask (catalogues1, model downloads2,
engine downloads4, remote servers8) and current chat/acquisition request count without endpoints or credentials. Accepted and blocked attempt arrays use local/catalogues/models/engines/remote-server order; local inference remains in the local bucket, while a loopback pull is a model-download attempt. These are application admission counters, not a packet capture or OS-wide firewall.
`state.guidance.pulse` reports
the passive action cue separately from layout animations; `historyActions` and `privacy` expose
bounded geometry/colour observations of those controls.
`diagnostics off` persists only the opt-out, stops trace/observation and denies further controls;
read-only `--inspect` remains available. It supports a test handoff without dismissing first setup.

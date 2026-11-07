# Architecture

OverTone has one owner of mutable application state. C17 implements chat, persistence, models
and resource policy. A small Mac adapter supplies native APIs; WKWebView presents HTML, CSS
and plain JavaScript. There is no second backend in the renderer or general plugin framework.

## Ownership and lifetimes

`src/macos/app.m` composes the application on a serial GCD queue. That queue owns the core,
SQLite connection, library, transport callbacks and engine handles. The main thread owns
AppKit and WKWebView and receives immutable JSON projections. It does not wait for inference,
file operations or diagnostic snapshot assembly.

File discovery, metadata hashing, memory planning and backup/export have bounded workers.
They return owned results to the serial queue; they never mutate live state or borrow its
SQLite statements. Backup/export opens a separate read connection. Slow startup file work
also stays off the main thread, leaving the menu and Quit available. The real workspace window
and its WKWebView are created first; its loading surface ends on the first durable projection,
without delaying readiness or opening another window. The logo is armed only after a one-second
real wait; a faster projection cancels it before display. If shown, its mark fades on an opaque
surface before revealing the workspace, so a loading animation never overlays an already visible app.

The core admits one generation at a time, including auxiliary work. A single request initializer
resets parser, output, timing and terminal state. Increasing operation IDs reject late callbacks.
The viewed chat/branch is independent of the generation owner: navigation, edits, renaming and
classification can continue while a reply streams. Completion belongs to its original turn
and cannot override a later branch choice. Deleting that chat cancels its request first.
Interactive work preempts background titles and summaries.

Pending turns commit before network activity. Completion commits text, reasoning, statistics
and branch publication together. Partial output survives cancellation and errors. Failed storage
never reports an answer as saved; opening a current-format database marks pending turns
interrupted. Auxiliary work uses the same lifecycle without creating a second user turn.

Errors carry an event revision and, where applicable, their chat and turn IDs. Ordinary state
updates do not create new error events. The renderer presents a saved response failure at its
turn, keeps request feedback in the active context and never treats another chat's failure as
a paging failure. `feedback.js` supplies the shared presentation and dismissal rules; progress
belongs to the busy command. Storage failures remain visible and disable unsafe writes.

The library schedules one active acquisition and up to 64 persistent waiting jobs. An interrupted
active job reopens paused. Pause retains verified partial bytes; Cancel discards them; clearing
finished activity does neither. Selection, acquisition and discovery have distinct operation
identities so stale work cannot replace a newer result.

Idle owner maintenance runs once per second, switching to 100 ms during active operations.
Commands and transport completions request a coalesced render immediately. Streaming projections
are coalesced at 33 ms and do not query SQLite. Hardware is sampled every two seconds. The local
inspection listener sleeps on descriptors; the engine supervisor uses kqueue for control closure
and child exit instead of periodic polling.

A renderer navigation starts a new epoch, invalidating acknowledgements from the previous page.
After loading, native code resends translations, chrome and current state. A terminated WebKit
content process is reloaded once; repeated termination within a minute ends recovery with a
native error rather than an endless restart loop.

## Persistence and reading

`store.c` owns the current schema. SQLite uses WAL, full synchronous writes, Mac full-fsync,
foreign keys, STRICT application tables and defensive mode. User input is bound, never SQL.
FTS5 indexes conversation text. There are no experimental-schema migrations before the first
release: recreate disposable profiles when the schema changes.

One database holds conversations, settings, library references and acquisitions. Weights remain
files. A turn has an immutable parent, an input identity, a remembered continuation and a selected
chat head. Regenerations share the input identity; edited prompts create a new one. Failed edits
preserve the old head. A turn records the model and protocol actually used.

Browsing filters, ranks and pages in SQLite. Excerpts are produced only for the returned page.
Indexes on chat/date and nonempty model/protocol provenance avoid scanning answer bodies for
list metadata. A single browse projection is invalidated by changes to chat/turn rows, rollback, or another
connection's SQLite data version. Download progress and unrelated preferences do not invalidate
it. The store owns the SQLite update/rollback hooks; history tables have rowids and mutations
use bounded DELETE/UPDATE statements, without truncate-style DELETE or REPLACE. Changes to that
contract must explicitly invalidate browsing. User browsing does not retain a growing set of cached pages.

`transcript.js` keeps at most 96 turns in eight-turn pages. A soft 2 MiB text budget trims distant
entries above 24 retained turns. Adaptive prefetch starts before the reader reaches a boundary;
inserting or evicting a page preserves an element/offset anchor. Up to 20 chats retain transient
reading anchors. Hidden alternate branches and full histories do not accumulate in the DOM.

The transcript lives in a local child document with its own style/layout tree. The same DOM
nodes and handlers are retained; navigation does not restyle a long answer through the shell's
ancestor state. `reader.js` owns this boundary, mirrors appearance/language and registers shared
events, selection, coordinates and focus for both documents. The bundled CSP applies to the reader;
answers still use text and DOM construction, never model-supplied HTML or scripts. There is one
conversation cache and one C owner, not a second application or state replica.

Spatial transitions use compositor transforms where possible. Inspector clipping and content
translations are opposite effects submitted together after geometry reads; text and corners are
not scaled. History filters use the same opposite-translation principle for their reveal edge;
the reading viewport moves with it, without a height tween or a row layout on every frame.
Changing surfaces use three paint sections so their borders and shadows resize without
stretching glyphs or corner radii. Descendant movement is relative to its moving parent.
Prompt suggestions coordinate that same surface motion with the reading edge, preserving
the destination scroll range and cleaning up on completion, navigation, resize or hiding.
The composer keeps the full conversation width and one action row. Context inputs, response
modes and editing/sending have stable groups; only secondary modes enter the anchored overflow
when space runs out. Disabled future capabilities stay disabled in both locations. The same
buttons move rather than creating duplicate state or handlers. Input grows to a bounded height;
review substitutes one editable field and shares the normal send and input-budget checks.
Catalogue column changes use one opacity timeline around a single rearrangement; their
controls stay live rather than being replaced by a screenshot.
Explicit navigation to a nearby conversation edge uses WebKit's native smooth scroller.
For a destination more than three viewports away, a short animated arrival avoids crossing
unpainted tiles at extreme speed. Both paths support interruption, moving destinations and
a bounded fallback for older systems without `scrollend`.
JavaScript callback sampling observes motion only in developer mode; it does not drive its cadence.
Code colouring observes entry into the reading viewport with IntersectionObserver. Scroll
events do not scan code geometry or force skipped text to lay itself out.

Context follows one parent chain within a conservative byte budget. It is not an exact model
tokenizer. Titles use only the first user question; manual titles take precedence. Invalid,
incomplete or paragraph-length generated titles leave the fallback intact. A summary belongs
to its exact branch and is labelled historical assistant context, never a new system instruction.
Limited summaries and prompt improvements carry a review state rather than appearing complete.
Automatic editing uses fixed task instructions, independently of the assistant's reply style.
The first question or selected history is formatted as bounded source text inside one user
message, not replayed as live chat turns. A final task instruction follows that source. The
shared local language callback examines the source, and sampling uses temperature zero.
Titles and summaries retain this framing. Prompt improvement sends the draft and bounded
history and detected language as separate JSON data. It asks for relevant requirements on open
tasks, light wording corrections on already specified requests, or no change when sufficient.
Only the selected branch is read, with a configurable recent-turn limit and byte budget.
No turn, summary, setting or tool action is written by this request; accepting changes only the
draft. The main chat later receives only the text the user chooses to send. Document and tool
workflows remain unimplemented, so no hidden retrieval or access is implied. A null proposal preserves the original byte for byte. C validates the JSON,
UTF-8, size, source literals and detected language; invalid or truncated proposals leave the
original draft available. These checks do not establish semantic correctness. The editable
review is required, and closing or reopening it does not issue another request. Task results
are bound to their source draft, conversation, branch and model so late replies cannot overwrite
unrelated work. No extra model, search, or automatic self-critique loop runs for prompt improvement.

Archive and trash are readable but cannot accept messages. Restore, classification and bulk
operations are atomic and preserve the open chat. Completed replies outside the viewed chat
remain unread until that chat is shown.

## Files and recovery

GGUF import records a canonical reference without copying weights. Removing a reference and
explicitly deleting its file are separate choices. File deletion checks the recorded identity;
external caches remain managed by their engine. Owned downloads use generated profile-local
names and device/inode checks. Shared references prevent premature cleanup.

Discovery compares size, modification time and device/inode on a worker. A changed file with a
recorded SHA-256 is rehashed through an open descriptor. Both descriptor and path must still
identify the same unchanged bytes before registration is renewed. Unchanged large models are
not rehashed on every launch. Missing files and Ollama manifests become unavailable and can
be rediscovered later.

A repeat download verifies a separate file, then switches the existing reference in one SQLite
transaction. Model parameters keep their identity. Failure retains the previous model; obsolete
managed files are removed only after the commit and ownership checks. Startup retries interrupted
cleanup. Imported originals are not deleted by replacement.

Backups use SQLite's backup API in a private sibling directory. They validate the current schema,
integrity, foreign keys, branch ownership, history states and structured records before publishing.
They reject a head or edge that would mix conversations, and a chat outside all three history views.
They flush the file, publish it without overwriting and
flush the destination directory. Preexisting destination journals/WAL files are rejected and
never deleted. Export writes NDJSON. Neither backup nor export includes weight files.
Restore validates and stages a current-format database; restart preserves `previous.sqlite`,
checkpoints the live database and atomically installs the staged one. Schema validation derives
from the same constructor as a new profile, without executing imported DDL.

`location.c` owns data-folder changes. Private location/profile locks exclude another instance.
Startup copies current-format databases and managed weights on a worker before committing the
new location. Ordinary copies verify SHA-256; confirmed APFS clones share source extents.
External references stay external. Failure or cancellation leaves the source authoritative;
the original folder is retained after success.

## Protocols, acquisition and process ownership

Ollama NDJSON and OpenAI-compatible SSE share lifecycle rules but keep explicit framing and
request fields. URLSession supplies HTTP/TLS. Inference redirects are rejected, preventing prompts
or authorization headers from silently moving to another destination. Catalog/download requests
are separate and carry no inference credentials. The native transports share one error classification
for timeouts, TLS and ATS restrictions; discovery and streaming present the same meaning.

`connection.c` reads bounded, untrusted model lists and metadata. The Mac connection adapter
normalises the server address, detects Ollama or OpenAI-compatible APIs and requires selection
from the returned list before applying a connection. Model-scoped `/props` and Ollama's
`/api/show` preserve the context and reasoning controls the server actually reports. Optional
OpenAI metadata may be absent; unknown capabilities stay unknown. Discovery verifies connectivity
and advertised identity, not answer quality or full protocol support, and never pays for a hidden
inference. A runtime can restrict context below its model's training limit. The client does not
enlarge a remote allocation or infer its GPU/RAM from this Mac.

Discovery has one cancellable request at a time, revision-checked callbacks, a 1 MiB response
limit and a 256-model list limit. Per-request and resource timeouts are bounded. Editing the
address or clearing/changing the selected model invalidates earlier confirmation. TLS validation
is supplied by URLSession, with no certificate bypass or redirect forwarding. Remote HTTPS is
the default; an explicit HTTP choice is visibly unencrypted. macOS App Transport Security remains enabled:
HTTP is supported for IP addresses, unqualified hosts and `.local` domains. A fully qualified
DNS name requires HTTPS; the connection form reports that requirement explicitly. Discovery and generation share
address and destination policy, including reverse-proxy prefixes.

Session credentials are an immutable server/key pair. Generation and discovery retrieve a key
only for the exact canonical address, including scheme, port and path; changing server cannot
reuse the previous key. **Remember on this Mac** writes a non-synchronising generic-password
item to the system Keychain through Security.framework. The legacy login Keychain is used because
the local ad-hoc build has no provisioned application identifier. Its public, process-local
noninteractive policy suppresses ACL dialogs; authentication-context/UI-skip queries alone did
not suppress them in the native test. A single narrowly scoped deprecation suppression documents
that OS boundary. No access is granted and no biometric/user-presence requirement is added.
Background restoration fails closed without a prompt. Session-only use does not overwrite an older remembered
key. Connecting without authentication and with remembering enabled removes the remembered
key for that address. The app's database, backup, logs and inspection output contain no keys.
macOS can still deny access when the Keychain is locked or the app's signing identity changes;
this is not a promise to bypass OS access control. Explicitly re-entering a verified key may
replace only that server's inaccessible item; its ACL is never broadened. A failed save remains
actionable in the form, with session-only use available.
API metadata is restored at startup and refreshed after completion or opening model parameters,
without a separate idle polling loop. External servers are never stopped by this connection flow.

The stable prefix carries the assistant's editable writing style. Apple's on-device recognizer
can add a confident language hint for the latest question; explicit language/translation requests
take precedence. One system directive carries this rule; user turns are transmitted unchanged.
UI locale does not decide the answer language. Local llama.cpp requests enable
prefix reuse; Ollama manages its own cache. External services receive no llama.cpp-specific field.
Releasing weights also releases their in-memory cache. No second translation inference is hidden
behind an answer. Model compliance and accuracy remain model properties.

A bounded prose-loop guard stops sustained adjacent repetition in answers and reasoning. Code
fences and code-like units are excluded. The partial remains readable and later context marks
the interruption. Token, cache and timing statistics are recorded only when supplied by the
engine; missing measurements remain unknown. Decode speed excludes connection/load time.

Public Hugging Face downloads use immutable revisions and advertised SHA-256; direct downloads
require an explicit digest. Resume rehashes the partial and requires a strong matching ETag and
consistent Content-Range. Changed representations are never appended to old bytes. A bounded
256 KiB buffer combines fragments before file writes/hashing. Registration happens only after
verification, file/directory flush and a no-overwrite install. Crash recovery handles the boundary
between file installation and the SQLite commit. Ordinary Ollama downloads and deletion use the engine API. Setup-owned pulls use a private per-job
staging store: immutable shared blobs are reusable hard-link inputs, partial blobs stay private, and
only SHA-256-verified contents are published to the normal store with the manifest committed last.
A Back action cancels the request and stops only that owned downloader. Superseded stages are removed
after the writer and verification worker stop; another acquisition waits for cleanup. Shared cache
content, externally owned servers and independent user work are never pruned to cancel a setup plan.

Initial setup uses the same library, installer, model registry and acquisition queue.
Global preferences retain completion, draft engine filters, activity and the committed model/engine intent;
downloads retain their durable queue states. A persistent hold marks Back, Skip, closing and restart;
no setup acquisition resumes until the user confirms again. Repeating the same intent reuses its jobs
and checkpoints. Confirming a different choice removes superseded incomplete work, retains common
required dependencies and preserves completed installations. Borrowed independent jobs are detached
and left paused rather than deleted. Draft filter edits alone never destroy the accepted plan. Downloaded models move to
the summary for actual loading; Start remains disabled until the engine confirms readiness. New, in-progress, skipped and completed are distinct
persistent states; readiness alone is not completion. Start or an explicit skip commits that decision.
An in-progress setup reopens after restart with downloads held. Returning to the model picker uses
the complete eligible shortlist rather than the single upgrade recommendation. A previous selection
outside new filters cannot replace the current results.
A persisted local model ID owns the connection independently of engine readiness or a held setup.
On restart, an unrelated previously selected model starts normally; a held target stays stopped.
An ephemeral local endpoint is never restored as a manual external server. Explicit None suppresses
startup without erasing the saved identity; an explicit server connection clears local ownership.
Manual selection/connection cancels pending automatic activation, so a late download cannot
replace a model the user has just chosen. Existing per-model parameters take precedence.

`recommendations.c` parses public catalogue metadata and ranks candidates locally by available
memory, pressure, GPU, disk headroom and the chosen activity. Both Ollama and llama.cpp are
selected for a new setup; either can be deselected in the same model step. An optional GB preference
favours sizes around the chosen value, subject to hardware/disk admission. Family retention preserves
size diversity rather than keeping only variants near one hardware target. Setup fetches current local-model
families and their sizes only after online consent; an unavailable feed falls back to the public
catalogue. Requests contain no hardware description, activity or private text. Bounded requests
have an eight-second deadline within a 45-second collection budget. Up to eight independent metadata
requests overlap, with at most 1 MiB per response. Read-ahead buffers are consumed in the original
semantic order; timing cannot change quotas or evidence. Cancellation owns every request and buffer. A five-minute fresh catalogue is reused across restarts and engine/activity filters when it covers the
selected formats. A cancelled explicit refresh retains that valid snapshot. A bounded, durable catalogue
combines popular and recently updated families. Families share capacity; variants are evaluated before
retention rather than selecting a fixed quantization or the first file. A refresh keeps the last valid
snapshot available. Background checks run opportunistically at most weekly after consent, while idle,
without creating another timer; failures without a cache back off for an hour. Manual refresh is explicit.
Provider collection reserves time for both engines within that budget; optional evidence cannot
consume the second provider’s opportunity. A C-owned collection epoch distinguishes request acknowledgement from earlier preference projections.
The initial shortlist is published together after catalogue collection, including reusable
local models; it is never first shown as an incomplete local subset. Only compatible candidates
participate: Ollama tags for Ollama, GGUF files for llama.cpp.
GGUF recommendations require an immutable revision, SHA-256, size, chat template, identifiable quantization and a standard-compatible runtime. Component files, adapters, projectors, split files, gated repositories and explicit incompatible runtime packages are excluded from automatic setup. The same eligibility applies to cached catalogues; manual discovery/import remains available. Existing usable models
participate without requiring a new download. Both selected engines are prepared, but choosing
a model does not download an additional copy for the other engine. These are capacity/relevance
heuristics, not professional accuracy ratings or exact allocation predictions.

`advice.c` owns bounded need statistics and evidence comparison. Personalization is enabled by default and can be disabled or reset.
While enabled, completed user requests contribute broad task/language needs, length, context pressure,
reasoning use and observed latency/throughput. No prompt copy is retained and no extra model runs.
At most 512 samples live in the same profile, with a 30-day half-life; archived/trashed chats are excluded,
turn deletion cascades, and reset invalidates samples captured by an in-flight request. Stopping or
regenerating an answer is not interpreted as a quality rating. Explicit purpose and priority remain anchors.
Runtime observations are matched by content identity, machine/OS, context and execution settings;
they describe recent use, not a controlled benchmark or proof of answer quality. One bounded
read classifies the latest 512 samples across up to 32 installed targets, rather than repeating
JSON decoding for every target. No additional persistent cache is introduced.

Structured Hugging Face evaluation metadata retains source, task, dataset revision and setup notes.
Only comparable metrics with known direction influence relative scores. Base-model results are weak
priors and cannot establish measured dominance of a quantized file. A Pareto exclusion needs exact,
verified quality evidence and no regression in the known resource/capability/latency dimensions.
Missing optional fields do not disqualify a model; missing decisive measurements cannot certify a winner.
“Verified by OverTone” remains a separate content qualification. The shortlist and unobtrusive notice
are suggestions; neither downloads nor active-model switches occur without user action.
The same ranking emits two independent projections: the setup shortlist and the single qualified
upgrade. Opening/closing either surface cannot change the other projection.
After setup, one candidate must improve on the local reference by a material ranking margin;
there is no quota to fill. The banner and detail view use the same qualification. Opening is
based on the cached selection and freezes the displayed candidate; explicit refresh or the idle schedule updates the
catalogue. Hiding a notice does not immediately replace it with another notice. Ignored identities
can be reconsidered from the recommendations view. Matching corner badges on Models and Recommended for you, plus an in-page notice, identify
an unread recommendation. Viewing it stores only its content identity locally; it does not dismiss
the candidate or require a network request. Disabling notices hides both signals. Offline advice offers explicit catalogue activation
in place, through the same network-consent setting as setup.

Setup completion is reused when current data already contains a configured connection or model.
A temporarily unavailable engine is not a fresh installation. The essential guide uses shared anchored
surfaces and a dimmed SVG spotlight over the complete explained area and its action. Navigation and
opening/closing dialogs are performed by the user through the highlighted controls. Inert branches and
capture guards block unrelated interactions without stopping core work. Steps remain grouped by topic;
Settings and ordinary download/pause controls are excluded. Ending or skipping leaves the current
screen in place. The guide keeps navigation and dialog opening user-driven; its parameter step advances after the normal Save acknowledgement closes that dialog, and contains no schema
migration or compatibility path for pre-release test formats.

Tested-content identities control the “Verified by OverTone” badge and prefer a verified candidate
when scores are within 3%, weights within 20%, and reasoning/context capabilities are comparable.
They never supply catalogue entries. GGUF checksums and complete Ollama manifest hashes identify
the tested content; a matching name alone cannot earn the badge. A badge qualifies the tested chat integration,
not future image/document/tool workflows or accounting/legal accuracy. Model licenses are separate
from the application's Apache-2.0 license; the model information links to its original catalogue.

The shared Recommended preset resolves context/input/output against actual model geometry and
current resource headroom after discovery/download. It leaves room for useful input and reasoning;
choosing an activity can change both candidate ranking and the context target. Resolved initial
parameters are stored per model, even when two models get identical values. Saved user choices
are preserved. Pull progress is retained by content identity across pause/restart, and is removed
when the transfer is cancelled or completed.

`package.c` pins the optional Ollama and llama.cpp archives by version, size and SHA-256. The ordinary download
worker verifies the same open descriptor again, uses macOS bsdtar without a shell, validates the
private output tree and staged executable, flushes it and atomically publishes a versioned engine
directory only after the engine's help probe succeeds. Extraction,
help probes and cancellation share the bounded process runner. Interrupted staging is cleaned at
the next installation; repair can replace only the private owned directory. Components stay in
the stable profile anchor, beside its location manifest, when conversation/model data moves to
another disk. Executable paths therefore remain valid without copying runtimes or retaining a
second installation. External applications,
launch agents, services and package managers are neither installed nor modified.

Choosing a local model starts its selected supported engine automatically. Server readiness and
model readiness are separate. GPU admission waits for observed acceleration. An asleep model
retains its selection and wakes on the next request. Model parameter changes that affect execution
recreate the owned engine, preventing old contexts or placement from surviving unnoticed.

Native executables start with argument arrays and a small environment, never shell strings.
A private mode of the same executable supervises each owned process group. Parent closure,
crash or child exit triggers termination and reaping, with a grace period before SIGKILL. The
leader PID stays reserved until descendants are cleaned up. Readiness checks both listener
ownership and a protocol response. An external server is never treated as an owned process.
Acquisition-only Ollama helpers have separate endpoints and stop after 30 seconds without work;
they do not replace the inference engine. No particular engine is required to build OverTone.

## Model filters and catalogue facts

The renderer filters the bounded library projection locally; it creates no requests or second model
registry. `filters.js` owns the anchored category/value surface and removable chips. Hidden native
selects own each choice once; declarative facet definitions supply labels, options and callbacks.
The same surface serves Installed, Activity, provider searches and each family's variants. Counts
and option changes update in place; they do not close the panel. Navigation closes surfaces whose
context has changed. Explicit zero values stay visible and disabled, except an active zero-result
choice that must remain clearable. Clear preserves query, sort and usage priority.

Installed exposes availability, GGUF/Ollama library, maximum file size, minimum supported context,
quantization families, known capabilities, verified content identities, warnings and declared
license. Q, IQ, ternary and microscaling formats are distinct; matching bit widths do not imply
matching quality. Unknown values never qualify as known capabilities. The selected-model facet
means the model chosen for chat, not the row currently inspected in Details. Unverified changed
files are excluded from Available. A scan refreshes facts; it cannot recreate missing files or
automatically discard references to potentially disconnected storage. Restoring verified content
clears the warning. Explicit removal hides an external reference so discovery does not re-add it.

A single-valued facet counts matches ignoring its own selection. Quantization checkboxes combine
with OR, capabilities with AND, and different categories with AND. Counts cover the current query;
only 40 matching Installed rows are rendered initially, with explicit increments of 40. Numeric
regional collation orders names, and unknown sizes sort last in either direction. The active model
is looked up once per filter pass, avoiding quadratic work as the library grows.

The C core shares provider metadata parsing, precision classification and usage hints with setup.
Hugging Face receives pipeline type, declared license, maximum *total* parameter count and sorting.
Responses are checked against these constraints again: a parameter count inferred from a name is
not sufficient for the numeric filter. Ollama receives its supported capability and popular/newest
sort criteria. Provider filters are part of the metadata-cache key. A raw full HF response retains
Load More even when local validation excludes some rows. The existing bound is 100 family results
per search; no global total or exhaustive catalogue coverage is claimed.

A family's own filter button scopes size, context, precision, acquisition status, execution and
resource-admission filters to that family. Known projector/adapter files and standalone split
shards are not offered as self-contained chat models. Filename screening is not a substitute for
GGUF/runtime validation. Ollama tags that do not disclose quantization retain Unknown; no extra
per-variant network scan or model download is used to fill gaps. File size is not a memory estimate.
The resource filter uses the same preliminary weight-plus-reserve admission policy as setup and
current free disk space with a reserve. It does not certify the full runtime footprint for every
context setting. Actual per-model memory estimation and load admission remain separate.

The six usage priorities share setup's vocabulary and bounded metadata hints. In manual discovery
they reorder available results locally; they do not certify professional accuracy or replace the
evidence-based recommendation system. Switching this priority creates no HTTP request. All
catalogue access, including cache reuse, remains behind explicit Internet consent.

## Memory and resource policy

Metal reports whether memory is unified. CPU and GPU capacity are counted once. Metal's recommended
working set is a policy input, not extra VRAM. The UI distinguishes used system memory, an owned
engine's measured physical footprint and expected model allocation. Engine memory is already
inside used memory. Hatched additional demand credits only observed residency, never stale buffer
reservations. Unknown measurements remain unknown.

`memory.c` provides a fast metadata estimate and an optional engine plan. GQA key/value dimensions
take precedence over inferred head geometry. Total MoE weights are not divided by active experts;
hybrid/recurrent, sliding and MLA metadata invalidate uniform KV assumptions. Unknown compute
overhead is not invented as a percentage. A compatible optional `llama-fit-params` can plan buffers
without loading weights. Plan and process receive the same fit target derived from the memory
budget and hardware recommendation. The budget is a placement/admission policy, not a hard allocator
sandbox for every provider.

Observed weight/cache/compute banks refine estimates. Independent banks add; resized reservations
replace earlier reports. Overlapping mmap spans are capped against weight storage, while CPU
repacking remains an allocation. For compatible loaded settings, cache precision and geometry
can project a changed context. Crossing unknown sliding-cache boundaries invalidates that
projection. Compute calibration requires compatible observations and remains approximate.
Allocation plans and physical footprint measure different things and are never added together.

The last configuration can retain two observed allocations in the model's existing metadata,
bounded to 4 KiB per record.
Only complete owned-runner observations qualify: known CPU/full-GPU placement, context and
component totals. Ollama's runtime digest must match the registered model. Model/file, executable,
OS build, hardware, budget and effective-thread identities bind each record; a local manifest
stamp also invalidates an offline Ollama observation before a service refresh. File-size accounting
does not identify service content. Unknown identities fall back to the ordinary estimate.
No model is loaded or benchmarked to populate this cache, and unchanged observations do not write.

An exact context reuses its observation. Interpolation stays between two measured contexts with
matching batch, placement, fixed cache banks and cache slope. It does not extrapolate an unknown
graph or cross a sliding-window boundary. These records stay in C; renderer model metadata excludes
memory observations and CPU-qualification internals. Neither supplies residency credit or replaces
post-load checks. Unknown uniform-KV geometry is not a proven minimum for admission.

First-use plans remain approximate. A runner's no-allocation planner can reserve a different output
shape or count mapped weights differently from its server. A matching fit-target argument does not
prove identical placement. OverTone does not subtract an empirical percentage, load weights behind
the user's back or bind to private engine ABIs to hide that uncertainty.

One memory worker retains the latest draft, debounces edits for 250 ms and caches 16 results.
Keys include model/executable identities, context, GPU mode, memory budget, effective threads and
hardware capacity. Stale results cannot replace newer ones. The optional planner has a five-second
deadline, cancellation, a 256 KiB output bound and process-group cleanup. A separate compact memory
projection updates sliders without rebuilding the catalog. Changes to budget or threads participate
in this path even before saving.

Model parameters persist per local model, or per endpoint/model/protocol for APIs. Global startup,
writing and appearance preferences are separate. Startup supports a chosen model, last used or None.
A 100% budget means maximum usable policy within current availability, pressure and the OS reserve.
Critical pressure prevents a new managed GGUF load and postpones background annotations. Thread
counts default to available performance cores and are bounded by actual hardware. Known-size
downloads check SSD space before starting.

CPU suitability is qualified on an explicit CPU choice, not through model-name lists or a parameter
count cutoff. Metadata bounds a short probe: at most 24 output tokens/eight seconds; observed decode
must reach eight tokens/s within three seconds. Results are cached by compatible model, engine,
settings and hardware identities. No background benchmark scans the library. Unqualified means
not supported by this interactive policy, not physically impossible to execute.

## Presentation and safety boundaries

The renderer has no filesystem or direct network authority. Native commands validate their payloads.
Model text becomes text nodes and a bounded Markdown subset, never executable HTML, SVG, CSS or
JavaScript. Remote images are not loaded automatically. Only a clicked HTTP(S) link opens a browser.
Optional fenced `overtone` descriptors accept `bars` or `flow`: at most 8 KiB, 12 finite nonnegative
bar values, or two to four flow steps. Invalid descriptors remain readable code.

Markdown and lexical code colouring have bounded work and literal fallbacks. Highlighting visits
two visible/nearby blocks per pass, at most 131,072 UTF-16 units and 8,192 coloured tokens per block.
Hidden/collapsed readers do not spend work colouring text. Stream updates reuse code and message
nodes. Copy extracts content, excluding controls and decorative gaps. Oversized code stays readable.

On supporting renderers, closed unwrapped code can skip offscreen layout. Its intrinsic block size
comes from the source's literal line count and the actual CSS line height, including empty/trailing
line rules. Wrapped/streaming code, unusual segment separators and space-taking scrollbars
retain ordinary layout; unsupported CSS falls back normally.
Navigation measures its own controls before revealing a large workspace and prepares visible text
before starting the arrival animation. Docked inspector layout still follows a shared moving edge.
Non-accelerated layout and renderer callbacks can remain near 60 Hz even on a faster display;
callback diagnostics do not measure physical compositor presentation.

Shared presentation primitives own motion, visibility, popovers/dialogs, tooltip delay, control
geometry and interaction roles. `appearance.js` owns paired ink/fill colours; `app.css` applies them
in one interaction layer. Selection, hover and destructive/constructive meaning remain distinct.
`locale.js` translates authored labels from ten bundled catalogues; model text and names remain literal.
Regional formatting uses local OS information, with independent user overrides and no geolocation.

AppKit owns window corners and native controls. The application inherits OS appearance while only
its window overrides the chosen theme. System preview therefore remains independent. The installed
icon uses the approved native Assets.car and an ICNS fallback for macOS 14. Its editable source is
OverTone.icon. The transparent UI mark keeps its approved fixed palette under every theme/accent.
There are no runtime icon mutations, Finder extensions or icon-cache tools.

## Bounds and extension points

Current limits include 16 KiB prompts, 128 KiB combined answer/reasoning, 64 KiB stream frames,
16 MiB stream traffic, JSON depth 64, 30-chat pages, 4 MiB catalog responses, 1 MiB readiness responses
and a 32 MiB GGUF metadata span read through a 64 KiB window. Inventory visits at most 10,000 entries,
eight levels and 256 models, with a five-second traversal budget checked between filesystem calls.
Library projections expose up to 1,000 models and 1,000 jobs. On truncation the UI shows a warning;
the current/requested model and nonterminal jobs take priority. Discovery checks at most 1,000
registered files in one pass and does not mark unvisited entries missing after an incomplete scan.
These limits bound work; they do not impose a deadline on a blocked filesystem operation.

Add a responsibility where its decisions or lifetime become independent. Do not split orchestration
into forwarding layers or invent future modules. A new feature should add its state/command/view
without rewriting provider framing, process ownership or shared interaction rules. See
[development](development.md) for verification and UI conventions, [diagnostics](diagnostics.md)
for local inspection, and the [product contract](product-contract.md) for implemented/prepared scope.

Only yyjson is bundled C library code. System frameworks provide storage, TLS, rendering and hardware
APIs; engines and model licenses remain separate. [Third-party notices](../THIRD_PARTY_NOTICES.md)
cover included code/assets. Windows and Linux are future adapters, not current build targets.

Internet access is controlled by one persisted master permission and a bounded capability mask in
`preferences.c`: catalogues, model downloads, engine downloads and remote servers. The switch is
binary; partial permission sets are shown as limited access. Turning it off retains those choices and
revokes active remote work. A narrow consent grants only its stated capabilities; it does not re-enable
other saved permissions. Future capabilities need new explicit permission rather than inheriting an
old master opt-in. The renderer cannot issue network requests. C acquisition admission and the native
transport check the same capability before a task is created and again at redirects. Local connections use a separate ephemeral session with no proxy/PAC; remote sessions are created only after admission and are invalidated at a general opt-out. This leaves local inference alive when Internet is revoked. Loopback inference
and discovery work offline; loopback commands that trigger an engine download still require the
download capability. Discovery rejects redirects. Local HTTPS trust validation uses system validation
without network certificate fetching. This policy governs OverTone’s requests and owned engines,
not independent activity of a separately running user-owned server or macOS.

Setup can prepare only missing selected engines, with no model or catalogue access. Its committed
engine mask remains independent of edits made while going back. A nonzero `setupTargetEngines`
with an empty target represents this operation. Existing engine paths are reused. Progress,
permission checks, acquisition, summary and Finish share the regular setup lifecycle. `follow`
resumes the exact committed target/mask without depending on a catalogue refresh or duplicating
an acquisition. Its required network scopes come from incomplete work and missing dependencies;
a complete local GGUF/package partial can finish offline. A removed completed engine is acquired
again only after current consent. Catalogue changes do not change the committed operation. Engine-only Finish depends on its engines, rather than inference-model readiness.

Action cues in the optional guide repeat an outward outline pulse while the highlighted action
is pending. A valid action, ending the guide or hiding it stops the cue; returning focus resumes it.
Only the passive cue changes opacity/scale; the control and text stay still. Reduced motion keeps
the steady outline. Passive attention effects do not block layout settlement or schedule repeated
layout reads. Skipping or closing the optional guide stops all its motion.

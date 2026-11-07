# Product contract

OverTone is a personal, local-first desktop workspace. It brings together conversations and a
user-selected model engine; documents, knowledge, research and automations are future extensions.
The current implementation is described in the README. A prepared screen or architectural goal
is not an implemented feature.

The interface should remain recognisable and simple. Advanced configuration belongs in details.
A local engine is preferred, but supported external APIs are an explicit user choice. No engine,
model or hardware configuration becomes mandatory because it is convenient for development.

| Capability | Observable requirement |
| --- | --- |
| Conversation | Progressive output, a coherent terminal outcome, stop, editing and alternatives without duplicated messages. |
| Continuity | Committed conversations survive restart; search and branch selection preserve their meaning. Errors are not reported as saved data. |
| Organisation | Automatic titles respect manual choices. Summaries belong to the correct branch and record lost context. Background work yields to interaction. |
| Models | Search, download, import, selection and removal have visible progress and controlled effects. External files are not implicitly owned. |
| Engines | Model identity, protocol and process ownership are distinct. Closing the app does not terminate a user's external server. |
| Setup | Optional and persistent; skipping leaves normal Settings/Models available. Reuse precedes installation. Explicit downloads share the normal queue; a final summary identifies the usable configuration. |
| Resources | Work and buffers are bounded. Resource policy reflects the actual device; unified memory is counted once when detected. |
| Data | Private local storage, explicit network access, consistent backup and recoverable restore. No credentials in diagnostic output. |
| UI | Readable text/code, selection/copy, keyboard access, reachable controls and responsive layout. Generated content never becomes executable HTML. |
| Observability | A non-graphical local interface reflects actual operations, resources and failures with bounded, sanitised output. |
| Maintainability | One active implementation per responsibility, few necessary dependencies, focused checks and a standalone build. No historical tools or personal evidence required by a clone. |

C17 owns application logic. System rendering and a small Mac adapter are deliberate exceptions to
an all-C source tree. Mac is the current implementation target; Windows and Linux remain future
desktop targets with their own acceptance work.

Until the first release, test data are disposable. Keep only the current schema; do not accumulate
migration code, compatibility branches or obsolete fixtures. Cleanliness applies to the whole
project continuously, not just its database format. Every retained module, test and tool must
serve a current behaviour or a concrete risk.

Archived and trashed conversations are excluded from the ordinary conversation list and
search. They must also be excluded from any future cross-conversation retrieval service;
no such retrieval service is implemented by the current application. Permanent deletion
removes the conversation, its alternatives and its full-text search entries atomically.
Emptying Trash applies to every trashed conversation, independently of the visible page or
search filters. It requires confirmation and preserves active and archived conversations.
Restore all in Archive restores every archived conversation in one transaction, independently
of search or paging, while preserving Trash and the selected document. Both history actions
remain visible while filters and the conversation list move underneath their fixed footer.


## Interaction identity

Search fields put their magnifier on the left and filters on the right. A search scope
chooses a catalogue; filters refine its content; importing a file or link is a separate
action. Labels describe what can be entered without duplicating instructions below.

Hover gives restrained feedback over the complete actionable area. A selected item keeps
its selection under the pointer; keyboard focus is a separate inset ring. Filled primary
actions, destructive actions and quiet icon actions have distinct roles. The common
surface/control radius is 12px, with smaller concentric insets derived from its padding;
AppKit owns the actual outer window corners. New controls reuse these tokens.
Concentricity applies to the content group and hit target as well as the outline: align icons,
labels and selection surfaces with balanced insets, without sacrificing room for long labels.

Motion explains appearance, dismissal and travel. Repeated state projections do not restart
a transition to the same destination. Short transitions can be interrupted; reduced-motion
preferences remove travel. Visual effects must not delay the underlying operation. Code
wrap is off by default; its per-block override does not change the global default in Chat
settings. Tooltips supplement icons after a delay, do not repeat visible explanatory text,
and follow the user's preference, including the model status.

Archived and trashed conversations are read-only: reading, copying and switching message
versions remain available; sending, editing and regeneration require restoring the conversation.
A short read-only notice and Restore action replace the composer. A new conversation returns
to the ordinary history without inheriting an archive/trash search filter.
Moving a conversation preserves the open document and reveals it in the destination history.
A turn already generating may finish; moving it does not interrupt unrelated work.

An explicit repeat download retains the existing model until verification and registration
succeed. Replacement preserves the model identity and its saved parameters. Old files managed
by OverTone are retired after the new reference commits; imported originals remain external.
Local discovery reconciles a changed filesystem identity only after its recorded SHA-256 matches.
In-place changes are checked through size and modification time; unchanged files are not rehashed.

Setup acquisitions have explicit ownership and a reversible draft. Back, Skip, closing or restart
hold unfinished work. The same confirmed model/engine plan resumes it; a different confirmation
discards obsolete partials, preserves installed resources and reuses dependencies still needed.
Unrelated acquisitions remain independent. Changing filters never silently replaces a committed plan.

Model filtering uses factual metadata and explicit unknown values. Store-wide search criteria and
family-specific variant criteria have separate scopes. Active filters remain visible/removable,
including after their result disappears. Rescanning refreshes availability without silently removing
references to missing external files. Notices are application messages, displayed as readable
paragraphs in context; they are never formatted as generated model content or empty key/value cells.

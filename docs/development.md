# Development and testing

English | [简体中文](development.zh-CN.md) |
[Documentation index](README.md)

## GUI and CLI feature parity

New or changed user-facing business capabilities must update the corresponding
GUI and CLI entry points together, including help, documentation, and regression
tests. Reuse shared application queries/services and strategy rules; do not
implement separate behavior in each frontend. Presentation-only controls need
not have identical interfaces, but any intentional capability gap must be
documented rather than silently deferred.

Blocked-item inspection shares `load_blocked_items` for offline state access
and `matches_blocked_item` for exact reason and searchable-field selection.
Frontends supply only the text matcher, preserving Qt's Unicode-aware matching
in the GUI and the CLI's documented ASCII matching without duplicating field
selection or database access. CLI output continues through the existing
Console events and text/JSON/TUI backends.

## Shared transfer progress

Upload and download reporting reuse `TransferProgressEstimator` and the existing
progress throttle; `DownloadProgressEstimator` remains a compatibility alias.
Pass logical completed bytes separately from current-operation transferred
payload bytes. Saved offsets and reused files can advance completion but must
never manufacture speed or ETA. Upload retry attempts contribute their actual
sent bytes without double-counting repeated callbacks.

`SyncContextEvent`, `SyncPhaseEvent`, and `SyncSummaryEvent` carry UI-neutral
direction, phase, and round counts. Operation lifecycle events remain separate:
an upload batch completing does not finish synchronization, and a Monitor round
finishing does not stop the monitor. Terminal GUI results still come from worker
completion after cleanup. New event variants must be handled by the text, JSON,
TUI, GUI forwarding, and GUI history consumers together.

## Cross-process synchronization ownership

Keep `RuntimePreflight` alive across the entire frontend operation. It validates
authentication and paths and holds the state-directory OS lock, but does not
create/probe/chmod synchronization directories. After resolving the account/Drive
root, shared synchronization/Monitor services acquire a root lease before opening
the item store or preparing data directories. Single-file download and cleanup use
the same acquisition helper. Monitor rounds reuse its held lease, including while
waiting; never reacquire it for each round.

The per-effective-UID private local registry uses a short-lived `flock` gate to
serialize lease inspection, stale removal, and registration. Each live lease has
its own exclusive OS lock, canonical root path, root/ancestor device/inode
identities, and bounded versioned owner metadata. Conflicting lexical roots or
filesystem identities reject acquisition. Only an OS-unlocked lease can be
removed; live malformed metadata fails closed. RAII descriptors and `O_CLOEXEC`
preserve cleanup/crash/exec behavior without relying on PID liveness.

`RuntimeBusyError` carries structured ownership facts. Frontends report them
through shared `runtime_busy` section/message events rather than parsing exception
text. Regression coverage uses exec'd subprocesses for direction pairs,
overlapping/missing/aliased roots, atomic startup races, crash release, and shared
service ownership during Monitor waiting.

Reuse `canonical_directory` for directory canonicalization, including trailing
separator aliases of missing roots. Directory validation returns the canonical
sync path so lease acquisition does not resolve it again. Application preparation
delegates creation and permission handling to the existing `prepare_sync_root`;
its write probe remains separate, and independent engine boundary checks stay
enabled. Lock files use `open_path_no_symlinks`, while lock inspection and
`SafeSyncRoot` share the utility `FilesystemIdentity` representation.

Lock metadata reuses `read_private_file`'s descriptor overload. It borrows the
locked descriptor and uses `pread` without changing its offset or reopening its
path. The path overload delegates to the same validation/reading code. Metadata
writes must remain in-place: atomic replacement would change the inode and
detach the path from its held OS lock.

## Shared Monitor control

`monitor::request_control` and `ControlServer` are UI-neutral local IPC shared by
CLI and GUI controls. Production Monitor owns the server inside `run_loop`,
under the outer state/root locks. Its IPC-only thread remains responsive while
the scheduler runs a synchronous callback; it never runs another sync engine.
Versioned, size-limited Unix seqpacket messages reject arbitrary configuration
and directory fields. Both ends verify `SO_PEERCRED`; private-directory and socket
validation reuse filesystem-security helpers. Cleanup checks endpoint identity
before unlinking, preserving replacements.

Manual pending requests use a mutex and eventfd. The combined cancellation token
joins external and IPC stop requests. Manual/local/remote pending work uses the
existing state machine and coalesces into a serial catch-up round. Watch the root
(or its nearest existing ancestor during first creation) before initial sync;
drain queued changes before rebuilding watches. A file created and deleted in the
same drained batch is transient work, preventing filesystem capability probes
from continually retriggering synchronization without filtering user filenames.
Stop acknowledgement precedes callback cleanup and lock release; later triggers
are rejected. Round failure/blocked results remain observable without stopping
the existing monitor policy.

Exec'd subprocess regressions cover responsive status, request coalescing,
local changes, cancellation, locks through cleanup, CLI offline text/JSON, crash
restart, invalid packets/replies, endpoint permissions/replacement, and timeouts.
Protocol v2 adds an opaque per-server `instance_id`; `ControlTarget` freezes
PID plus instance identity for GUI requests, including before modal stop
confirmation. The server rejects stale identities before mutating scheduler
state, even when a new Monitor shares the previous PID. Older v1 tasks must
restart after updating. CLI snapshots expose the same identity.

GUI-owned monitoring reuses `SyncController::start_task`, runtime preflight,
`app::monitor_account`, event forwarding, cancellation and completion. The
separate `MonitorController` only queries/controls IPC on one bounded background
worker. Configuration generations discard outdated callbacks; Qt receives results
on its event thread. One-second polling never starts a second synchronization engine.
`inspect_runtime_owner` checks only an existing state-lock inode, reusing safe
open/flock/metadata helpers without creating or rewriting it. Socket disappearance
therefore cannot masquerade as released resources. Unexpected errors remain visible
and uncertain ownership fails closed. Invalid IPC blocks new synchronization tasks;
settings/account recovery is allowed only when lock inspection establishes free
resources. An empty state-directory setting is not probed as the working directory.

Closing the GUI cancels/waits only for its own worker. External stops require
confirmation; external snapshots never fabricate direction, progress or round
timestamps. Offscreen and exec'd subprocess regressions cover owned rounds,
blocked results, trigger/stop/close, external ownership, read-only lock inspection,
cleanup occupancy, unsafe endpoints and overlapping roots across state directories.
Coverage regressions also switch configuration while IPC callbacks are pending,
replace a same-PID Monitor during stop confirmation, and check recovery from
stale/refused endpoints, rejected controls and corrupt occupied-lock metadata.
Obsolete callbacks must not publish snapshots or acknowledgements for the new selection.
Systemd unit management remains separate from stopping a running Monitor through IPC.

## GUI localization

Enable `ONEDRIVE_BUILD_GUI=ON` with Qt 6.8+ and the Linguist tools.
`qt6-translations-l10n` supplies `qtbase_zh_CN.qm` for Qt standard buttons and
dialogs. Missing GUI tools/catalogs fail configuration only when the GUI is
enabled. For a custom Qt installation, CMake's `ONEDRIVE_QTBASE_ZH_CN` cache
entry can point to its matching catalog.

GUI strings use `tr()` or `QCoreApplication::translate()`; account and
configuration view models expose enums rather than translated strings, so
their headless tests remain Qt-independent. `Localization` installs the
resource-bundled translators before constructing widgets and keeps them alive
for the application's lifetime. Simplified Chinese system locales use the
`zh_CN` catalog; all other locales fall back to the English source strings.
The GUI-only `QCommandLineParser` accepts `--language auto|en|zh_CN` before
installing translators or creating the window. `--help`/`--version` exit early;
invalid values/options fail explicitly. There is no startup selection dialog.
The session-only Language menu calls the application-owned `Localization`
controller; `LanguageChange` retranslates existing widgets and retained login
and synchronization messages without reconstructing the window, resetting form
values, or touching active workers. The CLI/TUI parser is unchanged.
Backend error details are intentionally preserved rather than translated.
The localization regressions also switch languages during browser authorization
and after successful or failed account refresh. A Qt URL handler captures valid
browser launches without opening a browser; malformed, non-HTTPS, hostless, and
credential-bearing URLs must never reach that handler. Worker completion is
gated so error/status checks do not depend on thread timing. Startup regressions
retain the distinction between `auto` and an explicit override even when both
resolve to the same locale, and reject positional arguments alongside help.

After changing GUI strings, extract them with Qt tooling, translate every new
entry in `src/ui/gui/translations/onedrive_zh_CN.ts`, and rebuild:

```bash
cmake --preset debug -DONEDRIVE_BUILD_GUI=ON -DONEDRIVE_ENABLE_GRAPH_E2E=OFF
cmake --build build/debug --target onedrive_gui_lupdate
cmake --build build/debug --target onedrive-cpp-gui gui_settings_tests gui_startup_state_tests gui_localization_tests gui_startup_options_tests gui_login_tests gui_synchronization_tests gui_monitoring_tests gui_state_view_model_tests gui_system_appearance_tests browser_request_tests
ctest --test-dir build/debug -R '^(gui_.*|browser_request_tests)$' --output-on-failure
```

`lrelease -nounfinished` compiles the catalog into `.qm`; Qt resources embed
both application and Qt catalogs in the GUI and its tests. Do not hand-edit
generated `.qm` files. Offscreen tests run in separate English, `zh_CN`,
`zh_SG`, French, and Traditional Chinese locale environments. They verify
resource loading, English fallback, translator lifetime, actual window
labels, account/configuration enums, login results, settings dialogs and Qt
buttons, placeholder substitutions, and UTF-8 loopback replies. Startup tests
exercise the real Qt parser and translators in English/Chinese environments,
including overrides and both `--language value` and `--language=value`.
Offscreen executable subprocesses verify help/version and invalid arguments
exit without entering the GUI event loop. Menu-switch tests verify checked
actions, existing widgets/status text, unsaved fields, and active login
preservation through English/Chinese/system-language transitions. No Graph
credentials or real browser are needed. These tests do not replace a real
desktop/native-dialog check.

Settings phases one and two are implemented: account/directories, sync rules,
transfers, network, monitor, and advanced/logging, including advanced account
endpoints, scope arrays and mount requirements. GUI monitoring uses the shared
worker/service and IPC; directory migration and systemd unit management remain deferred.
`SettingsEditor` keeps one widget-backed draft across all six tabs. Enum
`itemData` contains parser values, never translated labels. Numeric text fields
avoid Qt's 32-bit spin-box truncation and validate nonnegative TOML int64 values.
Only edited keys are patched; optional selective rules can be removed, and
editing sync mode removes legacy `sync.upload` while preserving the chosen
deletion policy. The core patcher uses parser source regions (including quoted
headers, dotted keys and inline tables), not regex or full-document serialization.
Typed arrays and nested Graph throttle tables retain their source structure.
Proxy URL/bypass override checkboxes preserve absent versus explicitly empty
values independently, including existing empty tables. Removing all proxy
settings can remove its table without losing surrounding comments.
GUI save/import reject inline URL credentials and unsafe auth/Graph endpoints;
password/CA files are references only, with runtime file-security checks intact.
Save validates before backup/atomic replacement. Import/reload intentionally
reset all draft fields; language switches and login completion do not.

`gui_settings_tests` exercises real offscreen controls and saved TOML for every
exposed setting, 64-bit boundaries, parser constraints, comments/unrelated fields,
backups, optional-key removal, draft/language/tab switching, reload, and first-time
Application ID setup. It checks all 57 concise tips and accessibility descriptions
on fields, labels, browse buttons and override rows, initial Chinese/English and
unsupported-locale fallback, reload into disabled inherited controls, and runtime
language switches with expanded advanced groups and inactive proxy drafts.
Run these cases with `ctest --preset debug -R '^gui_settings_tests$'`.
It also checks every transfer-order/sync-mode reload,
numeric lower/upper limits and strict draft syntax, advanced groups and browse
accept/cancel, inline optional-key removal at each position, UTF-8/multiline
source edits, rejected patches without writes/backups, cancelled save/import,
invalid imports, and first import with destination-relative rules. Unknown TOML
keys are rejected without modifying the source, not silently discarded.
Startup/import regressions cover corrupt-config recovery;
localization/login tests cover disabled controls and worker/draft preservation.

`gui_synchronization_tests` exercises the real application synchronization service
with offline Graph/storage adapters: preview preserves synchronized files, apply
downloads the expected bytes, and persistent dry-run is honored. Controller
regressions check worker/UI thread boundaries, runtime locks, cancellation,
restart/destruction, logger restoration, a 500-event queue and progress
coalescing. Window regressions cover translated progress/history, blocked and
failed results, settings/account interlocks, unsaved drafts, bounded history
with explicit omissions, and cancel-and-join on close. Graph/application tests
also cancel token refresh and drive identity lookup before storage creation.
Tab/log regressions check the Overview/Synchronization/Logs/Settings split,
navigation and overview summaries, cross-task history and exact overflow counts,
timestamps, every level filter, copying displayed text, clear-during-sync,
auto-scroll on/off, raw multiline details, and bilingual history/filter/draft
preservation. Account tests ensure browser authorization parameters do not enter
the session log.
Logging regressions retain all six shared backend severities, exercise every
configured GUI threshold including `off`, and verify diagnostic delivery before
failure completion, logger restoration and restart. TUI tests check low-severity
backend suppression, all application-message levels and invalid severity
rejection. A pseudo-terminal runs the real CLI application with the offline
authentication adapter to verify backend forwarding to the TUI while all enabled
levels remain in the private file log. Empty health inspection and login
completion during window closing use Info rather than a separate Success level.
Display-threshold regressions also cover live arrivals, empty results, copying,
language switching and retained lower-severity messages after clearing history.
Pixel checks verify blue progress at 0%, 1%, 50% and 100% in light/dark palettes,
including an unfilled track. CLI application tests exercise all logging
thresholds, including `off`, and verify that command results remain independent
of the backend diagnostic threshold.
`sync_preview_tests` covers private-cursor reuse in both merge and replacement
mode, repeated previews, last-change-wins merging, retained unapplied changes,
cursor expiry and directory hierarchy fallback, scope/filter/state/journal
invalidation, fresh local conflicts and remote-deletion safeguards, cancellation
and failure. GUI controller integration uses a real SQLite ItemDatabase to
verify preview does not persist state, incremental apply commits the latest
cursor and expected bytes, and saved-file/account/external-state changes or
explicit invalidation prevent reuse. Cache access is worker-owned during tasks
and idle-only during account transitions; saved-file hashes use exactly the
securely read bytes passed to the configuration parser.
Graph pagination and preview-cache merging share the private
`graph/delta_changes.hpp` accumulator: first-seen order is stable and each ID's
latest complete record replaces all earlier metadata, including deletion,
malware, validation and hash fields. Protocol tests also cover transport errors,
malformed delta pages and cancellation before requests/between pages.
Cache regressions additionally cover tracked-directory fallback, synthetic
full-snapshot removals, equivalent reordered state collections, changed persisted
hashes, empty cursors, late cancellation, failed full-query fallback and
selectively retained local files/upload suppressions, including local-path
inspection errors that must invalidate the cache without changing user data or
applied state. Application tests seed an
actual cache before early cancellation/identity failures and startup-observer
exceptions; GUI tests exercise the default production function using whitespace
credentials that fail locally before HTTP, plus the worker-ownership guard.
Unsupported formats are configured only in the private, compiled
`src/graph/unsupported_formats.hpp` rule table. Each rule has a stable, nonempty
unique ID and display name, MIME/extension sets (both must match), and independent
package types. Keep IDs stable: `RemoteItem.unsupported_format` and pending Delta
rows retain them, and durable blocked records use `unsupported_<id>` reason codes.
Shared metadata and package descendants use the same generic classification.
The complete rule table participates in pending-cache and synchronization-filter
fingerprints, so adding rules refreshes metadata without changing the apply
pipeline or adding per-format database migrations.

`sync_unsupported_tests` exercises real SQLite restart/retry, rule-policy refresh,
normal-file progress counts and strict hash failure, package descendants,
bidirectional upload exclusion, and preservation of existing notes and snapshots
across remote deletions/full refreshes. Graph helper/protocol tests cover all
configured rules, late-page parents, shared items, ordinary `.one` files and
metadata errors. Single-download and bilingual GUI regressions cover explicit
unsupported results, counts, translated history and severity filtering.

Progress regressions explicitly distinguish unknown scan/download totals,
completed scans and delta summaries, file-count versus byte percentages, and
download completion versus task completion. `EndDownloadProgressEvent` is a
terminal-line formatting marker and must not reset GUI download progress.
Tests also preserve cancellation through late scan summaries and check that
completed/stopped downloads do not retain a remaining-time estimate.
Transfer counts use locale-aware grouping; byte quantities and rates use
IEC units, and remaining time uses compact seconds/minutes/hours/days.
GUI transfers, CLI progress and quota/inspection fields share the Qt-independent
`util/byte_size.hpp` scaling helper while retaining their own numeric precision,
locale and existing maximum unit.
`sync_persistent_delta_tests` recreates real SQLite stores and synchronization
engines to verify interruption/restart in full and merge modes, latest-ID
updates/deletions, empty incrementals, completed-file reuse and byte-offset
resume. The shared engine checkpoints unfiltered/unaugmented remote metadata
before planning/execution, separately from the applied cursor. Preview may read
compatible pending metadata but never changes it. Scope/filter/applied-baseline
mismatches invalidate reuse; expired cursors and hierarchy changes fall back.
`db_pending_delta_tests` covers every raw metadata field and record order,
transactional replacement/commit cleanup, read-only access, reset/clear and
schema 26→27 and 27→28 migrations. Separate header/item tables avoid a whole-batch SQLite
value-size limit. Per-file commits and recovery may update tracked state while
a batch is pending; unlike session preview caching, durable reuse does not
require identical item-state vectors.
CLI application and GUI controller recreation regressions also use real SQLite
stores to verify that a fresh entry point polls the fetched cursor, executes
retained changes, and atomically commits and clears the pending batch.
Regressions cover unit/time boundaries, zero and uint64-sized quantities,
the selected language independently of the default locale, and live translation.
Phase-two regressions cover every new field, proxy patterns/optional states,
nested/inline/dotted tables and internal array comments, timeout units, backoff/throttle constraints,
endpoint safety and rejection without writes or backups.
Focused phase-two coverage also checks empty GUI overrides/scopes, independent
inherited proxy credentials, NUL/empty file-reference rejection, safe table
removal, empty inline throttle parents, and int64 timeout/throttle limits.
`proxy_basic_tests` uses loopback Digest challenges to verify authentication
for environment-inherited proxies and direct connections with empty overrides.
No GUI sync action, migration or online credential validation is implied.

## Live Graph end-to-end tests

The `e2e` preset is disabled from normal builds and requires a dedicated test
account or Drive whose remote root contains a stable fixture. The external
configuration must already be authenticated and must not be committed. The
runner copies its state directory into an isolated temporary workspace, clears
only that copy, and configures an isolated `sync_list`. It verifies that an
explicit single-file download bypasses an empty list, that the following sync
excludes its snapshot without deleting the local file, that adding an inclusion
rule triggers a full remote-state query and re-downloads the fixture, and that
the next incremental synchronization does not replace the unchanged file. It
then writes conflicting local content, clears only the isolated state copy, and
verifies that `sync.local_conflict = "backup"` emits its structured event,
preserves the exact local bytes in one same-directory `safeBackup`, restores
the authoritative Graph fixture, persists one snapshot, and leaves both files
unchanged during the following incremental synchronization. The runner forces
the isolated configuration to use the `backup` policy; it does not modify the
external configuration. It also creates a disposable local subtree and verifies
remote directory creation, simple upload, an upload-session file larger than
250 MB, completed-upload recovery, simultaneous local/remote conflict backup,
local moves into new parents, and local deletion. The large fixture defaults to
250,000,001 bytes; `ONEDRIVE_E2E_LARGE_UPLOAD_BYTES` may increase it up to 1 GB
but cannot lower it below the upload-session threshold. It then uses the copied
account token to create another uniquely named disposable Graph subtree, moves
and renames a file and a directory across remote parents, and verifies that
synchronization reuses the same local inodes, updates snapshots, emits
structured move events, and removes the fixture after remote cleanup. Temporary
remote subtrees are deleted even when the test fails. A separate disposable
subtree covers seven additional interoperability cases: bidirectional dry-run
without local, Graph, or snapshot mutation; upload-only and download-only
direction isolation; zero-byte files; names containing spaces and Unicode;
clean remote content replacement; and deletion of an individual remote file.
Finally, the runner injects an isolated trusted snapshot
whose ID is absent from a real full Graph Delta response and verifies safe
local deletion, SQLite cleanup, structured output, and preservation of the
live fixture. It also enables `sync_root_files`, verifies that the selection
fingerprint forces a full Graph query, and confirms the existing rule-selected
fixture is not rewritten.

### Configure and run

```bash
export ONEDRIVE_E2E_CONFIG=/absolute/path/to/dedicated-e2e.toml
export ONEDRIVE_E2E_EXPECTED_PATH=fixture/small.bin
export ONEDRIVE_E2E_EXPECTED_SHA256=<64-lowercase-hex-digits>
# Optional; must remain above 250,000,000 and no greater than 1,000,000,000.
export ONEDRIVE_E2E_LARGE_UPLOAD_BYTES=250000001

cmake --preset e2e
cmake --build --preset e2e
ctest --preset e2e -R graph_sync_e2e
```

The runner emits numbered stage and percentage progress. CTest keeps passing
test output quiet by default; add `--verbose` only when live progress needs to
remain visible.

The stable `graph_sync_e2e.py` entry point delegates to the Python modules in
`tests/e2e/`, which separate live Graph scenarios, system-boundary scenarios,
process orchestration, state fixtures, and runner self-tests. The live runner
keeps its baseline flow in `live.py` and delegates upload, remote-move, and
supplemental coverage scenarios to focused `live_*` modules. Configuration and
Graph helper self-tests are likewise separated from the main self-test driver.

### Test data and artifact safety

The dedicated Drive must grant file write access and should contain only
disposable test data even though the runner's generated `sync_list` materializes
only the expected and temporary fixtures. Allow enough local free space for
both the large source and its stable upload snapshot, and enough test-account
quota for the remote copy. Set
`ONEDRIVE_E2E_ARTIFACT_DIR` to retain command output and the client log after a
failure. These diagnostics may contain remote file metadata and must be handled
as sensitive data. Configuration, copied tokens, SQLite state, and downloaded
content are always removed. Never point the E2E runner at a daily-use state or
Drive.

### System-boundary scenarios

The live runner also exercises system boundaries with the real executable:
connections dropped by a local proxy and subsequent recovery, deterministic
local upload-storage exhaustion and recovery, unreadable upload sources, and
`SIGKILL` during a resumable upload. The Monitor boundary covers create,
modify, delete, file and directory rename, cross-directory move, atomic save,
burst coalescing, create-then-delete, recursive watches, a large upload-session
transfer, and recovery from a real inotify queue overflow before clean
`SIGTERM` shutdown. Run it as a non-root user; the permission scenario depends
on ordinary Unix access checks. CMake also requires the `stdbuf` command so
monitor JSON events are observable without changing
production buffering.

Each boundary is a separate CTest entry. Run all five with:

```bash
ctest --test-dir build/e2e --output-on-failure -L boundary
```

## Offline code coverage

`ONEDRIVE_ENABLE_COVERAGE=ON` enables Clang source-based instrumentation.
It requires `BUILD_TESTING=ON`. Normal builds remain uninstrumented.
Select the `coverage` configure, build, and test presets in VS Code CMake Tools.
The preset uses a separate `build/coverage` directory and disables live Graph
E2E tests. Qt is optional; enable `ONEDRIVE_BUILD_GUI=ON` in this configuration
to include GUI code and tests when Qt is available.

CTest writes process-specific profiles under `build/coverage/coverage/profiles`.
Start each fresh measurement with an empty profiles directory; never mix
profiles from different source revisions or build configurations.
After the test run, generate a report without rerunning tests:

```bash
python3 tools/coverage_report.py build/coverage
```

The reporter requires matching `llvm-cov` and `llvm-profdata` versions
(version 20 by default; override with `--llvm-cov` and `--llvm-profdata`).
It matches profiles to executable Build IDs using `readelf`, then writes
per-executable LLVM LCOV exports, JSON summaries with uncovered line numbers,
and a per-file text summary under the build's `coverage` directory.
This avoids mixing counters from different executables with identically named
functions such as `main`. Tests, dependencies, and
generated files are excluded from project totals. Application entry points and
GUI code not exercised by tests are included as uncovered when built.
LLVM tools perform no external debuginfod lookups. Stale profile Build IDs
are rejected rather than silently included. LCOV lines and branches are merged
by source location; these totals differ from LLVM's per-instantiation summaries.
A passing suite does not imply complete coverage: real desktop behavior,
live Graph integration, and system-call failure paths need separate validation.

The offline failure regression targets use executable-local linker `--wrap`
options, not production fault-injection hooks. Atomic-write tests cover retries,
the rename commit boundary, and cleanup failures. Metadata tests simulate xattr
probe round trips, unavailable/error fallback, and identity-write failures without
requiring host xattr support. HTTP tests use loopback-only responses to exercise
checkpoint exceptions, close failures, and rollback error precedence. Move tests
use the fake item store to introduce conflicts at the journal boundary and verify
safe recovery. Run this bounded group with:

```bash
cmake --build build/debug --target atomic_file_failure_tests filesystem_metadata_failure_tests http_download_failure_tests sync_move_recovery_tests
ctest --test-dir build/debug -R '^(atomic_file_failure|filesystem_metadata_failure|http_download_failure|sync_move_recovery)_tests$' --output-on-failure
```

These tests do not require Graph credentials, a browser, or resource exhaustion.

The authorization/wire-protocol regressions additionally check all 64 PKCE
transition pairs at compile time, move-only secret cleanup (including small
strings and exception unwinding), and deterministic browser/Socket.IO reducer
events. `auth_code_failure_tests` uses executable-local URL-parser/randomness
wrappers to verify exception propagation, terminal failure, and partial-random
output cleanup. The GUI integration tests use only local loopback connections.
`monitor_socket_transport_tests` wraps libcurl and poll only in its executable,
verifying that the production adapter executes reducer commands and retains
fragmented text across `CURLE_AGAIN`; no network or real browser is needed.

```bash
cmake --build build/debug --target auth_code_tests auth_code_typestate_tests auth_code_failure_tests browser_request_tests gui_login_tests monitor_socket_tests monitor_socket_transport_tests monitor_notify_tests
ctest --test-dir build/debug -R '^(auth_code_tests|auth_code_typestate_tests|auth_code_failure_tests|browser_request_tests|gui_login_tests|monitor_socket_tests|monitor_socket_transport_tests|monitor_notify_tests)$' --output-on-failure
```

Blocked-item regressions in `app_queries_tests` exercise read-only SQLite queries,
configured/canonical Drive selection, malformed or ambiguous metadata, and unsafe
state paths. `graph_support_tests`, `sync_unsupported_tests`, and
`blocked_policy_tests` cover ancestor validation/cancellation, same-Drive context
filtering, and exact-file versus directory-subtree recovery guards.
`gui_synchronization_tests` checks strategy descriptions in both languages;
`download_single_tests` checks invalid remote metadata and persisted local-path
conflicts without transferring content or changing files outside the sync root.
These scenarios require no online Graph access.

After rebuilding coverage binaries, archive the exact old `coverage/profiles`
directory and create an empty replacement before running CTest. Generate a
named report with `python3 tools/coverage_report.py build/coverage --name
state-machines`. Inspect the private reducer headers as well as adapter source
files. LLVM template dispatch/accessor `no-profile-record` warnings (including
known hash-zero Proxy accessors) are separate from uncovered source branches;
report them rather than claiming complete per-instantiation coverage.
Select the `debug` or `release` presets to return to normal development;
their separate build directories are not affected by the coverage preset.

## C++ Core Guidelines checks

The `lint` preset runs the faster Clang-Tidy policy during compilation.
Bug-prone, performance, portability, and selected C++ Core Guidelines
diagnostics are treated as build errors:

```bash
cmake --preset lint
cmake --build --preset lint
```

The path-sensitive Clang Static Analyzer is intentionally excluded from the
fast preset because it can be expensive for template-heavy translation units.
Use the deep preset for periodic or CI analysis:

```bash
cmake --preset lint-deep
cmake --build --preset lint-deep
```

For focused rechecks, keep the same GUI setting and deep analyzer policy, but
build only the affected object targets, for example:

```bash
cmake --preset lint-deep -DONEDRIVE_BUILD_GUI=ON
timeout 15m cmake --build --preset lint-deep --target \
  CMakeFiles/onedrive_core.dir/src/graph/discovery.cpp.o \
  CMakeFiles/onedrive_core.dir/src/monitor/socket.cpp.o
```

An object recheck does not certify unbuilt translation units or a timed-out
scan. Report third-party diagnostics separately; do not edit system headers
or disable checks to conceal them.

The policy excludes only reviewed noise from required C/POSIX APIs, protocol
constants, and checked buffer boundaries. Microsoft GSL is used to express
non-null borrowed dependencies at API and RAII boundaries. ngcpp/proxy
provides type-erased runtime ports with explicit owning and borrowed adapters.

Prefer shared, locale-independent ASCII comparison for protocol tokens and
log levels instead of allocating and lowercasing copies. Use `std::string_view`
for synchronous borrowed text and `std::span` subviews for remaining I/O buffers.
Keep POSIX calls behind RAII owners; newer syntax must not weaken checked
close, durability, or error-reporting behavior.

Pagination and download-attempt helpers own their callables and invoke them as
lvalues across pages/retries; pass `std::ref` when borrowing mutable callable
state. Socket.IO workers contain exceptions from both transport work and
failure reporting. Logging and disconnection publication are attempted
independently, even when a logging sink/error handler throws; under allocation
failure, publication is best-effort and must not terminate the process.

Test fixtures share checked binary file I/O and exception matching through
`tests/support/common.hpp`. `throws_with<Exception>(operation, message)` matches
a message substring and returns false when no exception is thrown. Exceptions
outside the requested type propagate; specify `std::runtime_error` when a test
must not accept logic errors. Callables are borrowed, including move-only ones.
The `test_support_tests` target verifies these helper contracts independently.

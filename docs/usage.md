# Usage and command output

English | [简体中文](usage.zh-CN.md) |
[Documentation index](README.md)

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success, including help/version output and graceful Monitor shutdown |
| 1 | Runtime, configuration, authentication, storage, or synchronization failure |
| 2 | Invalid command-line arguments |
| 3 | Synchronization incomplete because items were blocked |
| 130 | Operation cancelled |

Blocked results previously used 2; automation must now check for 3. Monitor
reports each round's result in `last_result`; this is separate from the
Monitor process's exit code.

## Output, logging, and automation

Running without arguments or using `--help` (including subcommand help) shows
the application name and build version above the usage details. `--version`
prints only the application name and version.

Uploads use the same shared transfer metrics and output backends as downloads.
Console output adds `UL` / `Upload done` progress; the TUI retains separate
upload/download panels and the current scan/transfer/apply/wait phase. In Monitor
mode, a finished round returns to waiting without marking the monitor stopped;
an incomplete round remains indicated while waiting.
The live synchronization layout prioritizes status, active transfers, warnings,
and exit controls. Short terminals use single-line transfer summaries and hide
details that cannot fit instead of leaving empty frames. File history and recent
activity use the remaining rows without inner borders. Monitor waiting hides
transfer panels, and completed monitor transfers remain in file history rather
than occupying a progress panel. Inspection result paging is unchanged.

JSON retains the existing `download_progress` and `execution_summary` formats.
Additional `upload_progress` records contain `completed_files`, `file_count`,
`uploaded_bytes`, `total_bytes`, `percentage`, `completed`, `bytes_per_second`,
`estimated_seconds_remaining`, and `elapsed_milliseconds`. Unknown upload totals
produce `percentage: null`, not a fabricated 0%/100%. `sync_context` reports the
configured `direction` and `preview` flag; `sync_phase` reports `preparing`,
`scanning`, `scanning_local`, `downloading`, `uploading`, `applying`, or `waiting`.
Consumers should ignore unneeded additional event types. `--quiet` suppresses
these progress/context/phase records.

Transfer counters may include saved progress, but speed counts only payload bytes
transferred during the current operation, including retransmitted upload bodies,
not historical resume offsets or reused local downloads. Upload file completion
requires successful remote validation and local state commit; transfer 100% is
not the final synchronization result. Check the command exit status and final
summary. Stop/failure retains resumable journals rather than reporting completion.

Runtime diagnostics are written to standard error with `info` severity by
default. systemd captures that stream automatically:

```bash
journalctl --user -u onedrive-cpp.service -f
```

Every subcommand accepts diagnostic logging and user-output options:

```bash
onedrive-cpp transfer sync --dry-run --log-level debug
onedrive-cpp transfer watch --log-file ~/.local/state/onedrive-cpp/onedrive-cpp.log
onedrive-cpp transfer sync --dry-run --color always
onedrive-cpp transfer sync --ui tui
onedrive-cpp transfer sync --theme ocean
onedrive-cpp account login --ui tui
onedrive-cpp inspect health --ui tui
onedrive-cpp inspect status --ui tui
onedrive-cpp inspect drives --ui tui
onedrive-cpp inspect shared --ui tui
onedrive-cpp inspect sites Engineering --ui tui
onedrive-cpp inspect quota --ui tui
onedrive-cpp transfer download Documents/report.pdf --ui tui
onedrive-cpp transfer sync --dry-run --output json
onedrive-cpp transfer sync --dry-run --quiet
onedrive-cpp inspect drives --output json
onedrive-cpp inspect shared
onedrive-cpp inspect sites Engineering
onedrive-cpp inspect quota
onedrive-cpp inspect status
onedrive-cpp inspect blocked
onedrive-cpp inspect blocked --reason unsupported_onenote --output json
onedrive-cpp inspect blocked --search Documents --ui tui
onedrive-cpp inspect storage
onedrive-cpp inspect partials
onedrive-cpp inspect files Documents --status modified
onedrive-cpp inspect verify Documents --mode content
onedrive-cpp inspect config
onedrive-cpp state cleanup --dry-run
onedrive-cpp state cleanup --yes
onedrive-cpp state migrate --dry-run
onedrive-cpp state migrate --yes
```

Supported levels are `trace`, `debug`, `info`, `warn`, `error`, `critical`,
and `off`. This is a minimum severity for backend diagnostics: `--log-level warn`
retains warnings, errors and critical messages. It does not filter command
results; use `--quiet` to suppress lower-severity result messages. The TUI also
omits trace/debug/info backend details from its activity list, even when enabled
in file logging. A configured log file rotates at 5 MiB and retains three older
files. The parent directory must already exist, be owned by the current user,
contain no symbolic-link path components, and not be writable by other users.
The active log and every rotated file must be regular, single-link files owned
by the current user; the client enforces mode `0600` and refuses symbolic or
hard links. Authentication tokens, device codes, authorization headers, proxy
passwords, and preauthorized transfer URLs are never written to logs. Logs can
still contain account and Drive display names, remote item names, and local
paths, so retain the enforced private permissions when copying diagnostics.

With debug logging enabled, download integrity diagnostics include the remote item
ID, eTag, expected and downloaded sizes, initial resume and durable checkpoint
offsets, hash source, and expected and calculated hashes. If a streamed hash
fails verification, the client recomputes SHA-256 and QuickXorHash from disk
before discarding the temporary file, and logs whether they match the streamed
hashes. A disk recheck failure is logged without replacing the original integrity
error. These diagnostics do not disable validation or accept a failed download.
All additional diagnostics use debug severity; the original integrity failure
remains a warning. Enable `debug` in the logging configuration before reproducing
the issue. Successful downloads also record their integrity context at debug level.

`--color=auto` is the default: styling is enabled only for a terminal and is
disabled when `NO_COLOR` is set. `always` forces ANSI styling and `never`
disables it. `--output=json` emits one compact JSON object per line and never
emits ANSI sequences; destructive interactive confirmation requires `--yes`
in this mode. Application messages use the same six severity levels as backend
logs; successful results use `info` instead of a separate `success` level.
JSON retains each original level (`trace`, `debug`, `info`, `warning`, `error`,
`critical`). `--quiet` suppresses trace, debug and informational output while
retaining warnings, errors and critical messages. Diagnostic logs remain on standard error, while
command results are written to standard output.

`account login`, all `inspect` commands, and all `transfer` commands use `console.ui` from the configuration,
which defaults to `auto`; an explicit `--ui` overrides it for one invocation.
Auto mode opens the FTXUI status dashboard only when standard input and output are terminals,
`TERM` supports terminal controls, and the terminal is at least 60 columns by
12 rows. Redirected output, pipes, JSON, quiet mode, `TERM=dumb`, and
undersized terminals automatically use the normal console. Use
`--ui=console` to disable the dashboard. `--ui=tui` requires the dashboard and
reports a clear error instead of falling back when the terminal cannot support
it. In the watch dashboard, press `q`, `Q`, or `Esc` to stop cleanly.

Inspection dashboards keep their final results visible with the prompt
`Press Enter to exit (or q)`. Press Enter, `q`, or `Q` to exit immediately;
`q` and `Q` do not require Enter. When an inspection command produces multiple
sections, press Left/Right or `p`/`n` to switch sections before exiting. Other
keys do not dismiss the results.
This does not change controls for running synchronization, transfers, or login.
The prompt stays visible even when a small terminal
clips the details; enlarge the terminal or use console output to see all fields.
Console and JSON output still exit immediately; use `--ui console` to retain
inspection results in the terminal scrollback without an interactive pause.

The dashboard enters the terminal's alternate full-screen buffer immediately,
shows the client version, and restores the original screen on exit. The
download dashboard reuses the persistent progress, transfer-rate, and ETA
display used during synchronization. After an interrupted sync, its file and
byte progress includes downloads already completed and verified, while speed
and ETA measure only new transfer activity. Its user-facing status uses cloud
and file terminology rather than API names.
`console.theme` selects `hacker` (the green default), `ocean`, `amber`, or
`synthwave`; `--theme` overrides it for one run. `--color=never` and
`NO_COLOR` keep the selected layout but suppress theme colors.
`console.language` selects `auto` (system locale), `en`, or `zh_CN` for CLI and
TUI display strings and help; `--language` or `ONEDRIVE_CPP_LANG` overrides it
for one run (`--output json` keys, configuration keys, and diagnostic log files
remain in English).
Command help shows named choices, without internal enum numbers:

```text
--language {auto,en,zh_CN} [auto]
--auth-flow {auto,auth-code,device-code} [auto]
--ui {auto,console,tui} [auto]
--theme {hacker,ocean,amber,synthwave} [hacker]
```

## Read-only account and synchronization information

`inspect drives` lists OneDrive drives available to the active Microsoft account and
marks the configured drive. Its `reference` field shows the configured reference
(such as `me`) separately from the real Drive ID; other drives are not labelled
`me`. Each drive includes total, used, remaining, deleted, and quota-state values,
reusing `inspect quota` formatting. Missing quota values are `unavailable`.

In the TUI, results open on the first drive, one drive per page, with a counter
such as `Drive 1/3`. Press Left/Right or `p`/`n` to switch pages without Enter;
navigation stops at the first and last pages rather than wrapping. Press Enter,
`q`, or `Q` to exit. Console and JSON output still include all drives without
pagination or waiting for input.

File statistics are explicitly labelled `local state (not cloud totals)`:
`known_files` counts distinct file IDs recorded in item snapshots, pending or
partial downloads, and non-deleted blocked items. Directories are excluded.
`downloaded_files` counts tracked regular files still present inside the local
Drive root, including modified or previously uploaded files; this is local
availability, not a historical download counter or content verification.
`pending_files` deduplicates pending and partial download records, while
`blocked_files` counts non-deleted blocked files. Counts can overlap (for example,
a local file may have an update pending). No cloud inventory scan is performed.
Each drive uses its own read-only state database; absent state is `unavailable`,
not zero. Invalid databases and filesystem inspection errors are reported.

`inspect shared` lists items returned by `sharedWithMe` and
shortcuts added to the configured OneDrive, including the target Drive and
item IDs needed for configuration. `inspect sites [QUERY]` lists or searches
accessible SharePoint sites and each site's document-library Drives: omit
`QUERY` (or pass `*`) to list accessible sites, pass a keyword, or pass a
SharePoint site URL (`https://contoso.sharepoint.com/sites/eng`),
`hostname:/sites/path`, `root`, or three-part site ID to resolve a single site
directly (compatible with `Sites.Selected`). When a keyword query matches no
sites, the command falls back to listing accessible sites with `*` when
permitted. Tenant-wide search requires `Sites.Read.All` or
`Sites.ReadWrite.All`; direct site resolution also works with `Sites.Selected`.
Change scopes and run `onedrive-cpp account login` again before using it.

`inspect quota` reports total, used, remaining, deleted, and quota-state values for the
configured drive. `inspect status` combines the active account and canonical Drive
identity with local read-only state: sync mode, delete policy, last recorded
synchronization result, tracked and blocked item counts, pending journals,
Delta cursor, selective-sync fingerprint, and WebSocket configuration.

`inspect blocked` reads the same saved blockers as the GUI for the active account
and configured Drive, entirely offline. It does not require an application ID
or refresh token, create state directories/databases, acquire the synchronization
lock, or start a transfer. Each record includes its path, reason code/message,
Drive/item IDs, recorded discovery count, strategy control, and whether recovery
is allowed by the shared strategy. Discovery counts are not download retry counts.
This command cannot change a strategy or unblock an item.
SQLite may create its normal WAL coordination files while reading an existing
database; the saved records and cursors remain unchanged.
Use `--reason CODE` for an exact reason-code filter (including unknown/future
codes), and `--search TEXT` to search paths, reason codes, and messages together.
Search ignores ASCII case and matches other UTF-8 bytes verbatim; both filters
can be combined. The summary shows the total recorded and filtered counts.
Missing account/Drive/database state produces an empty result; malformed,
ambiguous, or unsafe state and database failures return an error, not an empty
success. JSON emits one `blocked_item` section per matching record followed by
`blocked_item_summary`, using the standard string-valued `values` object.
An empty selection also emits `no_blocked_items`. Finding blockers is a successful
query (exit 0), not a synchronization failure. The TUI retains each record as a
page; use Left/Right or `n`/`p` to browse and Enter/`q` to exit. Use console or
JSON output if a small terminal clips details.

`inspect storage` reports the resolved Drive data root, filesystem capacity and
available space, tracked file bytes, partial-download bytes, and the state
database path. `inspect partials` lists each saved partial download and
classifies it as resumable, missing, type-changed, outside the sync root,
path-mismatched, or an invalid checkpoint.
Its TUI retains the complete list below the recorded/resumable/invalid counts
and the combined local temporary-file sizes. Each row shows the remote path,
status, and saved download progress / total file size.
Up/Down or `k`/`j` selects a file and scrolls it into view;
selection stops at the first and last files. At 18 terminal rows or more, a
details area also shows the selected file's remote, destination, and temporary
paths and local temporary file size. Smaller windows prioritize the list and controls; enlarge
the terminal or use console output for long paths that are clipped.
Enter, `q`, or `Q` exits. Console and JSON output still include every record.

The size labels distinguish what the app remembers from what is on this computer:

- `saved download progress`: how much downloading the app last saved as complete.
- `total file size`: the full size saved for the cloud file, not the amount left.
- `local temporary file size`: the size of the unfinished download file on this
  computer now, not a cloud file or the final destination file.

Progress is saved from time to time, so it may differ from the temporary file's
current size. `unavailable` means its size could not be confirmed, not zero.
The JSON field names and values are unchanged.

`inspect files [PATH]` compares
tracked regular files with saved size and modification metadata. Use
`--status ok|missing|modified|type-changed|outside-root` to filter its output.

These commands do not synchronize files or modify remote content. Commands
that inspect local state open an existing SQLite database read-only and do not
create, migrate, repair, or quarantine a missing or older database. All
inspection commands support `--output json`.

`inspect health` recursively checks every `items.sqlite3` under the configured
state directory, including legacy databases outside the account layout. Each
result includes `schema_version` and `latest_schema_version` (Console/TUI
labels: `version` and `latest`). An unreadable version is `unknown`.
An intact older database reports `upgrade-required`, not `unhealthy`;
unsupported versions and integrity/schema failures remain unhealthy. The
command returns 1 if any database needs an upgrade or fails a check.

`inspect verify [PATH]` performs the same metadata checks as `inspect files`.
With `--mode content`, it additionally computes the saved QuickXorHash, SHA-1,
or legacy SHA-256 and reports `verified`, `hash-mismatch`, or `hash-unavailable`.
Content hashes are retained with item state after validated downloads and
remote metadata updates; older tracked files may remain unavailable until a
later synchronization records a hash.

`inspect config` prints effective non-secret paths, synchronization policy,
transfer limits, output settings, and whether a proxy is configured. It never
prints application credentials, authentication tokens, proxy credentials, or
proxy URLs.

`state cleanup --dry-run` previews cleanup within the current account's configured
Drive only; it does not delete whole Drive directories or completed files.
Configured `sync.dry_run = true` also prevents cleanup, even with `--yes`.
Without dry-run, `--yes` confirms deletion; otherwise text output asks for the
configured Drive reference. JSON requires `--yes`.

Valid resumable downloads are retained, including files larger than their saved
progress. Missing temporary files need only a record removal. Truncated files
and invalid saved sizes/progress are removed with their records when the paths
are safe. Orphan candidates must match `.<filename>.onedrive-partial-<PID>-<sequence>`
with positive decimal numbers (no leading zeros) and have no references from
saved items or pending download/upload/delete/move operations.

Out-of-root paths, symlinks, non-regular files, multiply linked or foreign-owned
files, ambiguous references, and unrecognized recorded names are retained
**together with their records** and reported as skipped. An incomplete directory
scan aborts before deletion. Files are checked again against the preview before
removal; deletion is flushed before its record is removed. A deletion failure
keeps the record for retry. Rerunning after an interruption safely handles records
whose files have already been removed.

The plan reports retained entries, record-only removals, file counts, total file
bytes, and skips. The result reports actual removed records/files/bytes, skips,
and failures; any skip or failure returns exit status 1, even if other candidates
were cleaned. Byte counts are logical file sizes, not guaranteed recovered disk
blocks. An orphan-only cleanup does not create a missing database.
Cleanup holds both the state-directory lock and the resolved synchronization-root
lease. Stop non-participating tools that might write to the same data directory.

## Concurrent tasks and busy resources

CLI, GUI, and the systemd user service share the state-directory runtime lock.
Monitor holds it for its entire lifetime, including waiting between rounds.
Stopping does not release locks until workers and cleanup have finished.

Sync/preview, Monitor, single-file download, missing-file recovery, and partial-file cleanup also claim
the resolved account/Drive synchronization directory. Different state directories
cannot bypass this protection: identical and parent/child roots are rejected,
while independent sibling Drive roots may run concurrently. Path normalization
and directory device/inode identities detect aliases; symbolic links remain
unsupported. Preflight validates the configured path without creating or changing
sync directories; synchronization prepares the resolved root only after claiming it.

The private registry is `/tmp/onedrive-cpp-locks-<effective-UID>` (directory mode
`0700`, files `0600`). All participating processes must use the same local `/tmp`
namespace; do not isolate this service with `PrivateTmp=true`. This protection is
per Linux user and local machine, not a cross-user or cross-host filesystem lock.
Other tools and older clients that do not participate must still be stopped.

A rejected request fails rather than being forwarded or queued. `runtime_busy`
diagnostics identify the resource, requested directory, owner PID, operation,
start time, and state/sync directories; JSON emits a section with string-valued
`values` plus an error message. Metadata contains no authentication credentials.
OS locks, not PID existence, decide whether a resource is busy. A crash releases
the locks automatically; stale directory leases are reclaimed at the next claim.
Do not delete lock files to unlock a live process; stop its owner instead.

## Explicit local Monitor control

Start monitoring with `transfer watch`, then use the same configuration/state
directory for explicit control:

```bash
onedrive-cpp monitor status --config ~/.config/onedrive-cpp/config.toml
onedrive-cpp monitor trigger --config ~/.config/onedrive-cpp/config.toml
onedrive-cpp monitor stop --config ~/.config/onedrive-cpp/config.toml
```

These commands need no authentication, do not contact Graph, and do not open
databases or claim the Monitor's runtime locks. They control only the already
running task; requests cannot supply another root or change its configuration.
`status` reports lifecycle state (`starting`, `synchronizing`, `waiting`, or
`stopping`), PID, per-instance `instance_id`, actual sync directory, a pending manual request, completed-round
count, and the last round's exit code (`none` before the first completion). It is
a snapshot, not a detailed transfer-progress feed. `--output json` emits the
existing `monitor_control` section with string-valued `values`.

`trigger` starts a check promptly while waiting. Requests during a round merge
into one subsequent check, shared with local/remote changes; rounds never run
concurrently. `stop` cancels the active round and rejects further triggers.
`accepted`/`merged` acknowledge the request, **not successful synchronization or
finished shutdown**. Locks remain held until the owner finishes cleanup.
Ordinary `transfer sync` and a second `transfer watch` still fail on busy resources;
they are never implicitly forwarded. A busy Monitor diagnostic suggests these
explicit controls.

The endpoint is `monitor-control.sock` in the private state directory (`0700`),
with socket permissions `0600` and same-user peer checks. Protocol v2 accepts
only bounded JSON command packets; socket connection/read/write waits are bounded
to two seconds each. Missing, unsafe, unresponsive, or stale endpoints return an
error, never an idle-looking success. A new Monitor reclaims an owned inactive
socket after obtaining its normal runtime lock; do not delete a live endpoint.
Long state-directory paths exceeding the Unix socket limit fail explicitly.
Restart older v1 Monitors after updating; incompatible protocols fail explicitly.

### File-operation history

The GUI **Files** tab lists completed operations since this application started:
local completion time, download/upload/local or remote move/deletion, path,
previous path for moves, and Drive ID. Hover over the path for the local target.
Search matches paths, actions and drives. History survives subsequent syncs,
Monitor restarts, clearing logs and language changes; application exit clears it.
Only operations performed by this process are included, not external Monitor
activity, previews, already-current reuse, blocked attempts or unfinished work.
Completed work remains listed even if another operation later fails or cancels.
Recovery completions and folder moves/deletions are labelled; a folder operation
is one entry, not an expansion of every descendant.

CLI text and JSON output stream completed-operation rows as they occur (quiet
mode suppresses them). JSON uses `event: "file_operation"`, UTC `completed_at`,
`completed_at_unix_milliseconds`, `action`, `path`, `previous_path`, `drive_id`,
`local_path`, `directory`, `recovered`, `history_retained` and `history_omitted`.
The TUI shows recent operations live and prints the complete retained list after
leaving its full-screen view at completion or graceful Monitor shutdown.
Text and TUI rows start with paths, followed by actions and details, with UTC
timestamps last. Drive IDs are omitted from individual file rows and listed
once per Drive in the retained-history summary. This summary covers retained
operations, not necessarily every configured Drive. JSON retains each operation's
`drive_id` unchanged.
If a command fails after completing earlier operations, it still exports those
retained completions before reporting the error.
CLI timestamps use UTC. Both frontends retain at most
`history.maximum_entries` (default **10000**, positive integer), discard oldest
entries when full and display the omitted count. Already streamed terminal/JSON
output cannot be retracted. This limit is separate from the GUI's diagnostic log
limit and does not affect synchronization totals. The GUI setting is under
Settings / Advanced and logs; use Reload after external configuration edits.

### GUI monitoring and external ownership

**Start monitoring** runs the shared Monitor service with saved settings, including
`sync.dry_run`. The GUI-owned task uses the existing synchronization worker,
cancellation and upload/download events. Waiting retains the last round's totals
and completion time; a completed round or 100% transfer does not end monitoring.
Closing the window cancels only its own task and waits for cleanup.

GUI-owned monitoring shows both **Monitor session totals** and **Last round
totals**. Downloads, uploads, moves and deletions accumulate from reported round
summaries for the current Monitor task; an empty follow-up round does not erase
earlier work. These count operations, not distinct files. Blocked items remain a
latest-round count, never a sum across rounds. Starting another round preserves
the last reported summary; stopping or a failed/cancelled round without a summary
does not discard previously reported work. Clearing logs or changing language
does not reset statistics. Starting a new Monitor task resets session totals;
they are not persisted across application restarts. External Monitor snapshots
do not supply round or session totals, so the GUI leaves them unavailable.

The last-round result uses readable labels: succeeded, failed, incomplete
(blocked items), or cancelled. Blocked items do not prevent independent safe
operations from completing. CLI Monitor output retains numeric `last_result`
and adds `last_result_name` (`succeeded`, `failed`, `blocked`, `cancelled`,
`unknown`, or `none` when no round has finished).

Successful local removals emit an information-level `local_item_removed`
message containing the local path. The GUI's Logs tab retains events across
rounds, up to the latest 500 entries; select All levels or Information to see
these messages. Round summaries describe that round, not cumulative removals:
a following round can show zero removals after an earlier successful deletion.

The GUI automatically discovers an external CLI/service Monitor in the selected
state directory, refreshing snapshots asynchronously every second without
authentication. Polling reuses the loaded state directory rather than rereading
configuration every second; use Reload after editing the configuration externally.
**Sync now** requests that Monitor while waiting; it is disabled
during an active round or pending request. Stopping an external Monitor requires
confirmation. Controls pin both PID and instance identity, so a replacement task,
even in the same process, cannot inherit an earlier confirmation. Closing the
window leaves an external Monitor running.

External snapshots do not provide live transfer progress, direction or round
completion timestamps; the GUI does not invent them. Missing sockets do not prove
cleanup has finished: read-only inspection of the existing state lock keeps
actions disabled while resources remain occupied. Other IPC errors are visible,
and uncertain ownership blocks new tasks. Settings/account recovery remains
available when read-only lock inspection establishes that runtime resources are
free, even if IPC is invalid. Different state directories with overlapping roots
still fail as busy rather than attaching or taking over.
Stopping through IPC does not disable a systemd unit or change its restart policy;
systemd start/restart management and directory migration remain separate future work.

## Offline database upgrades

`state migrate --dry-run` lists existing databases and their current and target
versions without upgrading them or creating backups. To upgrade, stop any
sync/watch process using this state directory, then run `state migrate --yes`
with the same `--config` as your inspection. Without `--yes`, text output asks
you to type `migrate`; JSON requires `--yes`. A configured `sync.dry_run = true`
also prevents upgrades.

This command needs no authentication or network access and does not synchronize
files. It holds the same runtime lock as synchronization. It checks all
discovered databases before starting and refuses detected corruption or
unsupported versions; current databases are skipped. Older schemas undergo
full structural validation during migration, so a preview is not a guarantee
that migration will succeed.

Before each upgrade, SQLite creates a private, consistent backup beside the
database named `items.sqlite3.pre-migrate-v<VERSION>-<SUFFIX>`, including
committed WAL data. The command reports its path. Keep these backups until
you have verified the upgrade; they are not deleted automatically and may
contain sensitive synchronization metadata. Backups require additional disk
space. Each database's upgrade and final integrity/schema checks form one
transaction: a failed upgrade rolls back, retains the backup, and stops with
exit status 1. Databases already upgraded earlier in the run remain upgraded.
Corrupt databases are never automatically rebuilt or quarantined by this command.
Afterwards, run `inspect health` again.

## Shell completion

The package installs Bash and Zsh completion definitions for subcommands,
options, enumerated values, and file paths. New shell sessions load them
automatically when the shell completion system is enabled.

For a development build, enable Bash completion in the current shell with:

```bash
source packaging/completions/onedrive-cpp.bash
```

Then use Tab completion for commands and values:

```bash
build/release/onedrive-cpp <Tab>
build/release/onedrive-cpp transfer sync --<Tab>
build/release/onedrive-cpp transfer sync --log-level <Tab>
```

The installed definitions are placed in
`share/bash-completion/completions/onedrive-cpp` and
`share/zsh/vendor-completions/_onedrive-cpp`.

## Manual page

The CMake install rules place the section 1 manual at the standard
`share/man/man1` location and the Simplified Chinese translation under
`share/man/zh_CN/man1`. After installing the DEB package, `man` selects the
translation matching the current locale:

```bash
man onedrive-cpp
LANG=zh_CN.UTF-8 man onedrive-cpp
```

Debian's `man-db` trigger updates the index automatically. After a direct
`cmake --install` into a custom prefix, refresh the local index when needed:

```bash
sudo mandb
```

The generated page can also be inspected without installing it:

```bash
man --local-file build/release/generated/onedrive-cpp.1
```

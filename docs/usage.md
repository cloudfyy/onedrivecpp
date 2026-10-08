# Usage and command output

English | [简体中文](usage.zh-CN.md) |
[Documentation index](README.md)

## Output, logging, and automation

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
and `off`. A configured log file rotates at 5 MiB and retains three older
files. The parent directory must already exist, be owned by the current user,
contain no symbolic-link path components, and not be writable by other users.
The active log and every rotated file must be regular, single-link files owned
by the current user; the client enforces mode `0600` and refuses symbolic or
hard links. Authentication tokens, device codes, authorization headers, proxy
passwords, and preauthorized transfer URLs are never written to logs. Logs can
still contain account and Drive display names, remote item names, and local
paths, so retain the enforced private permissions when copying diagnostics.

`--color=auto` is the default: styling is enabled only for a terminal and is
disabled when `NO_COLOR` is set. `always` forces ANSI styling and `never`
disables it. `--output=json` emits one compact JSON object per line and never
emits ANSI sequences; destructive interactive confirmation requires `--yes`
in this mode. `--quiet` suppresses informational and success output while
retaining warnings and errors. Diagnostic logs remain on standard error, while
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
`q` and `Q` do not require Enter. Other keys do not dismiss the results.
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
Command help shows named choices, without internal enum numbers:

```text
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
item IDs needed for configuration. `inspect sites QUERY` searches accessible
SharePoint sites and lists each site's document-library Drives. Site discovery
requires `Sites.Read.All` or `Sites.ReadWrite.All`; change scopes and run
`onedrive-cpp account login` again before using it.

`inspect quota` reports total, used, remaining, deleted, and quota-state values for the
configured drive. `inspect status` combines the active account and canonical Drive
identity with local read-only state: sync mode, delete policy, last recorded
synchronization result, tracked and blocked item counts, pending journals,
Delta cursor, selective-sync fingerprint, and WebSocket configuration.

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
With `--mode content`, it additionally computes the saved Graph SHA-256 or
QuickXorHash and reports `verified`, `hash-mismatch`, or `hash-unavailable`.
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
The runtime lock covers processes sharing the same state directory; stop other
tools or configurations that might write to the same data directory before cleanup.

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

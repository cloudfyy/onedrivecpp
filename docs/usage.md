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

Inspection dashboards keep their final results visible until you press Enter.
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

## Read-only account and synchronization information

`inspect drives` lists OneDrive drives available to the active Microsoft account and
marks the configured drive. `inspect shared` lists items returned by `sharedWithMe` and
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
path-mismatched, or an invalid checkpoint. `inspect files [PATH]` compares
tracked regular files with saved size and modification metadata. Use
`--status ok|missing|modified|type-changed|outside-root` to filter its output.

These commands do not synchronize files or modify remote content. Commands
that inspect local state open an existing SQLite database read-only and do not
create, migrate, repair, or quarantine a missing or older database. All
inspection commands support `--output json`.

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

`state cleanup --dry-run` lists invalid partial-download records and orphaned
`.onedrive-partial-*` regular files without changing state. Run
`state cleanup --yes` to remove them. Files outside the resolved Drive root,
paths that do not match the destination's partial-file pattern, symbolic
links, and non-regular files are never deleted; invalid database records for
such paths can still be removed so they cannot be resumed.

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

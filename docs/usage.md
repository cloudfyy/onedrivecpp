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
onedrive-cpp sync --dry-run --log-level debug
onedrive-cpp monitor --log-file ~/.local/state/onedrive-cpp/onedrive-cpp.log
onedrive-cpp sync --dry-run --color always
onedrive-cpp sync --ui tui
onedrive-cpp sync --theme ocean
onedrive-cpp auth --ui tui
onedrive-cpp download Documents/report.pdf --ui tui
onedrive-cpp sync --dry-run --output json
onedrive-cpp sync --dry-run --quiet
onedrive-cpp drives --output json
onedrive-cpp shared
onedrive-cpp sites Engineering
onedrive-cpp quota
onedrive-cpp status
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

`auth`, `sync`, `download`, and `monitor` use `console.ui` from the configuration,
which defaults to `auto`; an explicit `--ui` overrides it for one invocation.
Auto mode opens the FTXUI status dashboard only when standard input and output are terminals,
`TERM` supports terminal controls, and the terminal is at least 60 columns by
12 rows. Redirected output, pipes, JSON, quiet mode, `TERM=dumb`, and
undersized terminals automatically use the normal console. Use
`--ui=console` to disable the dashboard. `--ui=tui` requires the dashboard and
reports a clear error instead of falling back when the terminal cannot support
it. In the monitor dashboard, press `q`, `Q`, or `Esc` to stop cleanly.

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

`drives` lists OneDrive drives available to the active Microsoft account and
marks the configured drive. `shared` lists items returned by `sharedWithMe` and
shortcuts added to the configured OneDrive, including the target Drive and
item IDs needed for configuration. `sites QUERY` searches accessible
SharePoint sites and lists each site's document-library Drives. Site discovery
requires `Sites.Read.All` or `Sites.ReadWrite.All`; change scopes and run
`onedrive-cpp auth` again before using it.

`quota` reports total, used, remaining, deleted, and quota-state values for the
configured drive. `status` combines the active account and canonical Drive
identity with local read-only state: sync mode, delete policy, last recorded
synchronization result, tracked and blocked item counts, pending journals,
Delta cursor, selective-sync fingerprint, and WebSocket configuration.

These commands do not synchronize files or modify remote content. `status`
opens an existing SQLite database read-only and does not create or migrate a
missing or older database. All five commands support `--output json`.

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
build/release/onedrive-cpp sync --<Tab>
build/release/onedrive-cpp sync --log-level <Tab>
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

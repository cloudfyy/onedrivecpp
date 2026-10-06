# Usage and command output

English | [简体中文](usage.zh-CN.md)

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
files. Authentication tokens, device codes, and authorization headers must
never be written to logs.

`--color=auto` is the default: styling is enabled only for a terminal and is
disabled when `NO_COLOR` is set. `always` forces ANSI styling and `never`
disables it. `--output=json` emits one compact JSON object per line and never
emits ANSI sequences; destructive interactive confirmation requires `--yes`
in this mode. `--quiet` suppresses informational and success output while
retaining warnings and errors. Diagnostic logs remain on standard error, while
command results are written to standard output.

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

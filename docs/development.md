# Development and testing

English | [简体中文](development.zh-CN.md)

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
remote subtrees are deleted even when the test fails. Finally, the runner
injects an isolated trusted snapshot
whose ID is absent from a real full Graph Delta response and verifies safe
local deletion, SQLite cleanup, structured output, and preservation of the
live fixture. It also enables `sync_root_files`, verifies that the selection
fingerprint forces a full Graph query, and confirms the existing rule-selected
fixture is not rewritten.

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

The stable `graph_sync_e2e.py` entry point delegates to the Python modules in
`tests/e2e/`, which separate live Graph scenarios, system-boundary scenarios,
process orchestration, state fixtures, and runner self-tests.

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

## C++ Core Guidelines checks

The lint preset runs Clang-Tidy during compilation with the project
`.clang-tidy` policy. Diagnostics from the Clang static analyzer, bug-prone,
performance, portability, and selected C++ Core Guidelines checks are treated
as build errors:

```bash
cmake --preset lint
cmake --build --preset lint
```

The policy excludes only reviewed noise from required C/POSIX APIs, protocol
constants, and checked buffer boundaries. Microsoft GSL is used to express
non-null borrowed dependencies at API and RAII boundaries. ngcpp/proxy
provides type-erased runtime ports with explicit owning and borrowed adapters.

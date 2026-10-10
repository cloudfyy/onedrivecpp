# Development and testing

English | [简体中文](development.zh-CN.md) |
[Documentation index](README.md)

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

# Documentation

English | [简体中文](README.zh-CN.md)

This guide groups the documentation by task. New users should read the
chapters in order; operators and contributors can jump directly to the
relevant reference chapter.

## Chapter 1: Getting started

1. [Project overview and installation](../README.md)
2. [Microsoft authentication](authentication.md)
3. [Configuration](configuration.md)
4. [Usage and command output](usage.md)

## Chapter 2: Synchronization operations

1. [Synchronization and recovery](synchronization.md)
2. [Configuration reference](configuration.md)
3. [Runtime diagnostics and read-only commands](usage.md)

The synchronization chapter explains Delta processing, conflict protection,
atomic transfers, move and deletion recovery, database repair, and operational
failure handling. The configuration chapter is the authoritative reference for
selection rules, synchronization modes, transfer limits, monitoring, proxy
settings, permissions, and storage layout.

## Chapter 3: Design and development

1. [Architecture](architecture.md)
2. [Development and testing](development.md)
3. [License, legal, and branding](project.md)

These chapters describe dependency boundaries, transaction state machines,
source-tree ownership, live Graph tests, system-boundary tests, and the
project's C++ Core Guidelines checks.

## Quick links

- Build and install: [project README](../README.md#build)
- First authorization: [authentication](authentication.md#authorize-the-client)
- Complete TOML example: [configuration](configuration.md#reference-configuration)
- Selective synchronization:
  [configuration](configuration.md#selection-and-filtering)
- Conflict and deletion policy:
  [configuration](configuration.md#conflict-handling)
- State reset and database repair:
  [synchronization](synchronization.md#state-reset-and-delta-rebuild)
- Troubleshooting output:
  [usage](usage.md)
- Contributor checks:
  [development](development.md#c-core-guidelines-checks)

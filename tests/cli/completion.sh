#!/usr/bin/env bash

set -euo pipefail

source "$1"

assert_contains()
{
    local expected="$1"
    shift
    local value
    for value in "$@"; do
        if [[ "$value" == "$expected" ]]; then
            return
        fi
    done
    printf 'completion result is missing %s: %s\n' \
        "$expected" "$*" >&2
    exit 1
}

COMP_WORDS=(onedrive-cpp "")
COMP_CWORD=1
_onedrive_cpp_completion
assert_contains sync "${COMPREPLY[@]}"
assert_contains download "${COMPREPLY[@]}"
assert_contains inspect "${COMPREPLY[@]}"
assert_contains state "${COMPREPLY[@]}"
assert_contains account "${COMPREPLY[@]}"
assert_contains --version "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp sync --)
COMP_CWORD=2
_onedrive_cpp_completion
assert_contains --dry-run "${COMPREPLY[@]}"
assert_contains --force-large-delete "${COMPREPLY[@]}"
assert_contains --output "${COMPREPLY[@]}"
assert_contains --quiet "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp account "")
COMP_CWORD=2
_onedrive_cpp_completion
assert_contains login "${COMPREPLY[@]}"
assert_contains logout "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp account login --)
COMP_CWORD=3
_onedrive_cpp_completion
assert_contains --ui "${COMPREPLY[@]}"
assert_contains --theme "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp account logout --)
COMP_CWORD=3
_onedrive_cpp_completion
assert_contains --config "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp inspect "")
COMP_CWORD=2
_onedrive_cpp_completion
assert_contains health "${COMPREPLY[@]}"
assert_contains status "${COMPREPLY[@]}"
assert_contains drives "${COMPREPLY[@]}"
assert_contains shared "${COMPREPLY[@]}"
assert_contains sites "${COMPREPLY[@]}"
assert_contains quota "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp inspect health --)
COMP_CWORD=3
_onedrive_cpp_completion
assert_contains --ui "${COMPREPLY[@]}"
assert_contains --theme "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp inspect status --)
COMP_CWORD=3
_onedrive_cpp_completion
assert_contains --ui "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp inspect sites --)
COMP_CWORD=3
_onedrive_cpp_completion
assert_contains --config "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp download --)
COMP_CWORD=2
_onedrive_cpp_completion
assert_contains --dry-run "${COMPREPLY[@]}"
assert_contains --config "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp state "")
COMP_CWORD=2
_onedrive_cpp_completion
assert_contains reset-cursor "${COMPREPLY[@]}"
assert_contains clear "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp state clear --)
COMP_CWORD=3
_onedrive_cpp_completion
assert_contains --yes "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp state reset-cursor --)
COMP_CWORD=3
_onedrive_cpp_completion
assert_contains --config "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp sync --log-level d)
COMP_CWORD=3
_onedrive_cpp_completion
assert_contains debug "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp sync --color a)
COMP_CWORD=3
_onedrive_cpp_completion
assert_contains auto "${COMPREPLY[@]}"
assert_contains always "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp sync --output j)
COMP_CWORD=3
_onedrive_cpp_completion
assert_contains json "${COMPREPLY[@]}"

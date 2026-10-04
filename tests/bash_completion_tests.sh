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
assert_contains reset-state "${COMPREPLY[@]}"
assert_contains --version "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp sync --)
COMP_CWORD=2
_onedrive_cpp_completion
assert_contains --dry-run "${COMPREPLY[@]}"
assert_contains --output "${COMPREPLY[@]}"
assert_contains --quiet "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp download --)
COMP_CWORD=2
_onedrive_cpp_completion
assert_contains --dry-run "${COMPREPLY[@]}"
assert_contains --config "${COMPREPLY[@]}"

COMP_WORDS=(onedrive-cpp reset-state --)
COMP_CWORD=2
_onedrive_cpp_completion
assert_contains --clear-all "${COMPREPLY[@]}"
assert_contains --yes "${COMPREPLY[@]}"

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

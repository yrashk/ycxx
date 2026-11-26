#!/bin/sh

set -eu

target=$1
case "$target" in
    x86_64-pc-linux-gnu)
        architecture=__x86_64__
        platform=__linux__
        ;;
    arm64-apple-darwin)
        architecture=__aarch64__
        platform=__APPLE__
        ;;
    *) printf 'Unsupported target: %s\n' "$target" >&2; exit 1 ;;
esac

IFS= read -r score < "$2"
case "$score" in
    [0-9]|[1-9][0-9]) ;;
    *) printf '%s\n' 'score.txt must contain a decimal integer from 0 through 99.' >&2; exit 1 ;;
esac

{
    printf '#pragma once\n\n'
    printf 'static_assert(%s == 1 && %s == 1, "Generated for %s");\n' "$architecture" "$platform" "$target"
    printf 'inline constexpr char build_target[] = "%s";\n' "$target"
    printf 'inline constexpr int score = %s;\n' "$score"
} > "$3"

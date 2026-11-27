#!/bin/sh
set -eu

IFS=' ' read -r score < "$1"
case "$score" in
    17|23) ;;
    *)
        printf '%s\n' 'Expected score.txt to contain 17 or 23.' >&2
        exit 1
        ;;
esac

printf '%s\n' \
    '#include "scanner_score.hh"' \
    '' \
    "static_assert(scanner_score == $score);" \
    '' \
    'int main() {' \
    '  return scanner_score;' \
    '}' > "$2"

printf '#pragma once\n\ninline constexpr int scanner_score = %s;\n' "$score" > "$3"

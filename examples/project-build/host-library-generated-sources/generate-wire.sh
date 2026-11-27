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
    '#include "wire_score.hh"' \
    '' \
    "static_assert(wire_seed == $score);" \
    '' \
    'int wire_score() {' \
    '  return wire_seed;' \
    '}' > "$2"

printf '#pragma once\n\ninline constexpr int wire_seed = %s;\n' "$score" > "$3"

#!/bin/sh
set -eu

IFS=' ' read -r score < "$1"
case "$score" in
    17|23|40|45) ;;
    *)
        printf '%s\n' 'Expected a score of 17, 23, 40, or 45.' >&2
        exit 1
        ;;
esac

printf '%s\n' \
    '#include "detail.hh"' \
    '' \
    "static_assert(detail_score == $score);" \
    '' \
    'int implementation_score() {' \
    '  return detail_score;' \
    '}' > "$2"

printf '#pragma once\n\ninline constexpr int detail_score = %s;\n' "$score" > "$3"

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

printf '#pragma once\n\ninline constexpr int library_score = %s;\n' "$score" > "$2"

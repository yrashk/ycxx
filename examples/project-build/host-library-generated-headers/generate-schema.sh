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

printf '#pragma once\n\ninline constexpr int wire_score = %s;\n' "$score" > "$2"

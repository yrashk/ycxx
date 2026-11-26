#!/bin/sh
set -eu

status=0
"$1" || status=$?
case "$status" in
    17|23) ;;
    *)
        printf 'Unexpected scanner status: %s\n' "$status" >&2
        exit 1
        ;;
esac

printf '#pragma once\n\ninline constexpr int generated_score = %s;\n' "$status" > "$2"

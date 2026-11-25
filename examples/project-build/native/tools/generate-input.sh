#!/bin/sh

IFS= read -r value < "$1" || exit 1
case "$value" in
    [0-9]|[1-9][0-9]) ;;
    *) printf '%s\n' 'input.txt must contain a decimal integer from 0 through 99' >&2
       exit 1 ;;
esac

printf '#pragma once\n\ninline constexpr int input_value = %s;\n' "$value" > "$2"

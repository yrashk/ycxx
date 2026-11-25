#!/bin/sh
IFS= read -r evidence < "$1" || exit 2
read -r mode < "$1" || exit 2
printf 'profile=%s\n' "$evidence"
case "$mode" in
    enabled) exit 0 ;;
    disabled)
        printf 'fast mode disabled\n' >&2
        exit 7 ;;
    *) exit 2 ;;
esac

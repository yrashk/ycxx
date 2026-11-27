#!/bin/sh
set -eu

case "$2" in
    x86_64-pc-linux-gnu) format=gnu ;;
    arm64-apple-darwin) format=darwin ;;
    *)
        printf 'Unsupported archive target: %s\n' "$2" >&2
        exit 1
        ;;
esac

exec "$1" --format="$format" rcsD "$4" "$3"

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

printf '#!/bin/sh\nexit %s\n' "$status" > "$2"
"$3" u+x "$2"

#!/bin/sh

# Preserve shader rows and omit comments and blank lines.
while IFS= read -r row || [ -n "$row" ]; do
    leading_space=${row%%[![:space:]]*}
    case ${row#"$leading_space"} in
        ''|\#*) continue ;;
    esac
    printf '%s\n' "$row" || exit 1
done < "$1" > "$2"

#!/usr/bin/env bash

# Verify, or with --write impose, commentflow reflow of the comment blocks.

set -uo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/common.sh" || exit 2

write=0
case "${1:-}" in
    --check) shift ;;
    --write)
        write=1
        shift
        ;;
esac

COMMENTFLOW=${COMMENTFLOW:-commentflow}

files=()
if [ "$#" -gt 0 ]; then
    files=("$@")
else
    require_repo
    collect_files '*.c' '*.h' '*.sh'
    files=(${FILES[@]+"${FILES[@]}"})
fi

[ "${#files[@]}" -gt 0 ] || exit 0
if ! command -v "$COMMENTFLOW" > /dev/null 2>&1; then
    echo "Error: $COMMENTFLOW not found" >&2
    echo "Install it from https://github.com/sysprog21/commentflow" >&2
    exit 2
fi
if [ "$write" -eq 1 ]; then
    exec "$COMMENTFLOW" -- "${files[@]}"
fi

status=0
"$COMMENTFLOW" --check -- "${files[@]}" || status=$?
if [ "$status" -eq 1 ]; then
    echo "Run 'make indent' to reflow comments." >&2
fi
exit "$status"

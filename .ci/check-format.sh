#!/usr/bin/env bash

# Verify, or with --write impose, clang-format conformance for the C sources.

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

CLANG_FORMAT=$(find_clang_format) || {
    echo "Error: clang-format version 20 is required" >&2
    exit 2
}

files=()
if [ "$#" -gt 0 ]; then
    files=("$@")
else
    require_repo
    collect_files '*.c' '*.h'
    files=(${FILES[@]+"${FILES[@]}"})
fi

[ "${#files[@]}" -gt 0 ] || exit 0
if [ "$write" -eq 1 ]; then
    exec "$CLANG_FORMAT" -i "${files[@]}"
fi

# One batched pass answers "is anything unformatted" in a third of the time the
# per-file loop below takes. The loop only has to run when the answer is yes,
# and then only to produce the diff that says what to change.
list=$(mktemp) || exit 2
trap 'rm -f "$list"' EXIT
printf '%s\n' "${files[@]}" > "$list"
"$CLANG_FORMAT" --dry-run -Werror --files="$list" > /dev/null 2>&1 && exit 0

failed=0
expected=$(mktemp) || exit 2
trap 'rm -f "$list" "$expected"' EXIT
for file in "${files[@]}"; do

    # An index entry with no file behind it -- a sparse checkout, or a deletion
    # staged but not yet committed -- is nothing to format.
    [ -f "$file" ] || continue
    if ! "$CLANG_FORMAT" "$file" > "$expected"; then
        echo "Error: $CLANG_FORMAT failed on $file" >&2
        exit 1
    fi
    diff -u -p --label="$file" --label="expected coding style" \
        "$file" "$expected" || failed=1
done

exit "$failed"

#!/usr/bin/env bash

# Ensure every text file uses LF line endings and ends with a newline, which is
# what the [*] section of .editorconfig asks editors to do.

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

files=()
if [ "$#" -gt 0 ]; then
    files=("$@")
else
    require_repo
    collect_files
    files=(${FILES[@]+"${FILES[@]}"})
fi

[ "${#files[@]}" -gt 0 ] || exit 0
if ! command -v file > /dev/null 2>&1; then
    echo "Error: file not found" >&2
    exit 2
fi

# One file(1) run for the whole set: it spends most of its time loading
# libmagic, so paying that once rather than per file is the difference between
# the newline check dominating "make check-style" and disappearing into it.
encodings=()
while IFS= read -r encoding; do
    encodings+=("$encoding")
done < <(file -b --mime-encoding -- "${files[@]}")

# Answers are matched to inputs by position, so a reply that ran short would
# quietly reclassify the rest of the tree as text and report nonsense about it.
if [ "${#encodings[@]}" -ne "${#files[@]}" ]; then
    echo "Error: file described ${#encodings[@]} of ${#files[@]} files" >&2
    exit 2
fi

text=()
for i in "${!files[@]}"; do
    [ "${encodings[i]}" = binary ] && continue
    text+=("${files[i]}")
done
[ "${#text[@]}" -gt 0 ] || exit 0

failed=0
for path in "${text[@]}"; do
    last=$(tail -c1 < "$path") || exit 2
    [ -n "$last" ] || continue
    if [ "$write" -eq 1 ]; then
        printf '\n' >> "$path"
        continue
    fi
    echo "No newline at end of file: $path" >&2
    failed=1
done

# grep exits 0 having found a carriage return, 1 having found none, and 2 on a
# real error, which says nothing about the tree and must not read as clean. Its
# answer goes through a file because the names it prints are NUL separated and a
# command substitution would drop the separators along with them.
matches=$(mktemp) || exit 2
trap 'rm -f "$matches"' EXIT
grep -lZ $'\r' -- "${text[@]}" > "$matches"
status=$?
if [ "$status" -gt 1 ]; then
    echo "Error: grep failed to scan for carriage returns" >&2
    exit 2
fi
while IFS= read -r -d '' path; do

    # Reported even under --write: a carriage return sits inside the text, and
    # stripping one is an edit to content rather than to layout.
    echo "CRLF line ending: $path" >&2
    failed=1
done < "$matches"

exit "$failed"

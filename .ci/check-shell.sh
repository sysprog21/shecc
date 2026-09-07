#!/usr/bin/env bash

# Verify, or with --write impose, shfmt formatting for the shell scripts, and
# lint them with ShellCheck.

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

SHFMT=${SHFMT:-shfmt}
SHELLCHECK=${SHELLCHECK:-shellcheck}

files=()
if [ "$#" -gt 0 ]; then
    files=("$@")
else
    require_repo
    collect_files '*.sh'
    files=(${FILES[@]+"${FILES[@]}"})
fi

[ "${#files[@]}" -gt 0 ] || exit 0
if ! command -v "$SHFMT" > /dev/null 2>&1; then
    echo "Error: $SHFMT not found" >&2
    exit 2
fi
if [ "$write" -eq 1 ]; then
    exec "$SHFMT" -w -- "${files[@]}"
fi

failed=0
"$SHFMT" -d -- "${files[@]}" || failed=1

# The test scripts embed shecc expressions that read as shell and trip
# ShellCheck. Naming what is exempt rather than what is covered keeps a new
# directory linted by default instead of silently skipped.
lint_files=()
for file in "${files[@]}"; do
    case "$file" in
        tests/*) ;;
        *) lint_files+=("$file") ;;
    esac
done

[ "${#lint_files[@]}" -gt 0 ] || exit "$failed"
if ! command -v "$SHELLCHECK" > /dev/null 2>&1; then
    echo "Error: $SHELLCHECK not found" >&2

    # A formatting violation shfmt already found is a verdict on the tree, and
    # outranks the linter that could not run: reporting 2 here would file it
    # under "unavailable" and let the pre-commit hook wave it through.
    [ "$failed" -eq 0 ] || exit "$failed"
    exit 2
fi
"$SHELLCHECK" --severity=warning -- "${lint_files[@]}" || failed=1

exit "$failed"

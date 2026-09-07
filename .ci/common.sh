#!/usr/bin/env bash

# Shared helpers for the style checks. Sourced, not executed.
#
# Every .ci/check-*.sh honors the same exit-code contract, which
# scripts/git-pre-commit.sh depends on to stay advisory: 0 when clean, 1 when
# the tree violates the rule, and 2 when the check could not run at all, which
# in practice means a missing tool. Only 1 blocks a commit.

# Abort unless the current directory sits inside a git repository. Without this
# the checks below find no files and report success, which reads exactly like a
# clean tree.
require_repo()
{
    if ! git rev-parse --git-dir > /dev/null 2>&1; then
        echo "Error: not a git repository" >&2
        exit 2
    fi
}

# Fill the FILES array with every file matching the given pathspecs that git
# would either track or add, so that a new file is checked by the same rule that
# formats it.
#
# The listing goes through a temporary file rather than a process substitution
# because git's exit status has to be read in this shell: inside "< <(...)" a
# failure exits only the subshell, the loop sees end of input, and an
# enumeration that never happened arrives as an empty list, which every checker
# reads as a clean tree.
#
# An index entry can also outlive its file, during a staged deletion or in a
# sparse checkout, and there is nothing for any checker to read at that path.
collect_files()
{
    local listing status file
    listing=$(mktemp) || exit 2
    trap 'rm -f "$listing"' EXIT
    git ls-files -z --cached --others --exclude-standard -- "$@" > "$listing"
    status=$?
    FILES=()
    if [ "$status" -ne 0 ]; then
        echo "Error: could not enumerate the worktree" >&2
        exit 2
    fi
    while IFS= read -r -d '' file; do

        # -f rather than -e: a submodule gitlink and a symlink to a directory
        # are both entries in the index that no file-oriented checker can read.
        [ -f "$file" ] || continue
        FILES+=("$file")
    done < "$listing"
    rm -f "$listing"
    trap - EXIT
}

# Print the name of a clang-format at the version the project pins, or nothing.
# CLANG_FORMAT names a specific binary to use instead of searching.
find_clang_format()
{
    local candidate candidates
    if [ -n "${CLANG_FORMAT:-}" ]; then
        candidates=("$CLANG_FORMAT")
    else
        candidates=(clang-format-20 clang-format)
    fi
    for candidate in "${candidates[@]}"; do
        if command -v "$candidate" > /dev/null 2>&1 \
            && "$candidate" --version 2> /dev/null | grep -qE 'version 20\.'; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

#!/usr/bin/env bash

# Reject a commit whose staged content violates the project's style rules.
#
# Checks run against a snapshot of the index rather than the working tree, so a
# partially staged file is judged on what is actually being committed. A checker
# that cannot run at all, exit 2 and in practice a missing tool, prints a note
# and lets the commit through: the hook is a convenience, CI is the authority.

set -uo pipefail

# git runs hooks from the top of the working tree.
ci_dir=$(git rev-parse --show-toplevel)/.ci || exit 1

staged=()
while IFS= read -r -d '' file; do
    staged+=("$file")
done < <(git diff --cached --name-only -z --diff-filter=ACMRT)

failed=0
run_check()
{ # <what> <command> [file...]
    local what=$1 status=0
    shift
    (cd "$snapshot" && "$@") || status=$?
    case "$status" in
        0) ;;
        1) failed=1 ;;
        2) echo "note: $what unavailable; CI will check it" >&2 ;;

        # Anything else is the checker dying rather than declining, and a signal
        # is not a verdict on the tree: block instead of waving it on.
        *)
            echo "error: $what exited with status $status" >&2
            failed=1
            ;;
    esac
}

if [ "${#staged[@]}" -gt 0 ]; then
    snapshot=$(mktemp -d) || exit 1
    trap 'rm -rf "$snapshot"' EXIT

    # Restyling rules apply to the whole tree, so a staged config change has to
    # drag every file it governs into the snapshot with it.
    style_files=("${staged[@]}")
    if [ -n "$(git diff --cached --name-only -- .clang-format .editorconfig)" ]; then
        style_files=()
        while IFS= read -r -d '' file; do
            style_files+=("$file")
        done < <(git ls-files -z -- '*.c' '*.h' '*.sh')
    fi

    # git ls-files already emits the NUL stream checkout-index wants, and asking
    # git which configs exist beats naming one the index may not hold.
    {
        printf '%s\0' "${staged[@]}" "${style_files[@]}"
        git ls-files -z -- .clang-format .editorconfig
    } | git checkout-index --stdin -z -f --prefix="$snapshot/" || exit 1

    c_files=()
    sh_files=()
    for file in "${style_files[@]}"; do
        case "$file" in
            *.c | *.h) c_files+=("$file") ;;
            *.sh) sh_files+=("$file") ;;
        esac
    done

    run_check "newline check" "$ci_dir/check-newline.sh" "${staged[@]}"
    [ "${#c_files[@]}" -eq 0 ] \
        || run_check "clang-format 20" "$ci_dir/check-format.sh" "${c_files[@]}"
    [ $((${#c_files[@]} + ${#sh_files[@]})) -eq 0 ] \
        || run_check commentflow "$ci_dir/check-commentflow.sh" \
            ${c_files[@]+"${c_files[@]}"} ${sh_files[@]+"${sh_files[@]}"}
    [ "${#sh_files[@]}" -eq 0 ] \
        || run_check "shell tools" "$ci_dir/check-shell.sh" "${sh_files[@]}"
fi

git diff --cached --check || failed=1
exit "$failed"
